// Guest memory: address translation, the guest heaps, and write watches for the texture cache.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>
#include <mutex>

namespace rt
{
    // Guest addresses are 32-bit offsets into one 4 GB host reservation.
    extern uint8_t* g_base;

    inline uint8_t* Translate(uint32_t address) { return g_base + address; }

    template<typename T>
    inline T* Translate(uint32_t address) { return address ? reinterpret_cast<T*>(g_base + address) : nullptr; }

    inline uint32_t ToGuest(const void* host) { return host ? uint32_t(static_cast<const uint8_t*>(host) - g_base) : 0; }

    void InitMemory();

    // First-fit allocator over a fixed guest address range. Allocations are zeroed.
    // Note: linear scan of the free list; switch to a size-bucketed index if it shows up in profiles.
    class GuestHeap
    {
    public:
        GuestHeap(uint32_t begin, uint32_t end);

        uint32_t Alloc(uint32_t size, uint32_t align = 16);
        // Claims [address, address + size) if it is entirely free. Used for fixed-address reservations.
        bool AllocAt(uint32_t address, uint32_t size);
        bool Free(uint32_t address);
        // Size of the allocation starting at or containing address, 0 if none.
        uint32_t Query(uint32_t address, uint32_t* allocationBase = nullptr);
        bool Contains(uint32_t address) const { return address >= begin_ && address < end_; }
        // Live allocations as (address, size), for diagnostics.
        std::vector<std::pair<uint32_t, uint32_t>> Allocations();

    private:
        uint32_t begin_;
        uint32_t end_;
        std::mutex mutex_;
        std::map<uint32_t, uint32_t> free_; // start -> size
        std::map<uint32_t, uint32_t> used_; // start -> size
    };

    // Xbox 360 address map:
    //   0x00000000-0x3FFFFFFF  virtual, 4 KB pages
    //   0x40000000-0x6FFFFFFF  virtual, 64 KB pages
    //   0x70000000-0x7EFFFFFF  kernel objects and thread stacks (runtime-owned)
    //   0x7FC80000-0x7FCFFFFF  GPU registers
    //   0xA0000000/0xC0000000/0xE0000000  three windows onto the same 512 MB of physical memory
    extern GuestHeap g_virtual4kHeap;
    extern GuestHeap g_virtualHeap;
    extern GuestHeap g_systemHeap;
    extern GuestHeap g_physicalHeap; // physical addresses 0x00000000-0x1FFFFFFF

    constexpr uint32_t GpuRegisterBase = 0x7FC80000;

    // The physical windows alias one backing store. Unlike hardware, the 4 KB window has no 0x1000
    // shift (16 KB host pages cannot express it); physical addresses come from MmGetPhysicalAddress,
    // which applies the same rule, so CPU and GPU agree.
    inline uint32_t PhysicalToVirtual(uint32_t physical, uint32_t window = 0xA0000000) { return window | (physical & 0x1FFFFFFF); }
    inline bool IsPhysicalWindow(uint32_t address) { return address >= 0xA0000000; }

    GuestHeap* HeapFor(uint32_t address);

    // Write watches over guest physical memory at host page granularity, so the texture cache can skip
    // re-hashing memory nobody wrote. Watched pages are read-only in all three windows; the first write
    // faults, HandleWriteFault marks the page dirty and makes it writable again, and the write proceeds.
    // Pages never watched count as dirty.
    // gpuRead: see MarkGpuRead.
    void WatchPhysical(uint32_t physical, uint32_t size, bool gpuRead = false);
    bool IsPhysicalDirty(uint32_t physical, uint32_t size);
    // The guest thread id that last wrote a watched page in the range (0 if unknown or host code).
    uint32_t PhysicalWriter(uint32_t physical, uint32_t size);
    // A host writer (a syscall such as pread cannot fault into the handler) or the GPU wrote this range.
    void TouchPhysical(uint32_t physical, uint32_t size);
    // Streaming textures (Bink video frames) the GPU may still have to read for frames the title submitted:
    // watched with gpuRead, the next guest write first waits (via the hook) until the command processor has
    // drawn those frames. MarkGpuRead keeps the range watched that way across swaps (RearmGpuReads).
    void MarkGpuRead(uint32_t physical, uint32_t size);
    void SetGpuReadWait(void (*wait)());
    // Set while the GPU waits for a streaming texture's writer to finish (a read that found it mid-write):
    // writes then go ahead instead of waiting for the GPU.
    extern std::atomic<bool> g_gpuAwaitingWriters;
    // At a swap: watch the marked ranges again (writes since lifted their watch).
    void RearmGpuReads();
    // From the SIGSEGV/SIGBUS handler: true if the fault was a watched page, which is now writable.
    bool HandleWriteFault(const void* hostAddress);
}
