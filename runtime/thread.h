// Guest threads: one host thread each, with the kernel structures (KTHREAD, PCR, TLS) the title expects.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <functional>
#include <string>
#include "guest.h"

namespace rt
{
    // Slots KeTlsAlloc hands out; each thread's slot array has at least this many.
    constexpr uint32_t DynamicTlsSlots = 64;

    struct GuestThread
    {
        uint32_t id = 0;
        uint32_t handle = 0;
        uint32_t kthread = 0;    // X_KTHREAD, also the thread's dispatcher object
        uint32_t pcr = 0;        // X_KPCR, r13 points here
        uint32_t tlsSlots = 0;   // dynamic TLS slots (KeTlsGetValue), at least DynamicTlsSlots
        uint32_t stackBase = 0;  // high address
        uint32_t stackLimit = 0; // low address

        uint32_t startup = 0;    // XapiThreadStartup trampoline, or 0
        uint32_t start = 0;
        uint32_t parameter = 0;

        std::atomic<int> waiting{ 0 }; // inside a kernel wait or sleep (see WaitScope)
        std::atomic<int64_t> lastWaitEnd{ 0 }; // steady-clock ns when it last left a wait
        std::atomic<bool> exited{ false };
        int32_t basePriority = 0;
        uint32_t affinity = 0xFF;

        std::mutex suspendMutex;
        std::condition_variable suspendCv;
        int suspendCount = 0;

        PPCContext* ctx = nullptr;
        std::function<void()> hostBody; // set for host-driven threads (audio, timers) that call into guest code
        std::string name;               // host-driven threads'

        // User-mode APCs, delivered during alertable waits. Guarded by the dispatcher lock.
        struct Apc
        {
            uint32_t routine;
            uint32_t arguments[3];
        };
        std::deque<Apc> apcs;
    };

    GuestThread* CurrentThread();

    // Marks the current guest thread as blocked in a kernel wait or sleep for its lifetime.
    struct WaitScope
    {
        GuestThread* thread;
        WaitScope() : thread(CurrentThread()) { if (thread) thread->waiting++; }
        ~WaitScope()
        {
            if (!thread)
                return;
            thread->lastWaitEnd = std::chrono::steady_clock::now().time_since_epoch().count();
            thread->waiting--;
        }
    };

    // Whether guest thread `id` exists and is running: not blocked in a kernel wait or sleep, and not in a
    // polling loop that waits in short sleeps (it waited within the last 2 ms).
    bool IsThreadRunning(uint32_t id);

    // Creates and starts (unless suspended) a guest thread. Returns the thread, which stays alive for the process.
    GuestThread* CreateGuestThread(uint32_t stackSize, uint32_t startup, uint32_t start, uint32_t parameter, uint32_t flags);

    // A host thread with full guest thread state (stack, KPCR, TLS), so body() can GuestCall into the title.
    GuestThread* CreateHostThread(const std::string& name, std::function<void()> body);

    // Starts the executable's entry point on a new guest thread. The process exits when it returns.
    void StartMainThread(uint32_t entryPoint, uint32_t stackSize, uint32_t tlsSlotCount);

    int ResumeThread(GuestThread* thread);
    GuestThread* ThreadFromKThread(uint32_t kthread);
    GuestThread* ThreadFromHandle(uint32_t handle);

    // Ends the calling guest thread with the given exit code.
    [[noreturn]] void ExitCurrentThread(uint32_t exitCode);

    // The Win32 last-error value lives in the thread object, where title code reads it directly.
    void SetGuestLastError(uint32_t error);

    constexpr uint32_t CREATE_SUSPENDED = 0x1;
}
