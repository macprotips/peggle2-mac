// Thread, TLS, spin lock and critical section imports.
#include "../thread.h"
#include "objects.h"
#include <bitset>
#include <thread>

namespace rt
{
    // --- Threads -------------------------------------------------------------------------------

    static uint32_t ExCreateThread(be<uint32_t>* handle, uint32_t stackSize, be<uint32_t>* threadId, uint32_t startup, uint32_t start, uint32_t parameter, uint32_t flags)
    {
        GuestThread* t = CreateGuestThread(stackSize, startup, start, parameter, flags);
        if (handle)
            *handle = t->handle;
        if (threadId)
            *threadId = t->id;
        return STATUS_SUCCESS;
    }

    static void ExTerminateThread(uint32_t exitCode)
    {
        ExitCurrentThread(exitCode);
    }

    static uint32_t NtResumeThread(uint32_t handle, be<uint32_t>* previous)
    {
        GuestThread* t = ThreadFromHandle(handle);
        if (!t)
            return STATUS_INVALID_HANDLE;
        int count = ResumeThread(t);
        if (previous)
            *previous = count;
        return STATUS_SUCCESS;
    }

    static int32_t KeSetBasePriorityThread(uint32_t kthread, int32_t priority)
    {
        GuestThread* t = ThreadFromKThread(kthread);
        if (!t)
            return 0;
        int32_t previous = t->basePriority;
        t->basePriority = priority;
        return previous;
    }

    static int32_t KeQueryBasePriorityThread(uint32_t kthread)
    {
        GuestThread* t = ThreadFromKThread(kthread);
        return t ? t->basePriority : 0;
    }

    static uint32_t KeSetAffinityThread(uint32_t kthread, uint32_t affinity, be<uint32_t>* previous)
    {
        // Affinity is advisory on the host; the scheduler places threads.
        GuestThread* t = ThreadFromKThread(kthread);
        if (t)
        {
            if (previous)
                *previous = t->affinity;
            t->affinity = affinity;
        }
        return STATUS_SUCCESS;
    }

    static uint32_t ObLookupThreadByThreadId(uint32_t, be<uint32_t>* out)
    {
        // Note: only the calling thread is resolvable by id; add an id map if the game queries others.
        *out = CurrentThread()->kthread;
        return STATUS_SUCCESS;
    }

    static uint32_t KeDelayExecutionThread(uint32_t, uint32_t alertable, be<int64_t>* interval)
    {
        return DelayExecution(interval, alertable != 0);
    }

    static uint32_t KeGetCurrentProcessType()
    {
        return 1; // title process
    }

    static void KeSetCurrentProcessType(uint32_t)
    {
    }

    static void KeEnterCriticalRegion()
    {
    }

    static void KeLeaveCriticalRegion()
    {
    }

    // --- TLS -----------------------------------------------------------------------------------

    static std::mutex g_tlsMutex;
    static std::bitset<DynamicTlsSlots> g_tlsUsed;

    static uint32_t KeTlsAlloc()
    {
        std::lock_guard lock(g_tlsMutex);
        for (uint32_t i = 0; i < g_tlsUsed.size(); i++)
        {
            if (!g_tlsUsed[i])
            {
                g_tlsUsed[i] = true;
                return i;
            }
        }
        return 0xFFFFFFFF;
    }

    static uint32_t KeTlsFree(uint32_t slot)
    {
        std::lock_guard lock(g_tlsMutex);
        if (slot >= g_tlsUsed.size())
            return 0;
        g_tlsUsed[slot] = false;
        return 1;
    }

    static uint32_t KeTlsGetValue(uint32_t slot)
    {
        return slot < DynamicTlsSlots ? uint32_t(Translate<be<uint32_t>>(CurrentThread()->tlsSlots)[slot]) : 0;
    }

    static uint32_t KeTlsSetValue(uint32_t slot, uint32_t value)
    {
        if (slot >= DynamicTlsSlots)
            return 0;
        Translate<be<uint32_t>>(CurrentThread()->tlsSlots)[slot] = value;
        return 1;
    }

    // --- Events, semaphores, mutants -----------------------------------------------------------

    static uint32_t NtCreateEvent(be<uint32_t>* handle, uint32_t, uint32_t eventType, uint32_t initialState)
    {
        uint32_t object = CreateDispatcherObject(eventType ? TypeSynchronizationEvent : TypeNotificationEvent, 0x10, initialState ? 1 : 0);
        *handle = CreateHandle(object);
        return STATUS_SUCCESS;
    }

    static uint32_t NtSetEvent(uint32_t handle, be<int32_t>* previous)
    {
        uint32_t object = HandleToObject(handle);
        if (!object)
            return STATUS_INVALID_HANDLE;
        int32_t state = SetEvent(object);
        if (previous)
            *previous = state;
        return STATUS_SUCCESS;
    }

    static uint32_t NtClearEvent(uint32_t handle)
    {
        uint32_t object = HandleToObject(handle);
        if (!object)
            return STATUS_INVALID_HANDLE;
        ResetEvent(object);
        return STATUS_SUCCESS;
    }

    static int32_t KeSetEvent(uint32_t event, uint32_t, uint32_t)
    {
        return SetEvent(event);
    }

    static int32_t KeResetEvent(uint32_t event)
    {
        return ResetEvent(event);
    }

    static uint32_t NtCreateSemaphore(be<uint32_t>* handle, uint32_t, int32_t count, int32_t limit)
    {
        uint32_t object = CreateDispatcherObject(TypeSemaphore, sizeof(KSemaphore), count);
        Translate<KSemaphore>(object)->limit = limit;
        *handle = CreateHandle(object);
        return STATUS_SUCCESS;
    }

    static uint32_t NtReleaseSemaphore(uint32_t handle, int32_t count, be<int32_t>* previous)
    {
        uint32_t object = HandleToObject(handle);
        if (!object)
            return STATUS_INVALID_HANDLE;
        uint32_t status;
        int32_t state = ReleaseSemaphore(object, count, &status);
        if (previous)
            *previous = state;
        return status;
    }

    static uint32_t NtCreateMutant(be<uint32_t>* handle, uint32_t, uint32_t initialOwner)
    {
        uint32_t object = CreateDispatcherObject(TypeMutant, sizeof(KMutant), initialOwner ? 0 : 1);
        if (initialOwner)
            Translate<KMutant>(object)->ownerThread = CurrentThread()->kthread;
        *handle = CreateHandle(object);
        return STATUS_SUCCESS;
    }

    static uint32_t NtReleaseMutant(uint32_t handle, be<int32_t>* previous)
    {
        uint32_t object = HandleToObject(handle);
        if (!object)
            return STATUS_INVALID_HANDLE;
        uint32_t status;
        int32_t state = ReleaseMutant(object, &status);
        if (previous)
            *previous = state;
        return status;
    }

    // --- Waits ---------------------------------------------------------------------------------

    static uint32_t KeWaitForSingleObject(uint32_t object, uint32_t, uint32_t, uint32_t alertable, be<int64_t>* timeout)
    {
        return WaitForObjects({ &object, 1 }, false, timeout, alertable);
    }

    static uint32_t KeWaitForMultipleObjects(uint32_t count, be<uint32_t>* objects, uint32_t waitType, uint32_t, uint32_t, uint32_t alertable, be<int64_t>* timeout)
    {
        std::vector<uint32_t> list(count);
        for (uint32_t i = 0; i < count; i++)
            list[i] = objects[i];
        return WaitForObjects(list, waitType == 0, timeout, alertable);
    }

    static uint32_t NtWaitForSingleObjectEx(uint32_t handle, uint32_t, uint32_t alertable, be<int64_t>* timeout)
    {
        uint32_t object = HandleToObject(handle);
        if (!object)
            return STATUS_INVALID_HANDLE;
        return WaitForObjects({ &object, 1 }, false, timeout, alertable);
    }

    static uint32_t NtWaitForMultipleObjectsEx(uint32_t count, be<uint32_t>* handles, uint32_t waitType, uint32_t, uint32_t alertable, be<int64_t>* timeout)
    {
        std::vector<uint32_t> list(count);
        for (uint32_t i = 0; i < count; i++)
        {
            list[i] = HandleToObject(handles[i]);
            if (!list[i])
                return STATUS_INVALID_HANDLE;
        }
        return WaitForObjects(list, waitType == 0, timeout, alertable);
    }

    // --- Handles and objects -------------------------------------------------------------------

    static uint32_t NtClose(uint32_t handle)
    {
        return CloseHandle(handle) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
    }

    static uint32_t NtDuplicateObject(uint32_t handle, be<uint32_t>* out, uint32_t options)
    {
        constexpr uint32_t DUPLICATE_CLOSE_SOURCE = 1;
        uint32_t duplicate = handle == 0xFFFFFFFE ? CreateHandle(CurrentThread()->kthread) : DuplicateHandle(handle);
        if (!duplicate)
            return STATUS_INVALID_HANDLE;
        *out = duplicate;
        if (options & DUPLICATE_CLOSE_SOURCE)
            CloseHandle(handle);
        return STATUS_SUCCESS;
    }

    static uint32_t ObReferenceObjectByHandle(uint32_t handle, uint32_t, be<uint32_t>* out)
    {
        uint32_t object = HandleToObject(handle);
        if (!object)
            return STATUS_INVALID_HANDLE;
        *out = object;
        return STATUS_SUCCESS;
    }

    static void ObDereferenceObject(uint32_t)
    {
        // Objects live for the process; reference counts are not tracked.
    }

    static uint32_t ObOpenObjectByPointer(uint32_t object, uint32_t, be<uint32_t>* handle)
    {
        *handle = CreateHandle(object);
        return STATUS_SUCCESS;
    }

    // --- Critical sections and spin locks ------------------------------------------------------

    static void RtlInitializeCriticalSection(RtlCriticalSection* cs)
    {
        cs->header.type = TypeSynchronizationEvent;
        cs->header.absolute = 0;
        cs->lockCount = -1;
        cs->recursionCount = 0;
        cs->owningThread = 0;
    }

    static uint32_t RtlInitializeCriticalSectionAndSpinCount(RtlCriticalSection* cs, uint32_t spinCount)
    {
        RtlInitializeCriticalSection(cs);
        cs->header.absolute = uint8_t((spinCount + 255) >> 8);
        return STATUS_SUCCESS;
    }

    static void RtlEnterCriticalSection(RtlCriticalSection* cs)
    {
        EnterCriticalSection(cs);
    }

    static uint32_t RtlTryEnterCriticalSection(RtlCriticalSection* cs)
    {
        return TryEnterCriticalSection(cs) ? 1 : 0;
    }

    static void RtlLeaveCriticalSection(RtlCriticalSection* cs)
    {
        LeaveCriticalSection(cs);
    }

    // Spin locks hold the owning KTHREAD so a recursive acquire is detectable while debugging.
    static void AcquireSpinLock(be<uint32_t>* lock)
    {
        auto* word = reinterpret_cast<std::atomic<uint32_t>*>(lock);
        const uint32_t self = __builtin_bswap32(CurrentThread()->kthread);
        uint32_t expected = 0;
        const auto start = std::chrono::steady_clock::now();
        bool reported = false;
        while (!word->compare_exchange_weak(expected, self, std::memory_order_acquire))
        {
            if (!reported && std::chrono::steady_clock::now() - start > std::chrono::seconds(5))
            {
                reported = true;
                Log("Spin lock {:X} held by KTHREAD {:X} for over 5 s", ToGuest(lock), __builtin_bswap32(expected));
            }
            expected = 0;
            std::this_thread::yield();
        }
    }

    static void ReleaseSpinLock(be<uint32_t>* lock)
    {
        reinterpret_cast<std::atomic<uint32_t>*>(lock)->store(0, std::memory_order_release);
    }

    static uint32_t KfAcquireSpinLock(be<uint32_t>* lock)
    {
        AcquireSpinLock(lock);
        return 0; // previous IRQL
    }

    static void KfReleaseSpinLock(be<uint32_t>* lock, uint32_t)
    {
        ReleaseSpinLock(lock);
    }

    static void KeAcquireSpinLockAtRaisedIrql(be<uint32_t>* lock)
    {
        AcquireSpinLock(lock);
    }

    static void KeReleaseSpinLockFromRaisedIrql(be<uint32_t>* lock)
    {
        ReleaseSpinLock(lock);
    }

    // --- Time ----------------------------------------------------------------------------------

    static uint64_t KeQueryPerformanceFrequency()
    {
        // mftb is recompiled to read cntvct_el0, so performance counters tick at cntfrq_el0.
        uint64_t frequency;
        asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
        return frequency;
    }

    static void KeQuerySystemTime(be<uint64_t>* time)
    {
        *time = GuestSystemTime();
    }
}

using namespace rt;
GUEST_IMPORT(ExCreateThread, ExCreateThread)
GUEST_IMPORT(ExTerminateThread, ExTerminateThread)
GUEST_IMPORT(NtResumeThread, NtResumeThread)
GUEST_IMPORT(KeSetBasePriorityThread, KeSetBasePriorityThread)
GUEST_IMPORT(KeQueryBasePriorityThread, KeQueryBasePriorityThread)
GUEST_IMPORT(KeSetAffinityThread, KeSetAffinityThread)
GUEST_IMPORT(ObLookupThreadByThreadId, ObLookupThreadByThreadId)
GUEST_IMPORT(KeDelayExecutionThread, KeDelayExecutionThread)
GUEST_IMPORT(KeGetCurrentProcessType, KeGetCurrentProcessType)
GUEST_IMPORT(KeSetCurrentProcessType, KeSetCurrentProcessType)
GUEST_IMPORT(KeEnterCriticalRegion, KeEnterCriticalRegion)
GUEST_IMPORT(KeLeaveCriticalRegion, KeLeaveCriticalRegion)
GUEST_IMPORT(KeTlsAlloc, KeTlsAlloc)
GUEST_IMPORT(KeTlsFree, KeTlsFree)
GUEST_IMPORT(KeTlsGetValue, KeTlsGetValue)
GUEST_IMPORT(KeTlsSetValue, KeTlsSetValue)
GUEST_IMPORT(NtCreateEvent, NtCreateEvent)
GUEST_IMPORT(NtSetEvent, NtSetEvent)
GUEST_IMPORT(NtClearEvent, NtClearEvent)
GUEST_IMPORT(KeSetEvent, KeSetEvent)
GUEST_IMPORT(KeResetEvent, KeResetEvent)
GUEST_IMPORT(NtCreateSemaphore, NtCreateSemaphore)
GUEST_IMPORT(NtReleaseSemaphore, NtReleaseSemaphore)
GUEST_IMPORT(NtCreateMutant, NtCreateMutant)
GUEST_IMPORT(NtReleaseMutant, NtReleaseMutant)
GUEST_IMPORT(KeWaitForSingleObject, KeWaitForSingleObject)
GUEST_IMPORT(KeWaitForMultipleObjects, KeWaitForMultipleObjects)
GUEST_IMPORT(NtWaitForSingleObjectEx, NtWaitForSingleObjectEx)
GUEST_IMPORT(NtWaitForMultipleObjectsEx, NtWaitForMultipleObjectsEx)
GUEST_IMPORT(NtClose, NtClose)
GUEST_IMPORT(NtDuplicateObject, NtDuplicateObject)
GUEST_IMPORT(ObReferenceObjectByHandle, ObReferenceObjectByHandle)
GUEST_IMPORT(ObDereferenceObject, ObDereferenceObject)
GUEST_IMPORT(ObOpenObjectByPointer, ObOpenObjectByPointer)
GUEST_IMPORT(RtlInitializeCriticalSection, RtlInitializeCriticalSection)
GUEST_IMPORT(RtlInitializeCriticalSectionAndSpinCount, RtlInitializeCriticalSectionAndSpinCount)
GUEST_IMPORT(RtlEnterCriticalSection, RtlEnterCriticalSection)
GUEST_IMPORT(RtlTryEnterCriticalSection, RtlTryEnterCriticalSection)
GUEST_IMPORT(RtlLeaveCriticalSection, RtlLeaveCriticalSection)
GUEST_IMPORT(KfAcquireSpinLock, KfAcquireSpinLock)
GUEST_IMPORT(KfReleaseSpinLock, KfReleaseSpinLock)
GUEST_IMPORT(KeAcquireSpinLockAtRaisedIrql, KeAcquireSpinLockAtRaisedIrql)
GUEST_IMPORT(KeReleaseSpinLockFromRaisedIrql, KeReleaseSpinLockFromRaisedIrql)
GUEST_IMPORT(KeQueryPerformanceFrequency, KeQueryPerformanceFrequency)
GUEST_IMPORT(KeQuerySystemTime, KeQuerySystemTime)
