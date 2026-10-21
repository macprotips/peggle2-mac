// Guest threads on host threads: creation, startup, suspension, and the per-thread guest structures.
#include "thread.h"
#include "kernel/objects.h"
#include <algorithm>
#include <map>
#include <pthread.h>
#include <pthread/qos.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <thread>

namespace rt
{
    static thread_local GuestThread* t_current;
    static std::mutex g_threadsMutex;
    static std::map<uint32_t, GuestThread*> g_threadsByKThread;
    static std::atomic<uint32_t> g_nextThreadId{ 1 };
    static uint32_t g_tlsSlotCount = DynamicTlsSlots;

    // Thrown to unwind a guest thread's host stack when it calls ExTerminateThread.
    struct ThreadExit
    {
        uint32_t code;
    };

    GuestThread* CurrentThread()
    {
        return t_current;
    }

    // Recompiled functions keep guest locals on the host stack, so guest threads get a roomy host stack.
    constexpr size_t HostStackSize = 32 * 1024 * 1024;
    constexpr uint32_t PcrSize = 0x2D8;
    constexpr uint32_t KThreadSize = 0xAB0;

    static void SetupGuestStructures(GuestThread* t, uint32_t stackSize)
    {
        stackSize = (stackSize + 0xFFFF) & ~0xFFFFu;
        t->stackLimit = g_systemHeap.Alloc(stackSize, 0x10000);
        if (!t->stackLimit)
            Fatal("Out of memory allocating a {:X} byte guest stack", stackSize);
        t->stackBase = t->stackLimit + stackSize;

        t->tlsSlots = g_systemHeap.Alloc(std::max(g_tlsSlotCount, DynamicTlsSlots) * 4);
        t->pcr = g_systemHeap.Alloc(PcrSize);
        t->kthread = g_systemHeap.Alloc(KThreadSize);

        // X_KTHREAD: layout and values follow Xenia's XThread::InitializeGuestObject.
        uint8_t* k = Translate(t->kthread);
        auto store32 = [&](uint32_t offset, uint32_t value) { *reinterpret_cast<be<uint32_t>*>(k + offset) = value; };
        auto* header = reinterpret_cast<DispatcherHeader*>(k);
        header->type = TypeThread;
        header->size = uint8_t(KThreadSize / 4); // the 8-bit field cannot hold the KTHREAD size; nothing reads it
        header->signalState = 0;
        store32(0x10, t->kthread + 0x10);
        store32(0x14, t->kthread + 0x10);
        store32(0x5C, t->stackBase);
        store32(0x60, t->stackLimit);
        store32(0x68, t->tlsSlots);
        store32(0xD0, t->stackBase);
        store32(0x14C, t->id);
        store32(0x150, t->start);

        // X_KPCR
        uint8_t* p = Translate(t->pcr);
        *reinterpret_cast<be<uint32_t>*>(p + 0x00) = t->tlsSlots;
        *reinterpret_cast<be<uint32_t>*>(p + 0x30) = t->pcr;
        *reinterpret_cast<be<uint32_t>*>(p + 0x70) = t->stackBase;
        *reinterpret_cast<be<uint32_t>*>(p + 0x74) = t->stackLimit;
        *reinterpret_cast<be<uint32_t>*>(p + 0x100) = t->kthread;
    }

    static void RunGuest(GuestThread* t)
    {
        PPCContext ctx{};
        t->ctx = &ctx;
        t_current = t;
        // Named for profilers and crash reports.
        pthread_setname_np(t->hostBody ? t->name.c_str() : fmt::format("Guest thread {} ({:X})", t->id, t->start).c_str());

        ctx.r1.u64 = t->stackBase - 0x100;
        ctx.r13.u64 = t->pcr;
        ctx.fpscr.loadFromHost();

        {
            std::unique_lock lock(t->suspendMutex);
            t->suspendCv.wait(lock, [&] { return t->suspendCount <= 0; });
        }

        uint32_t exitCode = 0;
        try
        {
            if (t->hostBody)
                t->hostBody();
            else if (t->startup)
                GuestCall(t->startup, { t->start, t->parameter });
            else
                exitCode = uint32_t(GuestCall(t->start, { t->parameter }));
        }
        catch (const ThreadExit& e)
        {
            exitCode = e.code;
        }

        Log("Guest thread {} exited with {:X}", t->id, exitCode);
        t->exited = true;
        AbandonMutants(t->kthread);
        SignalObject(t->kthread, 1);
        // The stack, TLS and KPCR belong to the running thread only; the KTHREAD stays, since handles to
        // the thread outlive it. (Bink starts two threads for every video played.)
        g_systemHeap.Free(t->stackLimit);
        g_systemHeap.Free(t->tlsSlots);
        g_systemHeap.Free(t->pcr);
    }

    static void StartHostThread(GuestThread* t)
    {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, HostStackSize);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        // Game threads run one step below the threads that stand in for console hardware (GPU command
        // processor, vblank, interrupts, audio), so a busy Mac delays the game rather than the "hardware".
        // The title relies on the GPU reading a frame's textures promptly (Bink reuses its buffers a frame
        // later); a starved command processor drew video frames the decoder had already started overwriting.
        pthread_attr_set_qos_class_np(&attr, t->hostBody ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_USER_INITIATED, 0);
        pthread_t host;
        int result = pthread_create(&host, &attr, [](void* arg) -> void* {
            // Precise timers: the console's kernel wakes a timed wait on time, and titles count on it (Bink's
            // sound thread tops up its voice every 13 ms with little to spare; macOS's default coalescing woke
            // it 2-3 ms late and its voice ran dry, clicking). Latency tier 0 asks for that without raising
            // the thread's CPU priority.
            thread_latency_qos_policy_data_t latency = { LATENCY_QOS_TIER_0 };
            thread_policy_set(mach_thread_self(), THREAD_LATENCY_QOS_POLICY, reinterpret_cast<thread_policy_t>(&latency),
                              THREAD_LATENCY_QOS_POLICY_COUNT);
            RunGuest(static_cast<GuestThread*>(arg));
            return nullptr;
        }, t);
        pthread_attr_destroy(&attr);
        if (result != 0)
            Fatal("pthread_create failed: {}", result);
    }

    GuestThread* CreateGuestThread(uint32_t stackSize, uint32_t startup, uint32_t start, uint32_t parameter, uint32_t flags)
    {
        auto* t = new GuestThread;
        t->id = g_nextThreadId++;
        t->startup = startup;
        t->start = start;
        t->parameter = parameter;
        t->suspendCount = (flags & CREATE_SUSPENDED) ? 1 : 0;
        SetupGuestStructures(t, stackSize ? stackSize : 0x10000);
        t->handle = CreateHandle(t->kthread);
        {
            std::lock_guard lock(g_threadsMutex);
            g_threadsByKThread[t->kthread] = t;
        }
        Log("Created guest thread {} start={:X} startup={:X} param={:X} stack={:X}-{:X}{}",
            t->id, start, startup, parameter, t->stackLimit, t->stackBase, t->suspendCount ? " (suspended)" : "");
        StartHostThread(t);
        return t;
    }

    GuestThread* CreateHostThread(const std::string& name, std::function<void()> body)
    {
        auto* t = new GuestThread;
        t->id = g_nextThreadId++;
        t->hostBody = std::move(body);
        t->name = name;
        SetupGuestStructures(t, 0x40000);
        t->handle = CreateHandle(t->kthread);
        {
            std::lock_guard lock(g_threadsMutex);
            g_threadsByKThread[t->kthread] = t;
        }
        Log("Created host thread {} \"{}\"", t->id, name);
        StartHostThread(t);
        return t;
    }

    void StartMainThread(uint32_t entryPoint, uint32_t stackSize, uint32_t tlsSlotCount)
    {
        g_tlsSlotCount = tlsSlotCount ? tlsSlotCount : DynamicTlsSlots;
        GuestThread* main = CreateGuestThread(stackSize, 0, entryPoint, 0, 0);
        uint32_t object = main->kthread;
        std::thread([object] {
            WaitForObjects({ &object, 1 }, false, nullptr, false);
            Log("Main thread returned; exiting");
            exit(0);
        }).detach();
    }

    int ResumeThread(GuestThread* t)
    {
        std::lock_guard lock(t->suspendMutex);
        int previous = t->suspendCount;
        if (t->suspendCount > 0 && --t->suspendCount == 0)
            t->suspendCv.notify_all();
        return previous;
    }

    bool IsThreadRunning(uint32_t id)
    {
        std::lock_guard lock(g_threadsMutex);
        for (auto& [kthread, t] : g_threadsByKThread)
            if (t->id == id)
                return !t->exited && t->waiting.load() == 0 &&
                       std::chrono::steady_clock::now().time_since_epoch().count() - t->lastWaitEnd.load() > 2'000'000;
        return false;
    }

    GuestThread* ThreadFromKThread(uint32_t kthread)
    {
        std::lock_guard lock(g_threadsMutex);
        auto it = g_threadsByKThread.find(kthread);
        return it == g_threadsByKThread.end() ? nullptr : it->second;
    }

    GuestThread* ThreadFromHandle(uint32_t handle)
    {
        if (handle == 0xFFFFFFFE)
            return CurrentThread();
        return ThreadFromKThread(HandleToObject(handle));
    }

    void SetGuestLastError(uint32_t error)
    {
        *Translate<be<uint32_t>>(CurrentThread()->kthread + 0x160) = error;
    }

    void ExitCurrentThread(uint32_t exitCode)
    {
        throw ThreadExit{ exitCode };
    }

    uint64_t GuestCall(uint32_t address, std::initializer_list<uint64_t> args)
    {
        GuestThread* t = CurrentThread();
        PPCContext& ctx = *t->ctx;
        size_t i = 0;
        for (uint64_t arg : args)
            (ctx.*Gprs[i++]).u64 = arg;

        PPCFunc* func = PPC_LOOKUP_FUNC(g_base, address);
        if (!func)
            Fatal("GuestCall to {:X}, which has no recompiled function", address);

        uint64_t savedLr = ctx.lr;
        ctx.lr = 0;
        func(ctx, g_base);
        ctx.lr = savedLr;
        return ctx.r3.u64;
    }
}

void ReportMissingIndirectCall(PPCContext& ctx, uint32_t target)
{
    rt::Fatal("Indirect call to {:X}, which is not a recompiled function (lr={:X}, r3={:X})", target, uint32_t(ctx.lr), ctx.r3.u32);
}
