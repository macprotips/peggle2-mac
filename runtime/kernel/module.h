#pragma once
#include <cstdint>
#include <vector>

namespace rt
{
    // Copies the executable's XEX header into guest memory and creates its loader entry.
    void InitExecutableModule(const std::vector<uint8_t>& xexFile, uint32_t entryPoint);
}
