#pragma once
#include <functional>
#include "../achievements.h"
#include <string>
#include <vector>

namespace rt
{
    struct AppCallbacks
    {
        // Whether the game files are present; if not, the window asks for the user's game package.
        std::function<bool()> gameInstalled;
        // Installs from a dropped or chosen file or folder, off the main thread. Returns an error
        // message, or empty on success. progress receives 0..1.
        std::function<std::string(const std::string& path, std::function<void(double)> progress)> installGame;
        // The installed game's artwork: its logo banner and its square thumbnail (PNG). The app icon is
        // composed from them; no game art ships with the app.
        std::function<std::string()> bannerPath;
        std::function<std::string()> iconPath;
        // Called on the main thread once the game can run; must return immediately (the game runs
        // on its own threads).
        std::function<void()> startGame;
    };

    // A modal alert before the app is running (for example, another copy is already open).
    void ShowStartupError(const std::string& title, const std::string& message);

    // Runs the macOS application on the main thread. Never returns.
    [[noreturn]] void RunApplication(AppCallbacks callbacks);

    // A toast for a newly unlocked achievement, like the console's. Callable from any thread.
    void ShowAchievementUnlocked(const achievements::Achievement& achievement);
    // Opens the achievements window (the Guide's list; also in the app menu). Callable from any thread.
    // The title is told a system UI is showing (XN_SYS_UI) while it is open.
    void ShowAchievements();

    // An alert on the game window with the given buttons (the first is the default when focused is out
    // of range). done receives the chosen button's index, on the main thread. Callable from any thread.
    void ShowMessageBox(const std::string& title, const std::string& text, const std::vector<std::string>& buttons, int focused,
                        std::function<void(int)> done);

    // Writes the app icon composed from the game's banner (or thumbnail) as a 1024x1024 PNG. For the build.
    bool WriteAppIcon(const std::string& bannerPath, const std::string& thumbnailPath, const std::string& pngPath);

    // Quits the app as Quit in the menu does. Callable from any thread.
    void QuitApplication();

    // The CAMetalLayer the renderer presents to (void* to keep this header plain C++).
    void* MetalLayer();
}
