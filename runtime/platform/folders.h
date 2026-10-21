// Where the port keeps its files, as macOS asks apps to: saves, the profile and the installed game in
// Application Support, regenerable data in Caches. Shared by the game and the installer.
#pragma once
#include <filesystem>

namespace rt
{
    // ~/Library/Application Support/Peggle 2 (PEGGLE2_DATA_DIR overrides it, for tests). The folder of
    // earlier versions, "Peggle2", is moved here the first time.
    std::filesystem::path UserDataDirectory();

    // The title's cache partition: ~/Library/Caches/<bundle id>, or a cache folder inside an overridden
    // data folder.
    std::filesystem::path CacheDirectory();

    // Settings saved under the bundle identifier of earlier versions (local.peggle2mac) are copied to the
    // current one, once.
    void MigrateSettings();
}
