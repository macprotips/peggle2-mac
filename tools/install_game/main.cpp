// Installs the game files from a Peggle 2 package (or a folder containing it), with the same checks as the
// app's installer. For building from source: tools/regen.sh needs game/Default.xex before the app exists.
// usage: install_game <package or folder> <destination>
#include "game_import.h"
#include "log.h"
#include <cstdio>
#include <cstdlib>

namespace rt
{
    void LogLine(const std::string& line) { fprintf(stderr, "%s\n", line.c_str()); }
    void FatalLine(const std::string& line) { fprintf(stderr, "%s\n", line.c_str()); exit(1); }
}

int main(int argc, char** argv)
{
    if (argc != 3)
        return fprintf(stderr, "usage: install_game <package or folder> <destination>\n"), 2;
    const std::filesystem::path package = rt::FindGamePackage(argv[1]);
    if (package.empty())
        return fprintf(stderr, "No Peggle 2 package found in %s\n", argv[1]), 1;
    const std::string error = rt::InstallGame(package, argv[2], [](double) {});
    if (!error.empty())
        return fprintf(stderr, "%s\n", error.c_str()), 1;
    printf("Installed to %s\n", argv[2]);
    return 0;
}
