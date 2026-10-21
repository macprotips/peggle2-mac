// Achievements: the title's list from its XDBF resource, unlocks, and their saved record.
#include "achievements.h"
#include "atomic_file.h"
#include "log.h"
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>

namespace rt::achievements
{
    static std::vector<Achievement> g_all;
    static std::mutex g_mutex;
    static std::map<uint16_t, int64_t> g_unlocked; // id -> Unix seconds
    static std::filesystem::path g_storage;

    // XDBF is big-endian throughout.
    static uint16_t Read16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
    static uint32_t Read32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
    static uint64_t Read64(const uint8_t* p) { return uint64_t(Read32(p)) << 32 | Read32(p + 4); }

    struct Xdbf
    {
        const uint8_t* data;
        size_t size;

        // The section (namespace 1 metadata, 2 images, 3 string tables) with this id, or empty.
        std::pair<const uint8_t*, size_t> Section(uint16_t ns, uint64_t id) const
        {
            if (size < 24 || memcmp(data, "XDBF", 4) != 0)
                return {};
            const uint32_t tableLength = Read32(data + 8), count = Read32(data + 12), freeLength = Read32(data + 16);
            const size_t dataStart = 24 + size_t(tableLength) * 18 + size_t(freeLength) * 8;
            for (uint32_t i = 0; i < count && 24 + (i + 1) * 18 <= size; i++)
            {
                const uint8_t* e = data + 24 + i * 18;
                const uint32_t offset = Read32(e + 10), length = Read32(e + 14);
                if (Read16(e) == ns && Read64(e + 2) == id && dataStart + offset + length <= size)
                    return { data + dataStart + offset, length };
            }
            return {};
        }

        std::map<uint16_t, std::string> Strings(uint32_t language) const
        {
            std::map<uint16_t, std::string> strings;
            auto [s, length] = Section(3, language);
            if (length < 14 || memcmp(s, "XSTR", 4) != 0)
                return strings;
            size_t p = 14;
            for (uint16_t i = 0, n = Read16(s + 12); i < n && p + 4 <= length; i++)
            {
                const uint16_t id = Read16(s + p), bytes = Read16(s + p + 2);
                if (p + 4 + bytes > length)
                    break;
                strings[id].assign(reinterpret_cast<const char*>(s + p + 4), bytes);
                p += 4 + bytes;
            }
            return strings;
        }
    };

    constexpr uint64_t SectionAchievements = 0x58414348; // 'XACH'
    constexpr uint64_t SectionStringConfig = 0x58535443; // 'XSTC': default language

    void Load(const uint8_t* data, size_t size, uint32_t language, const std::filesystem::path& storage)
    {
        const Xdbf xdbf{ data, size };
        auto [table, length] = xdbf.Section(1, SectionAchievements);
        if (length < 14 || memcmp(table, "XACH", 4) != 0)
        {
            Log("Achievements: no achievement table in the title");
            return;
        }
        auto strings = xdbf.Strings(language);
        if (strings.empty())
        {
            auto [config, configLength] = xdbf.Section(1, SectionStringConfig);
            strings = xdbf.Strings(configLength >= 16 ? Read32(config + 12) : 1);
        }
        auto text = [&](uint16_t id) { auto it = strings.find(id); return it == strings.end() ? std::string() : it->second; };

        const uint16_t count = Read16(table + 12);
        for (uint16_t i = 0; i < count && 14 + (i + 1) * 0x24 <= length; i++)
        {
            const uint8_t* e = table + 14 + i * 0x24;
            Achievement a;
            a.id = Read16(e);
            a.name = text(Read16(e + 2));
            a.description = text(Read16(e + 4));
            a.unachieved = text(Read16(e + 6));
            a.imageId = Read32(e + 8);
            a.gamerscore = Read16(e + 12);
            a.flags = Read32(e + 16);
            auto [image, imageLength] = xdbf.Section(2, a.imageId);
            a.image.assign(image, image + imageLength);
            g_all.push_back(std::move(a));
        }

        g_storage = storage;
        std::ifstream in(storage);
        uint32_t id;
        int64_t time;
        while (in >> id >> time)
            g_unlocked[uint16_t(id)] = time;
        Log("Achievements: {} in the title, {} unlocked", g_all.size(), g_unlocked.size());
    }

    const std::vector<Achievement>& All()
    {
        return g_all;
    }

    int64_t UnlockTime(uint16_t id)
    {
        std::lock_guard lock(g_mutex);
        auto it = g_unlocked.find(id);
        return it == g_unlocked.end() ? 0 : it->second;
    }

    const Achievement* Unlock(uint16_t id)
    {
        const Achievement* achievement = nullptr;
        for (const Achievement& a : g_all)
            if (a.id == id)
                achievement = &a;
        std::lock_guard lock(g_mutex);
        if (!achievement || g_unlocked.count(id))
            return nullptr;
        g_unlocked[id] = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

        std::string contents;
        for (auto [unlockedId, time] : g_unlocked)
            contents += fmt::format("{} {}\n", unlockedId, time);
        if (!WriteFileAtomically(g_storage, contents))
            Log("Achievements: could not save {}", g_storage.string());
        return achievement;
    }
}
