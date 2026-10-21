// XMA audio decoder hardware (see xma.cpp).
#pragma once
#include <cstdint>

namespace rt::xma
{
    void Initialize();
    // Guest virtual address of a free 64-byte context, or 0 when all are in use.
    uint32_t AllocateContext();
    void ReleaseContext(uint32_t context);
    // A store the title marked as MMIO (followed by eieio) into the XMA register block.
    void MmioWrite(uint32_t address, uint32_t stored);
}
