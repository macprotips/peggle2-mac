// Stand-in for the parts of Xenia's base/memory.h the GPU files use.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include "xenia/base/byte_order.h"
namespace xe
{
    using fourcc_t = uint32_t;
    constexpr inline fourcc_t make_fourcc(char a, char b, char c, char d)
    {
        return fourcc_t((fourcc_t(uint8_t(a)) << 24) | (fourcc_t(uint8_t(b)) << 16) | (fourcc_t(uint8_t(c)) << 8) | fourcc_t(uint8_t(d)));
    }
    constexpr inline fourcc_t make_fourcc(std::string_view s) { return make_fourcc(s[0], s[1], s[2], s[3]); }

    template<typename T> inline void copy_and_swap(T* dest, const T* src, size_t count)
    {
        for (size_t i = 0; i < count; i++)
            dest[i] = byte_swap(src[i]);
    }
    namespace memory
    {
        template<typename T, typename F> inline T Reinterpret(F from) { static_assert(sizeof(T) == sizeof(F)); T to; memcpy(&to, &from, sizeof(T)); return to; }
    }
}
