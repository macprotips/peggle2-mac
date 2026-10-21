// Virtual and physical memory imports (NtAllocateVirtualMemory, MmAllocatePhysicalMemoryEx, ...).
#include "../guest.h"
#include "xbox_types.h"
#include <algorithm>
#include <cstring>

namespace rt
{
    enum : uint32_t
    {
        MEM_COMMIT = 0x1000,
        MEM_RESERVE = 0x2000,
        MEM_DECOMMIT = 0x4000,
        MEM_RELEASE = 0x8000,
        MEM_LARGE_PAGES = 0x20000000,
        MEM_FREE = 0x10000,
        MEM_PRIVATE = 0x20000,
        PAGE_READWRITE = 0x04,
        PAGE_NOACCESS = 0x01,
    };

    static uint32_t PageSizeFor(uint32_t flags)
    {
        return (flags & MEM_LARGE_PAGES) ? 0x10000 : 0x1000;
    }

    // 4 KB page allocations come from the low virtual range, 64 KB pages from 0x40000000, as on hardware.
    static GuestHeap& VirtualHeapFor(uint32_t allocationType)
    {
        return (allocationType & MEM_LARGE_PAGES) ? g_virtualHeap : g_virtual4kHeap;
    }

    static uint32_t NtAllocateVirtualMemory(be<uint32_t>* baseAddress, be<uint32_t>* regionSize, uint32_t allocationType, uint32_t, uint32_t)
    {
        const uint32_t page = PageSizeFor(allocationType);
        uint32_t size = (uint32_t(*regionSize) + page - 1) & ~(page - 1);
        uint32_t requested = *baseAddress;
        if (size == 0)
            return STATUS_INVALID_PARAMETER;

        if (requested)
        {
            requested &= ~(page - 1);
            uint32_t allocationBase;
            GuestHeap* existing = HeapFor(requested);
            if (existing && existing->Query(requested, &allocationBase))
            {
                // Committing pages of an earlier reservation: the memory is already backed.
                *baseAddress = requested;
                *regionSize = size;
                return STATUS_SUCCESS;
            }
            if (!VirtualHeapFor(allocationType).AllocAt(requested, size))
                return STATUS_NO_MEMORY;
            *baseAddress = requested;
            *regionSize = size;
            return STATUS_SUCCESS;
        }

        uint32_t address = VirtualHeapFor(allocationType).Alloc(size, page);
        if (getenv("PEGGLE2_MEMLOG"))
            Log("VirtualAlloc {:X} bytes type {:X} -> {:X}", size, allocationType, address);
        if (!address)
        {
            Log("NtAllocateVirtualMemory: out of memory for {:X} bytes", size);
            return STATUS_NO_MEMORY;
        }
        *baseAddress = address;
        *regionSize = size;
        return STATUS_SUCCESS;
    }

    static uint32_t NtFreeVirtualMemory(be<uint32_t>* baseAddress, be<uint32_t>* regionSize, uint32_t freeType, uint32_t)
    {
        uint32_t address = *baseAddress;
        uint32_t allocationBase;
        GuestHeap* heap = HeapFor(address);
        uint32_t size = heap ? heap->Query(address, &allocationBase) : 0;
        if (!size)
            return STATUS_INVALID_PARAMETER;

        if (getenv("PEGGLE2_MEMLOG"))
            Log("VirtualFree {:X} (base {:X}, size {:X}) type {:X}", address, allocationBase, size, freeType);
        if (freeType & MEM_RELEASE)
        {
            heap->Free(allocationBase);
            *baseAddress = allocationBase;
            *regionSize = size;
            return STATUS_SUCCESS;
        }

        // MEM_DECOMMIT: committing these pages again must yield zero-filled pages, and heap allocators
        // rely on that for their metadata. Zero now so a later commit sees fresh pages. A size of 0
        // decommits to the end of the allocation.
        uint32_t start = address & ~0xFFFu;
        uint32_t length = *regionSize ? uint32_t(*regionSize) : allocationBase + size - start;
        length = std::min(length, allocationBase + size - start);
        // Note: memset rather than remapping, since guest pages (4/64 KB) need not align with
        // 16 KB host pages; remap whole host pages if decommit-heavy titles show up in profiles.
        memset(Translate(start), 0, length);
        *baseAddress = start;
        *regionSize = length;
        return STATUS_SUCCESS;
    }

    struct MemoryBasicInformation
    {
        be<uint32_t> baseAddress;
        be<uint32_t> allocationBase;
        be<uint32_t> allocationProtect;
        be<uint32_t> regionSize;
        be<uint32_t> state;
        be<uint32_t> protect;
        be<uint32_t> type;
    };

    static uint32_t NtQueryVirtualMemory(uint32_t address, MemoryBasicInformation* info)
    {
        GuestHeap* heap = HeapFor(address);
        uint32_t allocationBase = 0;
        uint32_t size = heap ? heap->Query(address, &allocationBase) : 0;
        if (!size)
        {
            info->baseAddress = address & ~0xFFFu;
            info->allocationBase = 0;
            info->allocationProtect = 0;
            info->regionSize = 0x1000;
            info->state = MEM_FREE;
            info->protect = PAGE_NOACCESS;
            info->type = 0;
            return STATUS_SUCCESS;
        }
        info->baseAddress = allocationBase;
        info->allocationBase = allocationBase;
        info->allocationProtect = PAGE_READWRITE;
        info->regionSize = size;
        info->state = MEM_COMMIT;
        info->protect = PAGE_READWRITE;
        info->type = MEM_PRIVATE;
        return STATUS_SUCCESS;
    }

    // Protect flags pick the page size and with it the window the caller sees the memory through.
    static uint32_t PhysicalWindowFor(uint32_t protect, uint32_t& pageSize)
    {
        constexpr uint32_t MEM_16MB_PAGES = 0x80000000;
        if (protect & MEM_16MB_PAGES)
        {
            pageSize = 0x1000000;
            return 0xC0000000;
        }
        if (protect & MEM_LARGE_PAGES)
        {
            pageSize = 0x10000;
            return 0xA0000000;
        }
        // The 4 KB window (0xE0000000) maps physical memory shifted by 0x1000 on hardware, and D3D
        // applies that shift itself when it derives GPU addresses. 16 KB host pages cannot reproduce
        // the shift, so 4 KB allocations are handed out through the 0xA0000000 window, where CPU and
        // GPU addresses agree by masking.
        pageSize = 0x1000;
        return 0xA0000000;
    }

    static uint32_t MmAllocatePhysicalMemoryEx(uint32_t, uint32_t size, uint32_t protect, uint32_t minAddress, uint32_t maxAddress, uint32_t alignment)
    {
        uint32_t page;
        uint32_t window = PhysicalWindowFor(protect, page);
        uint32_t align = alignment > page ? alignment : page;
        size = (size + page - 1) & ~(page - 1);
        uint32_t physical = g_physicalHeap.Alloc(size, align);
        if (!physical)
        {
            Log("MmAllocatePhysicalMemoryEx: out of memory for {:X} bytes", size);
            return 0;
        }
        if ((minAddress && physical < minAddress) || (maxAddress && physical + size - 1 > maxAddress))
            Log("MmAllocatePhysicalMemoryEx: {:X} outside requested range {:X}-{:X}", physical, minAddress, maxAddress);
        uint32_t address = PhysicalToVirtual(physical, window);
        memset(Translate(address), 0, size);
        return address;
    }

    static void MmFreePhysicalMemory(uint32_t, uint32_t address)
    {
        g_physicalHeap.Free(address & 0x1FFFFFFF);
    }

    static uint32_t MmGetPhysicalAddress(uint32_t address)
    {
        return IsPhysicalWindow(address) ? (address & 0x1FFFFFFF) : address;
    }

    static uint32_t MmQueryAddressProtect(uint32_t)
    {
        return PAGE_READWRITE;
    }

    static uint32_t XamAlloc(uint32_t, uint32_t size, be<uint32_t>* out)
    {
        uint32_t address = g_virtualHeap.Alloc(size);
        *out = address;
        return address ? ERROR_SUCCESS : E_OUTOFMEMORY;
    }

    static uint32_t XamFree(uint32_t address)
    {
        g_virtualHeap.Free(address);
        return ERROR_SUCCESS;
    }

    static void RtlFillMemoryUlong(uint32_t destination, uint32_t length, uint32_t pattern)
    {
        auto* p = Translate<be<uint32_t>>(destination);
        for (uint32_t i = 0; i < length / 4; i++)
            p[i] = pattern;
    }

    static uint32_t RtlCompareMemoryUlong(uint32_t source, uint32_t length, uint32_t pattern)
    {
        auto* p = Translate<be<uint32_t>>(source);
        uint32_t matched = 0;
        for (uint32_t i = 0; i < length / 4 && p[i] == pattern; i++)
            matched += 4;
        return matched;
    }
}

using namespace rt;
GUEST_IMPORT(NtAllocateVirtualMemory, NtAllocateVirtualMemory)
GUEST_IMPORT(NtFreeVirtualMemory, NtFreeVirtualMemory)
GUEST_IMPORT(NtQueryVirtualMemory, NtQueryVirtualMemory)
GUEST_IMPORT(MmAllocatePhysicalMemoryEx, MmAllocatePhysicalMemoryEx)
GUEST_IMPORT(MmFreePhysicalMemory, MmFreePhysicalMemory)
GUEST_IMPORT(MmGetPhysicalAddress, MmGetPhysicalAddress)
GUEST_IMPORT(MmQueryAddressProtect, MmQueryAddressProtect)
GUEST_IMPORT(XamAlloc, XamAlloc)
GUEST_IMPORT(XamFree, XamFree)
GUEST_IMPORT(RtlFillMemoryUlong, RtlFillMemoryUlong)
GUEST_IMPORT(RtlCompareMemoryUlong, RtlCompareMemoryUlong)
