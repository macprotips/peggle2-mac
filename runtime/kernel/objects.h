// Kernel objects: handles, dispatcher waits and signaling, APCs and critical sections.
#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include "xbox_types.h"

namespace rt
{
    // Host-side state some handles carry in addition to their guest object (files, enumerators, ...).
    struct HostObject
    {
        virtual ~HostObject() = default;
    };

    // Handles map to a guest object address (dispatcher objects) and/or a host object.
    uint32_t CreateHandle(uint32_t guestObject, std::shared_ptr<HostObject> host = nullptr);
    bool CloseHandle(uint32_t handle);
    uint32_t DuplicateHandle(uint32_t handle);
    uint32_t HandleToObject(uint32_t handle); // 0 if unknown
    std::shared_ptr<HostObject> HandleToHost(uint32_t handle);

    template<typename T>
    std::shared_ptr<T> HandleTo(uint32_t handle) { return std::dynamic_pointer_cast<T>(HandleToHost(handle)); }

    // Allocates and initializes a dispatcher object in the system heap.
    uint32_t CreateDispatcherObject(uint8_t type, uint32_t size, int32_t signalState);

    // Timeout is in 100 ns units (negative relative, positive absolute), nullptr waits forever.
    uint32_t WaitForObjects(std::span<const uint32_t> objects, bool waitAll, const be<int64_t>* timeout, bool alertable);

    // Signal operations. Return the previous signal state.
    int32_t SetEvent(uint32_t event);
    int32_t ResetEvent(uint32_t event);
    int32_t ReleaseSemaphore(uint32_t semaphore, int32_t count, uint32_t* status);
    int32_t ReleaseMutant(uint32_t mutant, uint32_t* status);
    void SignalObject(uint32_t object, int32_t state); // threads, timers
    // A thread exited: mutants it still owns become free and abandoned, as on the console.
    void AbandonMutants(uint32_t thread);

    void EnterCriticalSection(RtlCriticalSection* cs);
    bool TryEnterCriticalSection(RtlCriticalSection* cs);
    void LeaveCriticalSection(RtlCriticalSection* cs);

    // Sleeps the calling guest thread. Returns STATUS_SUCCESS, or STATUS_USER_APC if an APC ran during an
    // alertable delay.
    uint32_t DelayExecution(const be<int64_t>* interval, bool alertable);

    // Queues a user APC for a guest thread and wakes it if it is in an alertable wait.
    struct GuestThread;
    void QueueApc(GuestThread* thread, uint32_t routine, uint32_t a0, uint32_t a1, uint32_t a2);
}
