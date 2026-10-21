#include <filesystem>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <image.h>
#include <file.h>
#include <xex.h>
#include "guest.h"
#include "thread.h"
#include "kernel/module.h"
#include "kernel/vfs.h"
#include "kernel/xam.h"
#include "platform/app.h"
#include "platform/folders.h"
#include "gpu/renderer.h"
#include "game_import.h"
#include "audio/xma.h"
#include "achievements.h"

using namespace rt;

static void LoadExecutable(const std::filesystem::path& path, uint32_t& entryPoint, uint32_t& stackSize, uint32_t& tlsSlots)
{
    // A damaged install: forget it, so the next launch asks for the game again instead of failing again.
    auto damaged = [&](const std::string& detail) {
        std::error_code error;
        std::filesystem::remove(path.parent_path() / ".installed", error);
        Fatal("The installed game files are damaged ({}). Open Peggle 2 again to reinstall the game.", detail);
    };
    std::vector<uint8_t> file = LoadFile(path);
    if (file.empty())
        damaged("Default.xex could not be read");

    Image image = Image::ParseImage(file.data(), file.size());
    if (!image.data || image.base != PPC_IMAGE_BASE || image.size != PPC_IMAGE_SIZE)
        damaged("Default.xex is not the expected executable");

    memcpy(Translate(uint32_t(image.base)), image.data.get(), image.size);
    entryPoint = uint32_t(image.entry_point);

    auto* stack = static_cast<const be<uint32_t>*>(getOptHeaderPtr(file.data(), XEX_HEADER_DEFAULT_STACK_SIZE));
    stackSize = stack ? uint32_t(*stack) : 0x40000;
    auto* tls = static_cast<const be<uint32_t>*>(getOptHeaderPtr(file.data(), XEX_HEADER_TLS_INFO));
    tlsSlots = tls ? uint32_t(tls[0]) : 64;

    InitExecutableModule(file, entryPoint);
}


// The title's achievements live in its XDBF resource (an XEX resource, loaded with the image).
static void LoadAchievements(const std::filesystem::path& xexPath)
{
    std::vector<uint8_t> file = LoadFile(xexPath);
    auto* info = static_cast<const Xex2ResourceInfo*>(getOptHeaderPtr(file.data(), XEX_HEADER_RESOURCE_INFO));
    for (uint32_t i = 0; info && i < (info->sizeOfHeader - 4) / 16; i++)
    {
        const auto* entry = reinterpret_cast<const uint8_t*>(info) + 4 + i * 16;
        const uint32_t address = *reinterpret_cast<const be<uint32_t>*>(entry + 8), size = *reinterpret_cast<const be<uint32_t>*>(entry + 12);
        const uint8_t* data = Translate<uint8_t>(address);
        if (size >= 4 && memcmp(data, "XDBF", 4) == 0)
        {
            achievements::Load(data, size, ConsoleLanguage(), UserDataDirectory() / "achievements.txt");
            return;
        }
    }
}

static void MountDevices(const std::filesystem::path& gameDir)
{
    for (const char* device : { "game:", "d:", "\\Device\\Cdrom0" })
        Mount(device, gameDir, false);

    Mount("cache:", CacheDirectory(), true);
    Mount("\\Device\\Harddisk0\\Cache0", CacheDirectory(), true);
    InitXam(UserDataDirectory());
}

static void InstallFunctionTable()
{
    size_t count = 0;
    for (PPCFuncMapping* m = PPCFuncMappings; m->host; m++, count++)
        PPC_LOOKUP_FUNC(g_base, m->guest) = m->host;
    Log("Installed {} recompiled functions", count);
}

// Launched from Finder, stderr is /dev/null: send the log (and crash reports) to
// ~/Library/Logs/Peggle 2/Peggle 2.log, keeping the previous run's as Peggle 2.previous.log.
static void RedirectLogWhenDetached()
{
    struct stat err, null;
    if (fstat(STDERR_FILENO, &err) != 0 || stat("/dev/null", &null) != 0 || err.st_dev != null.st_dev || err.st_ino != null.st_ino)
        return;
    const char* home = getenv("HOME");
    if (!home)
        return;
    const std::filesystem::path dir = std::filesystem::path(home) / "Library/Logs/Peggle 2";
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    std::filesystem::rename(dir / "Peggle 2.log", dir / "Peggle 2.previous.log", error);
    if (FILE* log = freopen((dir / "Peggle 2.log").c_str(), "w", stderr))
        setvbuf(log, nullptr, _IOLBF, 0);
}

int main(int argc, char** argv)
{
    RedirectLogWhenDetached();
    // --write-icon <game dir> <png>: the build's AppIcon, from the game's artwork.
    if (argc == 4 && std::string(argv[1]) == "--write-icon")
    {
        const std::filesystem::path game = argv[2];
        return WriteAppIcon((game / "BannerLogo.png").string(), (game / "icon.png").string(), argv[3]) ? 0 : 1;
    }

    // --install <package or folder> [game dir]: install without the window, for scripts and tests.
    if (argc >= 3 && std::string(argv[1]) == "--install")
    {
        std::filesystem::path package = FindGamePackage(argv[2]);
        if (package.empty())
        {
            fprintf(stderr, "No Peggle 2 package found in %s\n", argv[2]);
            return 1;
        }
        std::filesystem::path target = argc >= 4 ? std::filesystem::path(argv[3]) : UserDataDirectory() / "game";
        std::string error = InstallGame(package, target, [](double fraction) { fprintf(stderr, "\r%3d%%", int(fraction * 100)); });
        fprintf(stderr, "\n%s\n", error.empty() ? ("Installed to " + target.string()).c_str() : error.c_str());
        return error.empty() ? 0 : 1;
    }

    // A game directory on the command line is for development; the app installs its own copy. (macOS
    // may pass -psn_... to apps opened from Finder; that is not a directory.)
    const bool explicitDir = argc >= 2 && std::string(argv[1]).rfind("-psn_", 0) != 0;
    const std::filesystem::path gameDir = explicitDir ? std::filesystem::path(argv[1]) : UserDataDirectory() / "game";
    InstallCrashHandler();

    // One copy at a time per data folder: two would write the same saves.
    const std::filesystem::path lockPath = UserDataDirectory() / ".lock";
    const int lock = open(lockPath.c_str(), O_RDWR | O_CREAT, 0644);
    if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) != 0)
    {
        ShowStartupError("Peggle 2 is already open.", "Only one copy of Peggle 2 can run at a time, so that both don't write the same saves.");
        return 0;
    }
    InitMemory();

    AppCallbacks callbacks;
    callbacks.gameInstalled = [=] { return explicitDir || IsGameInstalled(gameDir); };
    callbacks.installGame = [=](const std::string& dropped, std::function<void(double)> progress) -> std::string {
        std::filesystem::path package = FindGamePackage(dropped);
        if (package.empty())
            return "No Peggle 2 package was found there. Choose the package file (in a folder named 58411436) or a folder that contains it.";
        return InstallGame(package, gameDir, progress);
    };
    callbacks.bannerPath = [=] { return (gameDir / "BannerLogo.png").string(); };
    callbacks.iconPath = [=] { return (gameDir / "icon.png").string(); };
    callbacks.startGame = [=] {
        MountDevices(gameDir);
        uint32_t entryPoint, stackSize, tlsSlots;
        LoadExecutable(gameDir / "Default.xex", entryPoint, stackSize, tlsSlots);
        LoadAchievements(gameDir / "Default.xex");
        InstallFunctionTable();
        gpu::InitializeRenderer();
        xma::Initialize();
        Log("Starting entry point {:X} (stack {:X}, {} TLS slots)", entryPoint, stackSize, tlsSlots);
        StartMainThread(entryPoint, stackSize, tlsSlots);
    };
    RunApplication(std::move(callbacks));
}
