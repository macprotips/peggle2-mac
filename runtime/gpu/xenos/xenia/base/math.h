// Stand-in for the parts of Xenia's base/math.h the GPU files use.
#pragma once
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
namespace xe
{
    template<typename T, size_t N> constexpr size_t countof(T (&)[N]) { return N; }
    template<typename T> constexpr T clamp(T value, T low, T high) { return std::clamp(value, low, high); }
    inline uint32_t bit_count(uint32_t v) { return uint32_t(std::popcount(v)); }
    inline uint32_t bit_count(uint64_t v) { return uint32_t(std::popcount(v)); }
    template<typename T> constexpr T next_pow2(T v) { return v <= 1 ? 1 : T(std::bit_ceil(std::make_unsigned_t<T>(v))); }
    template<typename T> constexpr T round_up(T value, T multiple) { return multiple ? (value + multiple - 1) / multiple * multiple : value; }
    template<typename T> constexpr T align(T value, T alignment) { return (value + alignment - 1) & ~(alignment - 1); }
    inline bool bit_scan_forward(uint32_t v, uint32_t* out) { if (!v) return false; *out = uint32_t(std::countr_zero(v)); return true; }
    inline bool bit_scan_forward(uint64_t v, uint32_t* out) { if (!v) return false; *out = uint32_t(std::countr_zero(v)); return true; }
    template<typename T> constexpr T clamp_float(T value, T low, T high) { return std::min(std::max(value, low), high); }
    template<typename T> constexpr T saturate(T value) { return clamp_float(value, T(0), T(1)); }
    inline uint32_t log2_floor(uint32_t v) { return 31 - uint32_t(std::countl_zero(v)); }
    inline uint8_t lzcnt(uint32_t v) { return uint8_t(std::countl_zero(v)); }
    inline uint32_t log2_ceil(uint32_t v) { return v <= 1 ? 0 : 32 - uint32_t(std::countl_zero(v - 1)); }
}
