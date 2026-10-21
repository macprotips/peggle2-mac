// Installs the game from the user's own Xbox Live Arcade package (STFS "LIVE"/"PIRS"/"CON " container).
// Layout as documented by the Free60 project and implemented by Xenia's STFS device.
#include "game_import.h"
#include "log.h"
#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace rt
{
    namespace fs = std::filesystem;

    constexpr uint32_t PeggleTitleId = 0x58411436;
    // SHA-256 of the retail Default.xex this build was recompiled from.
    constexpr const char* ExecutableSha256 = "1ddb076bd45cfaf8942c94719858860749ed005f198d82da10e479f832b4e088";

    static uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
    static uint32_t Le24(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16; }

    static bool IsPeggleTitle(const fs::path& file)
    {
        uint8_t header[0x364];
        std::ifstream in(file, std::ios::binary);
        if (!in.read(reinterpret_cast<char*>(header), sizeof(header)))
            return false;
        bool magic = !memcmp(header, "LIVE", 4) || !memcmp(header, "PIRS", 4) || !memcmp(header, "CON ", 4);
        return magic && Be32(header + 0x360) == PeggleTitleId;
    }

    fs::path FindGamePackage(const fs::path& dropped)
    {
        std::error_code error;
        if (fs::is_regular_file(dropped, error))
            return IsPeggleTitle(dropped) ? dropped : fs::path();
        for (auto it = fs::recursive_directory_iterator(dropped, fs::directory_options::skip_permission_denied, error);
             it != fs::recursive_directory_iterator(); it.increment(error))
        {
            if (it->is_regular_file(error) && it->file_size(error) > 0x1000 && IsPeggleTitle(it->path()))
                return it->path();
        }
        return {};
    }

    static std::string Sha256(const fs::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        CC_SHA256_CTX ctx;
        CC_SHA256_Init(&ctx);
        std::vector<char> buffer(1 << 20);
        while (in.read(buffer.data(), buffer.size()) || in.gcount())
            CC_SHA256_Update(&ctx, buffer.data(), CC_LONG(in.gcount()));
        unsigned char digest[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256_Final(digest, &ctx);
        std::string hex;
        for (unsigned char b : digest)
            hex += fmt::format("{:02x}", b);
        return hex;
    }

    bool IsGameInstalled(const fs::path& gameDir)
    {
        std::error_code error;
        return fs::exists(gameDir / ".installed", error) && fs::exists(gameDir / "Default.xex", error);
    }

    std::string InstallGame(const fs::path& package, const fs::path& gameDir, const std::function<void(double)>& progress)
    {
        std::ifstream in(package, std::ios::binary);
        std::vector<uint8_t> header(0x1720);
        if (!in.read(reinterpret_cast<char*>(header.data()), header.size()))
            return "The file is too short to be the Peggle 2 game file.";

        const uint32_t headerSize = Be32(&header[0x340]);
        const uint8_t* volume = &header[0x379];
        const bool readOnly = volume[2] & 1;
        const uint32_t tableBlocks = volume[3] | volume[4] << 8;
        const uint32_t tableStart = Le24(volume + 5);
        const uint32_t tablesPerLevel = readOnly ? 1 : 2;

        // Data blocks are interleaved with hash tables: one per 0xAA blocks, per 0xAA^2, per 0xAA^3.
        auto blockOffset = [&](uint32_t index) {
            uint64_t block = index;
            uint64_t base = 0xAA;
            for (int level = 0; level < 3; level++)
            {
                block += ((index + base) / base) * tablesPerLevel;
                if (index < base)
                    break;
                base *= 0xAA;
            }
            return ((uint64_t(headerSize) + 0xFFF) & ~0xFFFull) + (block << 12);
        };

        struct Entry
        {
            std::string name;
            bool directory, consecutive;
            uint32_t start, size;
            int16_t parent;
        };
        std::vector<Entry> entries;
        std::vector<uint8_t> block(0x1000);
        for (uint32_t b = 0; b < tableBlocks; b++)
        {
            in.seekg(std::streamoff(blockOffset(tableStart + b)));
            if (!in.read(reinterpret_cast<char*>(block.data()), block.size()))
                return "The game file's file table could not be read.";
            for (uint32_t o = 0; o < 0x1000; o += 0x40)
            {
                const uint8_t* e = &block[o];
                uint32_t nameLength = e[0x28] & 0x3F;
                if (!nameLength)
                    continue;
                entries.push_back({ std::string(reinterpret_cast<const char*>(e), nameLength), bool(e[0x28] & 0x80),
                                    bool(e[0x28] & 0x40), Le24(e + 0x2F), Be32(e + 0x34), int16_t(e[0x32] << 8 | e[0x33]) });
            }
        }

        uint64_t totalBytes = 0;
        for (const Entry& e : entries)
        {
            if (e.directory)
                continue;
            // Note: read-only XBLA packages store every file in consecutive blocks; following the
            // hash-table chain would add support for fragmented (save-style) packages.
            if (!e.consecutive && e.size > 0x1000)
                return "This game file stores its files in a layout the installer does not support yet.";
            totalBytes += e.size;
        }

        // Names come from the package, which may be hostile: each must be one plain path component, and
        // each parent a directory entry (or -1, the root), so nothing can land outside the install.
        for (const Entry& e : entries)
        {
            const bool plain = e.name != "." && e.name != ".." && e.name.find_first_of(std::string("/\\:\0", 4)) == std::string::npos;
            const bool parentOk = e.parent == -1 || (e.parent >= 0 && size_t(e.parent) < entries.size() && entries[e.parent].directory);
            if (!plain || !parentOk)
                return "This game file's file table is damaged.";
        }
        auto pathOf = [&](size_t i) {
            std::vector<const std::string*> names;
            for (int index = int(i); index >= 0 && names.size() < 64; index = entries[index].parent)
                names.push_back(&entries[index].name);
            fs::path p;
            for (auto it = names.rbegin(); it != names.rend(); ++it)
                p /= **it;
            return p;
        };

        std::error_code error;
        const fs::path staging = gameDir.string() + ".partial";
        fs::remove_all(staging, error);
        fs::create_directories(staging, error);
        // Whatever goes wrong below, no partial install is left behind.
        struct RemoveStaging
        {
            const fs::path& path;
            bool keep = false;
            ~RemoveStaging() { std::error_code ignored; if (!keep) fs::remove_all(path, ignored); }
        } removeStaging{ staging };
        uint64_t doneBytes = 0;
        for (size_t i = 0; i < entries.size(); i++)
        {
            const Entry& e = entries[i];
            const fs::path target = (staging / pathOf(i)).lexically_normal();
            if (std::mismatch(staging.begin(), staging.end(), target.begin(), target.end()).first != staging.end())
                return "This game file's file table is damaged.";
            if (e.directory)
            {
                fs::create_directories(target, error);
                continue;
            }
            fs::create_directories(target.parent_path(), error);
            std::ofstream out(target, std::ios::binary);
            for (uint32_t written = 0, k = 0; written < e.size; k++)
            {
                in.seekg(std::streamoff(blockOffset(e.start + k)));
                uint32_t chunk = std::min<uint32_t>(0x1000, e.size - written);
                if (!in.read(reinterpret_cast<char*>(block.data()), chunk) || !out.write(reinterpret_cast<const char*>(block.data()), chunk))
                    return fmt::format("Copying {} failed. Check free disk space.", pathOf(i).string());
                written += chunk;
                doneBytes += chunk;
                if ((k & 0xFF) == 0 && totalBytes)
                    progress(double(doneBytes) / double(totalBytes));
            }
            out.close();
            if (out.fail())
                return fmt::format("Copying {} failed. Check free disk space.", pathOf(i).string());
        }

        if (Sha256(staging / "Default.xex") != ExecutableSha256)
            return "This is a different release of Peggle 2. This build supports the retail Xbox Live Arcade release.";
        // The package's title thumbnail (PNG) becomes the app icon; no game art ships with the app.
        uint32_t thumbnailSize = Be32(&header[0x1716]);
        if (thumbnailSize && thumbnailSize <= 0x4000)
        {
            std::vector<char> png(thumbnailSize);
            in.clear();
            in.seekg(0x571A);
            if (in.read(png.data(), png.size()))
                std::ofstream(staging / "icon.png", std::ios::binary).write(png.data(), png.size());
        }
        {
            std::ofstream marker(staging / ".installed");
            marker << ExecutableSha256 << "\n";
            marker.close();
            if (marker.fail())
                return "The installation could not be completed. Check free disk space.";
        }
        // Swap in: the previous install is only removed once the new one is in place.
        const fs::path previous = gameDir.string() + ".previous";
        fs::remove_all(previous, error);
        error.clear();
        const bool hadPrevious = fs::exists(gameDir, error);
        if (hadPrevious)
        {
            fs::rename(gameDir, previous, error);
            if (error)
                return "The previous installation could not be replaced: " + error.message();
        }
        fs::rename(staging, gameDir, error);
        if (error)
        {
            std::error_code ignored;
            if (hadPrevious)
                fs::rename(previous, gameDir, ignored);
            return "The game could not be moved into place: " + error.message();
        }
        removeStaging.keep = true;
        fs::remove_all(previous, error);
        progress(1.0);
        Log("Installed game from {}", package.string());
        return {};
    }
}
