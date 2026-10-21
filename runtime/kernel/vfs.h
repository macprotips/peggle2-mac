#pragma once
#include <filesystem>
#include <optional>
#include <string>

namespace rt
{
    // Maps a guest device prefix (eg. "game:", "\Device\Cdrom0") to a host directory.
    void Mount(const std::string& device, const std::filesystem::path& root, bool writable);

    struct ResolvedPath
    {
        std::filesystem::path host;
        bool writable = false;
    };

    // Resolves a guest path through symbolic links and mounts. Missing components are matched
    // case-insensitively, as on the Xbox file systems.
    std::optional<ResolvedPath> ResolvePath(const std::string& guestPath);

    void CreateSymbolicLink(const std::string& link, const std::string& target);
    void DeleteSymbolicLink(const std::string& link);
}
