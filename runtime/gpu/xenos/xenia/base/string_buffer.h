// Stand-in for Xenia's base/string_buffer.h, on std::string.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <fmt/format.h>
namespace xe
{
    class StringBuffer
    {
    public:
        explicit StringBuffer(size_t initialCapacity = 0) { s_.reserve(initialCapacity); }
        char* buffer() { return s_.data(); }
        size_t length() const { return s_.size(); }
        void Reset() { s_.clear(); }
        void Append(char c) { s_ += c; }
        void Append(char c, size_t count) { s_.append(count, c); }
        void Append(const char* value) { s_ += value; }
        void Append(std::string_view value) { s_ += value; }
        template<typename... Args> void AppendFormat(fmt::format_string<Args...> format, Args&&... args) { s_ += fmt::format(format, std::forward<Args>(args)...); }
        void AppendBytes(const uint8_t* data, size_t length) { s_.append(reinterpret_cast<const char*>(data), length); }
        std::string to_string() { return s_; }
        std::string_view to_string_view() const { return s_; }
        std::vector<uint8_t> to_bytes() const { return { s_.begin(), s_.end() }; }
    private:
        std::string s_;
    };
}
