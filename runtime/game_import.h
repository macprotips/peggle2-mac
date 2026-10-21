// Finding and installing the player's Peggle 2 package (an STFS LIVE/PIRS file) from a drop or folder.
#pragma once
#include <filesystem>
#include <functional>
#include <string>

namespace rt
{
    // The Peggle 2 package inside a dropped file or folder (searched recursively), or empty.
    std::filesystem::path FindGamePackage(const std::filesystem::path& dropped);

    // Extracts the package into gameDir and checks it is the release this build was recompiled from.
    // Returns an error message for the user, or empty on success. progress receives 0..1.
    std::string InstallGame(const std::filesystem::path& package, const std::filesystem::path& gameDir,
                            const std::function<void(double)>& progress);

    bool IsGameInstalled(const std::filesystem::path& gameDir);
}
