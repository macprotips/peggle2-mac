// Dispatcher objects (events, semaphores, mutants, timers, threads), handles, waits, APCs and
// critical sections.
#include <atomic>
#include "objects.h"
#include "../log.h"
#include "../memory.h"
#include "../thread.h"
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <algorithm>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace rt
{
    // One lock for every dispatcher object; each blocked thread waits on its own condition variable and is
    // woken only by a change to what it waits for (an object, a critical section, an APC). Waking every
    // waiter on every signal kept a dozen idle threads cycling through the lock, about a core of CPU.
    // Note: waiters are a flat list scanned per signal; fine for the title's few dozen threads.
    static std::mutex g_dispatcherMutex;
    static std::vector<uint32_t> g_mutants; // every mutant created (guarded by g_dispatcherMutex)

    struct Waiter
    {
        std::condition_variable cv;
        std::span<const uint32_t> objects;
        RtlCriticalSection* section = nullptr;
        GuestThread* thread = nullptr;
    };
    static std::vector<Waiter*> g_waiters; // guarded by g_dispatcherMutex

    // Registers the current thread as blocked for its lifetime. Constructed and destroyed under the lock.
    struct WaiterScope
    {
        Waiter waiter;
        WaiterScope(std::span<const uint32_t> objects, RtlCriticalSection* section = nullptr)
        {
            waiter.objects = objects;
            waiter.section = section;
            waiter.thread = CurrentThread();
            g_waiters.push_back(&waiter);
        }
        ~WaiterScope() { std::erase(g_waiters, &waiter); }
    };

    // Caller holds g_dispatcherMutex.
    static void WakeObject(uint32_t object)
    {
        for (Waiter* w : g_waiters)
            if (std::find(w->objects.begin(), w->objects.end(), object) != w->objects.end())
                w->cv.notify_one();
    }

    static void WakeAll()
    {
        for (Waiter* w : g_waiters)
            w->cv.notify_one();
    }

    struct HandleEntry
    {
        uint32_t object;
        std::shared_ptr<HostObject> host;
    };

    static std::mutex g_handlesMutex;
    static std::map<uint32_t, HandleEntry> g_handles;
    static uint32_t g_nextHandle = 0xF8000004;

    uint32_t CreateHandle(uint32_t guestObject, std::shared_ptr<HostObject> host)
    {
        std::lock_guard lock(g_handlesMutex);
        uint32_t handle = g_nextHandle;
        g_nextHandle += 4;
        g_handles[handle] = { guestObject, std::move(host) };
        return handle;
    }

    bool CloseHandle(uint32_t handle)
    {
        std::lock_guard lock(g_handlesMutex);
        return g_handles.erase(handle) != 0;
    }

    uint32_t DuplicateHandle(uint32_t handle)
    {
        std::unique_lock lock(g_handlesMutex);
        auto it = g_handles.find(handle);
        if (it == g_handles.end())
            return 0;
        HandleEntry entry = it->second;
        lock.unlock();
        return CreateHandle(entry.object, entry.host);
    }

    uint32_t HandleToObject(uint32_t handle)
    {
        if (handle == 0xFFFFFFFE)
            return CurrentThread()->kthread;
        std::lock_guard lock(g_handlesMutex);
        auto it = g_handles.find(handle);
        return it == g_handles.end() ? 0 : it->second.object;
    }

    std::shared_ptr<HostObject> HandleToHost(uint32_t handle)
    {
        std::lock_guard lock(g_handlesMutex);
        auto it = g_handles.find(handle);
        return it == g_handles.end() ? nullptr : it->second.host;
    }

    uint32_t CreateDispatcherObject(uint8_t type, uint32_t size, int32_t signalState)
    {
        uint32_t object = g_systemHeap.Alloc(size);
        auto* header = Translate<DispatcherHeader>(object);
        header->type = type;
        header->size = uint8_t(size / 4);
        header->signalState = signalState;
        if (type == TypeMutant)
        {
            std::lock_guard lock(g_dispatcherMutex);
            g_mutants.push_back(object);
        }
        return object;
    }

    void AbandonMutants(uint32_t thread)
    {
        std::lock_guard lock(g_dispatcherMutex);
        bool any = false;
        for (uint32_t object : g_mutants)
        {
            auto* mutant = Translate<KMutant>(object);
            if (mutant->ownerThread != thread)
                continue;
            mutant->header.signalState = 1;
            mutant->ownerThread = 0;
            mutant->abandoned = 1;
            any = true;
        }
        if (any)
            WakeAll();
    }

    static uint32_t CurrentKThread()
    {
        GuestThread* t = CurrentThread();
        return t ? t->kthread : 0;
    }

    // Caller holds g_dispatcherMutex.
    static bool IsSignaled(uint32_t object, uint32_t thread)
    {
        auto* header = Translate<DispatcherHeader>(object);
        if (header->type == TypeMutant)
        {
            auto* mutant = reinterpret_cast<KMutant*>(header);
            return header->signalState > 0 || mutant->ownerThread == thread;
        }
        return header->signalState > 0;
    }

    // Caller holds g_dispatcherMutex. Applies the side effects of a satisfied wait; true if it took a mutant
    // its owner abandoned (the wait then reports STATUS_ABANDONED_WAIT_0 + index).
    static bool Acquire(uint32_t object, uint32_t thread)
    {
        auto* header = Translate<DispatcherHeader>(object);
        switch (header->type)
        {
        case TypeSynchronizationEvent:
        case TypeSynchronizationTimer:
            header->signalState = 0;
            break;
        case TypeSemaphore:
            header->signalState = header->signalState - 1;
            break;
        case TypeMutant:
        {
            auto* mutant = reinterpret_cast<KMutant*>(header);
            header->signalState = header->signalState - 1;
            mutant->ownerThread = thread;
            const bool abandoned = mutant->abandoned;
            mutant->abandoned = 0;
            return abandoned;
        }
        default:
            break;
        }
        return false;
    }

    int64_t RelativeTimeout100ns(int64_t value)
    {
        // Negative is relative, positive absolute, both in 100 ns units. Titles pass huge values to mean
        // "practically forever"; clamp so converting to nanoseconds cannot overflow into an invalid deadline.
        constexpr int64_t Max = 30LL * 24 * 3600 * 10000000; // 30 days
        int64_t relative = value < 0 ? (value == INT64_MIN ? Max : -value) : std::max<int64_t>(0, value - int64_t(GuestSystemTime()));
        static std::atomic<bool> reported;
        if (relative > Max && !reported.exchange(true))
            Log("Kernel timeout {:X} clamped to 30 days", uint64_t(value));
        return std::min(relative, Max);
    }

    static std::optional<std::chrono::steady_clock::time_point> Deadline(const be<int64_t>* timeout)
    {
        if (!timeout)
            return std::nullopt;
        int64_t relative100ns = RelativeTimeout100ns(*timeout);
        return std::chrono::steady_clock::now() + std::chrono::nanoseconds(relative100ns * 100);
    }

    void QueueApc(GuestThread* thread, uint32_t routine, uint32_t a0, uint32_t a1, uint32_t a2)
    {
        std::lock_guard lock(g_dispatcherMutex);
        thread->apcs.push_back({ routine, { a0, a1, a2 } });
        for (Waiter* w : g_waiters)
            if (w->thread == thread)
                w->cv.notify_one();
    }

    // Caller holds the lock; it is released while the APCs run in guest code.
    static void DeliverApcs(std::unique_lock<std::mutex>& lock)
    {
        GuestThread* self = CurrentThread();
        while (!self->apcs.empty())
        {
            GuestThread::Apc apc = self->apcs.front();
            self->apcs.pop_front();
            lock.unlock();
            GuestCall(apc.routine, { apc.arguments[0], apc.arguments[1], apc.arguments[2] });
            lock.lock();
        }
    }

    uint32_t WaitForObjects(std::span<const uint32_t> objects, bool waitAll, const be<int64_t>* timeout, bool alertable)
    {
        const uint32_t thread = CurrentKThread();
        const auto deadline = Deadline(timeout);
        GuestThread* self = CurrentThread();
        WaitScope waiting;

        std::unique_lock lock(g_dispatcherMutex);
        WaiterScope scope(objects);
        while (true)
        {
            if (alertable && !self->apcs.empty())
            {
                DeliverApcs(lock);
                return STATUS_USER_APC;
            }
            if (waitAll)
            {
                bool all = true;
                for (uint32_t object : objects)
                    all = all && IsSignaled(object, thread);
                if (all)
                {
                    bool abandoned = false;
                    for (uint32_t object : objects)
                        abandoned |= Acquire(object, thread);
                    return abandoned ? STATUS_ABANDONED_WAIT_0 : STATUS_SUCCESS;
                }
            }
            else
            {
                for (size_t i = 0; i < objects.size(); i++)
                {
                    if (IsSignaled(objects[i], thread))
                        return (Acquire(objects[i], thread) ? STATUS_ABANDONED_WAIT_0 : STATUS_SUCCESS) + uint32_t(i);
                }
            }

            if (deadline)
            {
                if (scope.waiter.cv.wait_until(lock, *deadline) == std::cv_status::timeout && std::chrono::steady_clock::now() >= *deadline)
                {
                    // One last check: the object may have been signaled as the deadline passed.
                    for (size_t i = 0; !waitAll && i < objects.size(); i++)
                    {
                        if (IsSignaled(objects[i], thread))
                            return (Acquire(objects[i], thread) ? STATUS_ABANDONED_WAIT_0 : STATUS_SUCCESS) + uint32_t(i);
                    }
                    return STATUS_TIMEOUT;
                }
            }
            else
            {
                scope.waiter.cv.wait(lock);
            }
        }
    }

    int32_t SetEvent(uint32_t event)
    {
        std::lock_guard lock(g_dispatcherMutex);
        auto* header = Translate<DispatcherHeader>(event);
        int32_t previous = header->signalState;
        header->signalState = 1;
        WakeObject(event);
        return previous;
    }

    int32_t ResetEvent(uint32_t event)
    {
        std::lock_guard lock(g_dispatcherMutex);
        auto* header = Translate<DispatcherHeader>(event);
        int32_t previous = header->signalState;
        header->signalState = 0;
        return previous;
    }

    int32_t ReleaseSemaphore(uint32_t semaphore, int32_t count, uint32_t* status)
    {
        std::lock_guard lock(g_dispatcherMutex);
        auto* s = Translate<KSemaphore>(semaphore);
        int32_t previous = s->header.signalState;
        if (int64_t(previous) + count > s->limit)
        {
            *status = STATUS_SEMAPHORE_LIMIT_EXCEEDED;
            return previous;
        }
        s->header.signalState = previous + count;
        WakeObject(semaphore);
        *status = STATUS_SUCCESS;
        return previous;
    }

    int32_t ReleaseMutant(uint32_t mutant, uint32_t* status)
    {
        std::lock_guard lock(g_dispatcherMutex);
        auto* m = Translate<KMutant>(mutant);
        int32_t previous = m->header.signalState;
        if (m->ownerThread != CurrentKThread())
        {
            *status = STATUS_MUTANT_NOT_OWNED;
            return previous;
        }
        m->header.signalState = previous + 1;
        if (m->header.signalState > 0)
            m->ownerThread = 0;
        WakeObject(mutant);
        *status = STATUS_SUCCESS;
        return previous;
    }

    void SignalObject(uint32_t object, int32_t state)
    {
        std::lock_guard lock(g_dispatcherMutex);
        Translate<DispatcherHeader>(object)->signalState = state;
        WakeObject(object);
    }

    // Critical sections take no host lock unless contended: the owner word is claimed with a CAS, like the
    // kernel's interlocked fast path, spinning briefly before blocking until the owner leaves. Fields stay
    // big-endian in guest memory, so the CAS works on raw words.
    static std::atomic<int> g_csWaiters;

    static std::atomic_ref<uint32_t> OwnerWord(RtlCriticalSection* cs)
    {
        return std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(&cs->owningThread));
    }

    static void AddLockCount(RtlCriticalSection* cs, int32_t delta)
    {
        // Bookkeeping only (titles do not depend on it); kept consistent with an atomic read-modify-write.
        std::atomic_ref<uint32_t> word(*reinterpret_cast<uint32_t*>(&cs->lockCount));
        uint32_t raw = word.load(std::memory_order_relaxed);
        while (!word.compare_exchange_weak(raw, __builtin_bswap32(uint32_t(int32_t(__builtin_bswap32(raw)) + delta)),
                                           std::memory_order_relaxed))
        {
        }
    }

    static bool TryClaim(RtlCriticalSection* cs, uint32_t selfRaw)
    {
        auto owner = OwnerWord(cs);
        uint32_t current = owner.load(std::memory_order_relaxed);
        if (current == selfRaw)
        {
            cs->recursionCount = cs->recursionCount + 1; // only the owner touches it
            AddLockCount(cs, 1);
            return true;
        }
        uint32_t expected = 0;
        if (current == 0 && owner.compare_exchange_strong(expected, selfRaw, std::memory_order_acquire))
        {
            cs->recursionCount = 1;
            AddLockCount(cs, 1);
            return true;
        }
        return false;
    }

    bool TryEnterCriticalSection(RtlCriticalSection* cs)
    {
        if (TryClaim(cs, __builtin_bswap32(CurrentKThread())))
            return true;
        // A thread that keeps failing to take one section (a polling loop) is reported once with the owner,
        // like a long EnterCriticalSection wait.
        thread_local RtlCriticalSection* failing;
        thread_local std::chrono::steady_clock::time_point since;
        thread_local bool reported;
        const auto now = std::chrono::steady_clock::now();
        if (cs != failing)
        {
            failing = cs;
            since = now;
            reported = false;
        }
        else if (!reported && now - since > std::chrono::seconds(5))
        {
            reported = true;
            GuestThread* owner = ThreadFromKThread(cs->owningThread);
            GuestThread* self = CurrentThread();
            Log("Critical section {:X} held by thread {} (recursion {}) for over 5 s; thread {} keeps trying to enter it", ToGuest(cs),
                owner ? owner->id : 0, int32_t(cs->recursionCount), self ? self->id : 0);
        }
        return false;
    }

    void EnterCriticalSection(RtlCriticalSection* cs)
    {
        const uint32_t selfRaw = __builtin_bswap32(CurrentKThread());
        for (int i = 0; i < 256; i++)
        {
            if (TryClaim(cs, selfRaw))
                return;
            if (i >= 64)
                std::this_thread::yield();
        }

        WaitScope waiting;
        std::unique_lock lock(g_dispatcherMutex);
        WaiterScope scope({}, cs);
        g_csWaiters.fetch_add(1);
        // Pairs with LeaveCriticalSection: the claim below must not read the owner word before this waiter is
        // counted, or both sides could miss each other (each seeing the other's old value).
        std::atomic_thread_fence(std::memory_order_seq_cst);
        // Long waits are reported once with the owner, which makes guest deadlocks diagnosable.
        bool reported = false;
        while (!TryClaim(cs, selfRaw))
        {
            if (scope.waiter.cv.wait_for(lock, std::chrono::seconds(3)) == std::cv_status::timeout && !reported)
            {
                reported = true;
                GuestThread* owner = ThreadFromKThread(cs->owningThread);
                Log("Critical section {:X} held by thread {} for over 3 s; waiter is thread {}", ToGuest(cs),
                    owner ? owner->id : 0, CurrentThread()->id);
                MaybeLogBacktrace("CriticalSectionWait");
            }
        }
        g_csWaiters.fetch_sub(1);
    }

    void LeaveCriticalSection(RtlCriticalSection* cs)
    {
        AddLockCount(cs, -1);
        if (cs->recursionCount > 1)
        {
            cs->recursionCount = cs->recursionCount - 1;
            return;
        }
        cs->recursionCount = 0;
        OwnerWord(cs).store(0, std::memory_order_seq_cst);
        // The release and the waiter count are both sequentially consistent, and a waiter fences between counting
        // itself and retrying the claim, so either it sees the released owner or this sees it waiting.
        if (g_csWaiters.load(std::memory_order_seq_cst) > 0)
        {
            std::lock_guard lock(g_dispatcherMutex);
            for (Waiter* w : g_waiters)
                if (w->section == cs)
                    w->cv.notify_one();
        }
    }

    uint32_t DelayExecution(const be<int64_t>* interval, bool alertable)
    {
        const auto deadline = Deadline(interval);
        WaitScope waiting;
        if (!alertable)
        {
            if (!deadline || *deadline <= std::chrono::steady_clock::now())
                std::this_thread::yield();
            else
                std::this_thread::sleep_until(*deadline);
            return STATUS_SUCCESS;
        }

        GuestThread* self = CurrentThread();
        std::unique_lock lock(g_dispatcherMutex);
        WaiterScope scope({});
        while (true)
        {
            if (!self->apcs.empty())
            {
                DeliverApcs(lock);
                return STATUS_USER_APC;
            }
            if (!deadline)
                scope.waiter.cv.wait(lock);
            else if (scope.waiter.cv.wait_until(lock, *deadline) == std::cv_status::timeout)
                return STATUS_SUCCESS;
        }
    }

    uint64_t GuestSystemTime()
    {
        // Seconds between 1601-01-01 and 1970-01-01.
        constexpr uint64_t EpochDelta = 11644473600ull;
        auto now = std::chrono::system_clock::now().time_since_epoch();
        return (EpochDelta * 10000000ull) + uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() / 100);
    }
}
