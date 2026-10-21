// Guest memory: the 4 GB reservation, the guest heaps, and write watches on physical memory.
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include "memory.h"
#include "thread.h"
#include "log.h"
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace rt
{
    uint8_t* g_base;

    GuestHeap g_virtual4kHeap(0x00010000, 0x40000000);
    GuestHeap g_virtualHeap(0x40000000, 0x70000000);
    GuestHeap g_systemHeap(0x70000000, 0x7F000000);
    // Physical addresses below 16 MB are kept for GPU write-back, as on hardware; the top 64 KB is
    // unusable on hardware too.
    GuestHeap g_physicalHeap(0x01000000, 0x1FFF0000);

    void InitMemory()
    {
        // 4 GB plus a guard so 16-byte accesses at the very top stay inside the mapping.
        // Pages are committed lazily by the host and read as zero.
        constexpr size_t Size = 0x100000000ull + 0x10000;
        // A fixed base keeps guest-to-host addresses identical across runs, which makes watchpoints and
        // crash reports reproducible. Falls back to any address if the hint is taken.
        void* hint = reinterpret_cast<void*>(0x7000000000ull);
        void* p = mmap(hint, Size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED)
            Fatal("Unable to reserve guest memory");
        g_base = static_cast<uint8_t*>(p);
        if (p != hint)
            Log("Guest memory at {} (preferred base unavailable)", p);

        // One 512 MB physical memory, visible through the three physical windows.
        char name[64];
        snprintf(name, sizeof(name), "/peggle2-phys-%d", getpid());
        int fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0600);
        if (fd < 0)
            Fatal("Unable to create physical memory");
        shm_unlink(name);
        if (ftruncate(fd, 0x20000000) != 0)
            Fatal("Unable to size physical memory");
        for (uint32_t window : { 0xA0000000u, 0xC0000000u, 0xE0000000u })
        {
            void* view = mmap(g_base + window, 0x20000000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
            if (view == MAP_FAILED)
                Fatal("Unable to map physical window {:X}", window);
        }
        close(fd);
    }

    GuestHeap::GuestHeap(uint32_t begin, uint32_t end)
        : begin_(begin), end_(end)
    {
        free_.emplace(begin, end - begin);
    }

    static uint32_t AlignUp(uint32_t value, uint32_t align)
    {
        return (value + align - 1) & ~(align - 1);
    }

    uint32_t GuestHeap::Alloc(uint32_t size, uint32_t align)
    {
        if (size == 0)
            size = 1;
        size = AlignUp(size, 16);
        align = align < 16 ? 16 : align;

        std::lock_guard lock(mutex_);
        for (auto it = free_.begin(); it != free_.end(); ++it)
        {
            const uint64_t start = it->first;
            const uint64_t end = start + it->second;
            const uint64_t aligned = AlignUp(uint32_t(start), align);
            if (aligned + size > end)
                continue;

            free_.erase(it);
            if (aligned > start)
                free_.emplace(uint32_t(start), uint32_t(aligned - start));
            if (aligned + size < end)
                free_.emplace(uint32_t(aligned + size), uint32_t(end - aligned - size));
            used_.emplace(uint32_t(aligned), size);
            // Physical heap addresses are not guest virtual addresses; callers clear through a window.
            if (begin_ >= 0x00010000 && this != &g_physicalHeap)
                memset(Translate(uint32_t(aligned)), 0, size);
            return uint32_t(aligned);
        }
        return 0;
    }

    bool GuestHeap::AllocAt(uint32_t address, uint32_t size)
    {
        size = AlignUp(size, 16);
        std::lock_guard lock(mutex_);
        auto it = free_.upper_bound(address);
        if (it == free_.begin())
            return false;
        --it;
        const uint64_t start = it->first;
        const uint64_t end = start + it->second;
        if (address < start || uint64_t(address) + size > end)
            return false;

        free_.erase(it);
        if (address > start)
            free_.emplace(uint32_t(start), uint32_t(address - start));
        if (uint64_t(address) + size < end)
            free_.emplace(address + size, uint32_t(end - address - size));
        used_.emplace(address, size);
        if (this != &g_physicalHeap)
            memset(Translate(address), 0, size);
        return true;
    }

    bool GuestHeap::Free(uint32_t address)
    {
        std::lock_guard lock(mutex_);
        auto it = used_.find(address);
        if (it == used_.end())
            return false;

        uint32_t start = it->first;
        uint32_t size = it->second;
        used_.erase(it);

        // Coalesce with neighbours.
        auto next = free_.lower_bound(start);
        if (next != free_.end() && next->first == start + size)
        {
            size += next->second;
            next = free_.erase(next);
        }
        if (next != free_.begin())
        {
            auto prev = std::prev(next);
            if (prev->first + prev->second == start)
            {
                start = prev->first;
                size += prev->second;
                free_.erase(prev);
            }
        }
        free_.emplace(start, size);
        return true;
    }

    uint32_t GuestHeap::Query(uint32_t address, uint32_t* allocationBase)
    {
        std::lock_guard lock(mutex_);
        auto it = used_.upper_bound(address);
        if (it == used_.begin())
            return 0;
        --it;
        if (address >= it->first + it->second)
            return 0;
        if (allocationBase)
            *allocationBase = it->first;
        return it->second;
    }

    GuestHeap* HeapFor(uint32_t address)
    {
        for (GuestHeap* heap : { &g_virtual4kHeap, &g_virtualHeap, &g_systemHeap })
        {
            if (heap->Contains(address))
                return heap;
        }
        return nullptr;
    }

    std::vector<std::pair<uint32_t, uint32_t>> GuestHeap::Allocations()
    {
        std::lock_guard lock(mutex_);
        return { used_.begin(), used_.end() };
    }

    // --- Write watches -------------------------------------------------------------------------------

    static constexpr uint32_t PhysicalSize = 0x20000000;
    static constexpr uint32_t WatchPageShift = 14; // 16 KB host pages
    static constexpr uint32_t WatchPageCount = PhysicalSize >> WatchPageShift;
    enum : uint8_t { PageWatched = 1, PageDirty = 2, PageGpuRead = 4 };
    static std::atomic<uint8_t> g_pageState[WatchPageCount];
    static const bool g_watchInit = [] {
        for (auto& state : g_pageState)
            state.store(PageDirty, std::memory_order_relaxed);
        return true;
    }();

    static void ProtectPage(uint32_t page, int protection)
    {
        for (uint32_t window : { 0xA0000000u, 0xC0000000u, 0xE0000000u })
            mprotect(g_base + window + (uint64_t(page) << WatchPageShift), size_t(1) << WatchPageShift, protection);
    }

    void WatchPhysical(uint32_t physical, uint32_t size, bool gpuRead)
    {
        physical &= PhysicalSize - 1;
        uint32_t first = physical >> WatchPageShift, last = (physical + std::max(size, 1u) - 1) >> WatchPageShift;
        for (uint32_t page = first; page <= last && page < WatchPageCount; page++)
        {
            // Mark clean first, then protect. A write before the protection lands before the caller reads
            // the page; one after it faults and marks the page dirty again. (Protecting first let a fault
            // between the two steps unprotect the page and have its dirty mark overwritten, so the page
            // stayed writable but clean and later writes were never seen: stale rows in Bink videos.)
            if (!(g_pageState[page].exchange(gpuRead ? PageWatched | PageGpuRead : PageWatched, std::memory_order_acq_rel) & PageWatched))
                ProtectPage(page, PROT_READ);
        }
    }

    bool IsPhysicalDirty(uint32_t physical, uint32_t size)
    {
        physical &= PhysicalSize - 1;
        uint32_t first = physical >> WatchPageShift, last = (physical + std::max(size, 1u) - 1) >> WatchPageShift;
        for (uint32_t page = first; page <= last && page < WatchPageCount; page++)
        {
            if (g_pageState[page].load(std::memory_order_acquire) & PageDirty)
                return true;
        }
        return false;
    }

    void TouchPhysical(uint32_t physical, uint32_t size)
    {
        physical &= PhysicalSize - 1;
        uint32_t first = physical >> WatchPageShift, last = (physical + std::max(size, 1u) - 1) >> WatchPageShift;
        for (uint32_t page = first; page <= last && page < WatchPageCount; page++)
        {
            uint8_t previous = g_pageState[page].exchange(PageDirty, std::memory_order_acq_rel);
            if (previous & PageWatched)
                ProtectPage(page, PROT_READ | PROT_WRITE);
        }
    }

    // Pages marked by MarkGpuRead, re-armed at every swap (see RearmGpuReads).
    static std::mutex g_gpuReadMutex;
    struct GpuReadRange { uint32_t last; uint32_t swapsLeft; };
    static std::map<uint32_t, GpuReadRange> g_gpuReadRanges; // by first page

    void MarkGpuRead(uint32_t physical, uint32_t size)
    {
        physical &= PhysicalSize - 1;
        uint32_t first = physical >> WatchPageShift, last = (physical + std::max(size, 1u) - 1) >> WatchPageShift;
        std::lock_guard lock(g_gpuReadMutex);
        GpuReadRange& range = g_gpuReadRanges[first]; // several textures can share a base
        range.last = std::max(range.last, std::min(last, WatchPageCount - 1));
        range.swapsLeft = 10;
    }

    void RearmGpuReads()
    {
        std::lock_guard lock(g_gpuReadMutex);
        for (auto it = g_gpuReadRanges.begin(); it != g_gpuReadRanges.end();)
        {
            auto& [first, range] = *it;
            for (uint32_t page = first; page <= range.last; page++)
            {
                // Keep the dirty mark (the texture cache has not seen these writes yet); only watch again.
                const uint8_t previous = g_pageState[page].fetch_or(PageWatched | PageGpuRead, std::memory_order_acq_rel);
                if (!(previous & PageWatched))
                    ProtectPage(page, PROT_READ);
            }
            it = --range.swapsLeft ? std::next(it) : g_gpuReadRanges.erase(it); // no longer streaming
        }
    }

    static std::atomic<void (*)()> g_gpuReadWait;
    std::atomic<bool> g_gpuAwaitingWriters;
    void SetGpuReadWait(void (*wait)())
    {
        g_gpuReadWait = wait;
    }

    // The guest thread that last faulted on each watched page (0 for host code).
    static std::atomic<uint32_t> g_pageWriter[WatchPageCount];

    uint32_t PhysicalWriter(uint32_t physical, uint32_t size)
    {
        physical &= PhysicalSize - 1;
        uint32_t first = physical >> WatchPageShift, last = (physical + std::max(size, 1u) - 1) >> WatchPageShift;
        for (uint32_t page = first; page <= last && page < WatchPageCount; page++)
            if (uint32_t id = g_pageWriter[page].load(std::memory_order_relaxed))
                return id;
        return 0;
    }

    bool HandleWriteFault(const void* hostAddress)
    {
        uintptr_t offset = uintptr_t(hostAddress) - uintptr_t(g_base);
        if (uintptr_t(hostAddress) < uintptr_t(g_base) || offset < 0xA0000000ull || offset >= 0x100000000ull)
            return false;
        uint32_t page = uint32_t(offset & (PhysicalSize - 1)) >> WatchPageShift;
        // The physical windows are always mapped read-write unless watched, so any write fault here is a
        // watch, possibly one another thread already lifted while this fault was in flight. mprotect is
        // async-signal-safe.
        GuestThread* writer = CurrentThread(); // thread-local read, async-signal-safe in practice
        // A guest thread overwriting memory the GPU may still read for a submitted frame (Bink decoding into
        // the buffer the previous frame drew) waits for the GPU to draw it first: on the console the GPU was
        // done with it by then, but the command processor here can trail the title by a frame. The page stays
        // protected meanwhile, so the GPU's read sees it clean. (The hook only sleeps and polls.)
        if (writer && (g_pageState[page].load(std::memory_order_acquire) & PageGpuRead) && !g_gpuAwaitingWriters.load(std::memory_order_acquire))
            if (auto wait = g_gpuReadWait.load(std::memory_order_relaxed))
                wait();
        ProtectPage(page, PROT_READ | PROT_WRITE);
        g_pageState[page].store(PageDirty, std::memory_order_release);
        g_pageWriter[page].store(writer ? writer->id : 0, std::memory_order_relaxed);
        return true;
    }
}
