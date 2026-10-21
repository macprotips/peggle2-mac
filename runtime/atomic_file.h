// Replaces a file with new contents all at once: written to a temporary file, flushed to disk, then
// renamed over the original. A crash, power loss or full disk leaves the previous version intact.
#pragma once
#include <filesystem>
#include <string_view>

namespace rt
{
    bool WriteFileAtomically(const std::filesystem::path& path, std::string_view contents);
}
