// Stand-in for Xenia's Memory: guest virtual and physical address translation.
#pragma once
#include <cstdint>
namespace rt
{
    uint8_t* TranslateGuest(uint32_t address);
    namespace gpu { uint8_t* TranslatePhysical(uint32_t physical); }
}
namespace xe
{
    class Memory
    {
    public:
        uint8_t* TranslateVirtual(uint32_t address) const { return rt::TranslateGuest(address); }
        uint8_t* TranslatePhysical(uint32_t address) const { return rt::gpu::TranslatePhysical(address); }
        template<typename T> T TranslatePhysical(uint32_t address) const { return reinterpret_cast<T>(TranslatePhysical(address)); }
    };
}
