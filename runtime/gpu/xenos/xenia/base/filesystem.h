// Stand-in for Xenia's base/filesystem.h.
#pragma once
#include <cstdio>
#include <filesystem>
namespace xe::filesystem
{
    using std::filesystem::create_directories;
    inline FILE* OpenFile(const std::filesystem::path& path, const char* mode) { return fopen(path.c_str(), mode); }
}
