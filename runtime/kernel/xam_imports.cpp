// XAM imports: profiles and sign-in, content (saves), messages, achievements and system UI.
#include "xam.h"
#include "../achievements.h"
#include "../atomic_file.h"
#include "../platform/app.h"
#include "../platform/input.h"
#include "../thread.h"
#include "objects.h"
#include "vfs.h"
#include "xbox_types.h"
#include <cstring>
#include <deque>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace rt
{
    static std::filesystem::path g_userData;

    // --- Overlapped requests -------------------------------------------------------------------

    struct XamOverlapped
    {
        be<uint32_t> result;
        be<uint32_t> length;
        be<uint32_t> context;
        be<uint32_t> event;
        be<uint32_t> completionRoutine;
        be<uint32_t> completionContext;
        be<uint32_t> extendedError;
    };

    uint32_t CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t length)
    {
        auto* o = Translate<XamOverlapped>(overlapped);
        o->result = result;
        o->length = length;
        o->extendedError = result ? (0x80070000 | result) : 0;
        if (o->completionRoutine)
            Log("XAM overlapped completion routine {:X} not supported", uint32_t(o->completionRoutine));
        if (o->event)
        {
            if (uint32_t object = HandleToObject(o->event))
                SetEvent(object);
        }
        return ERROR_IO_PENDING;
    }

    static uint32_t Finish(uint32_t overlapped, uint32_t result, uint32_t length = 0)
    {
        return overlapped ? CompleteOverlapped(overlapped, result, length) : result;
    }

    // --- Notifications -------------------------------------------------------------------------

    struct NotifyListener : HostObject
    {
        uint64_t mask = 0;
        uint32_t maxVersion = 0;
        uint32_t event = 0; // guest notification event, signaled while notifications are pending
        std::deque<std::pair<uint32_t, uint32_t>> queue;
    };

    static std::mutex g_notifyMutex;
    static std::vector<std::shared_ptr<NotifyListener>> g_listeners;
    static bool g_startupNotified;

    static void Enqueue(NotifyListener& listener, uint32_t id, uint32_t data)
    {
        const uint32_t area = (id >> 25) & 0x3F;
        const uint32_t version = (id >> 16) & 0x1FF;
        if (!(listener.mask & (1ull << area)) || version > listener.maxVersion)
            return;
        listener.queue.emplace_back(id, data);
        SetEvent(listener.event);
    }

    void SystemUIShown(bool shown)
    {
        static std::atomic<int> open;
        const int now = shown ? ++open : --open;
        if ((shown && now == 1) || (!shown && now == 0))
            BroadcastNotification(0x09, shown ? 1 : 0); // XN_SYS_UI
    }

    void BroadcastNotification(uint32_t id, uint32_t data)
    {
        std::lock_guard lock(g_notifyMutex);
        for (auto& listener : g_listeners)
            Enqueue(*listener, id, data);
    }

    static uint32_t XamNotifyCreateListener(uint64_t mask, uint32_t maxVersion)
    {
        auto listener = std::make_shared<NotifyListener>();
        listener->mask = mask;
        listener->maxVersion = std::min<uint32_t>(maxVersion, 10);
        listener->event = CreateDispatcherObject(TypeNotificationEvent, 0x10, 0);

        std::lock_guard lock(g_notifyMutex);
        // Titles expect the system's startup notifications on their first system-area listener.
        if (!g_startupNotified && (mask & 1))
        {
            g_startupNotified = true;
            for (auto [id, data] : { std::pair{ 0x09u, 1u }, { 0x09u, 0u }, { 0x0Au, 1u }, { 0x0Au, 1u }, { 0x12u, 0u }, { 0x12u, 0u }, { 0x13u, 0u }, { 0x13u, 0u } })
                Enqueue(*listener, id, data);
        }
        g_listeners.push_back(listener);
        return CreateHandle(listener->event, listener);
    }

    static uint32_t XNotifyGetNext(uint32_t handle, uint32_t matchId, be<uint32_t>* id, be<uint32_t>* param)
    {
        if (param)
            *param = 0;
        if (!id)
            return 0;
        *id = 0;
        auto listener = HandleTo<NotifyListener>(handle);
        if (!listener)
            return 0;

        std::lock_guard lock(g_notifyMutex);
        for (auto it = listener->queue.begin(); it != listener->queue.end(); ++it)
        {
            if (matchId && it->first != matchId)
                continue;
            *id = it->first;
            if (param)
                *param = it->second;
            listener->queue.erase(it);
            if (listener->queue.empty())
                ResetEvent(listener->event);
            return 1;
        }
        return 0;
    }

    // --- The signed-in local profile -----------------------------------------------------------

    constexpr uint64_t PlayerXuid = 0xE000000050474C32ull; // offline local profile
    constexpr const char* PlayerName = "Player";
    // Player 2 (a second controller) is a local guest: signed in, default settings, nothing saved.
    constexpr uint64_t Player2Xuid = 0xE000000050474C33ull;
    constexpr const char* Player2Name = "Player 2";

    static bool IsSignedIn(uint32_t userIndex)
    {
        return userIndex == 0 || (userIndex == 1 && IsPlayer2Connected());
    }

    static uint32_t XamUserGetSigninState(uint32_t userIndex)
    {
        return IsSignedIn(userIndex) ? 1 : 0; // signed in locally
    }

    struct SigninInfo
    {
        be<uint64_t> xuid;
        be<uint32_t> unk08;
        be<uint32_t> signinState;
        be<uint32_t> unk10;
        be<uint32_t> unk14;
        char name[16];
    };
    static_assert(sizeof(SigninInfo) == 40);

    static uint32_t XamUserGetSigninInfo(uint32_t userIndex, uint32_t, SigninInfo* info)
    {
        if (!info)
            return E_INVALIDARG;
        memset(info, 0, sizeof(*info));
        if (!IsSignedIn(userIndex))
            return X_E_NO_SUCH_USER;
        info->xuid = userIndex ? Player2Xuid : PlayerXuid;
        info->signinState = 1;
        strncpy(info->name, userIndex ? Player2Name : PlayerName, sizeof(info->name) - 1);
        return 0;
    }

    static uint32_t XamUserGetXUID(uint32_t userIndex, uint32_t, be<uint64_t>* xuid)
    {
        if (!IsSignedIn(userIndex))
            return X_E_NO_SUCH_USER;
        *xuid = userIndex ? Player2Xuid : PlayerXuid;
        return 0;
    }

    static uint32_t XamUserGetName(uint32_t userIndex, char* buffer, uint32_t length)
    {
        if (!IsSignedIn(userIndex))
            return ERROR_NO_SUCH_USER;
        if (length)
        {
            strncpy(buffer, userIndex ? Player2Name : PlayerName, length - 1);
            buffer[length - 1] = 0;
        }
        return ERROR_SUCCESS;
    }

    static uint32_t XamUserCheckPrivilege(uint32_t userIndex, uint32_t, be<uint32_t>* allowed)
    {
        if (!IsSignedIn(userIndex))
            return ERROR_NO_SUCH_USER;
        *allowed = 1;
        return ERROR_SUCCESS;
    }

    static uint32_t XamUserAreUsersFriends(uint32_t, uint32_t, uint32_t, be<uint32_t>* result, uint32_t overlapped)
    {
        if (result)
            *result = 0;
        return Finish(overlapped, ERROR_SUCCESS);
    }

    static uint32_t XamUserGetDeviceContext(uint32_t userIndex, uint32_t, be<uint32_t>* out)
    {
        // Each player uses the controller with their own index.
        if (out)
            *out = userIndex;
        return IsSignedIn(userIndex) ? 0 : X_E_NO_SUCH_USER;
    }

    // Profile settings. IDs encode type (bits 28-31) and size (bits 16-27). Values the title writes are
    // persisted, which matters because titles keep progress in their title-specific settings.
    struct ProfileSettingData
    {
        uint8_t type;
        uint8_t pad[3];
        be<uint32_t> unk4;
        union
        {
            be<int32_t> s32;
            be<int64_t> s64;
            be<uint64_t> u64;
            struct
            {
                be<uint32_t> size;
                be<uint32_t> ptr;
            } blob;
        };
    };
    static_assert(sizeof(ProfileSettingData) == 16);

    struct ProfileSetting
    {
        be<uint32_t> from;      // 0 default, 1 user, 2 title
        be<uint32_t> unk04;
        be<uint64_t> xuidOrUserIndex;
        be<uint32_t> settingId;
        be<uint32_t> unk14;
        ProfileSettingData data;
    };
    static_assert(sizeof(ProfileSetting) == 40);

    enum : uint32_t { SettingWString = 4, SettingBinary = 6 };

    static std::mutex g_profileMutex;
    static std::map<uint32_t, std::vector<uint8_t>> g_profile; // setting id -> raw value (big-endian as written)

    static std::filesystem::path ProfilePath() { return g_userData / "profile.bin"; }

    // Records of (setting id, size, value). Settings are at most 0xFFF bytes; a damaged file stops at
    // the first record that does not fit.
    static void LoadProfile()
    {
        std::ifstream in(ProfilePath(), std::ios::binary);
        uint32_t id, size;
        while (in.read(reinterpret_cast<char*>(&id), 4) && in.read(reinterpret_cast<char*>(&size), 4) && size <= 0xFFF)
        {
            std::vector<uint8_t> value(size);
            if (!in.read(reinterpret_cast<char*>(value.data()), size))
                break;
            g_profile[id] = std::move(value);
        }
    }

    static void SaveProfile()
    {
        std::string bytes;
        for (auto& [id, value] : g_profile)
        {
            const uint32_t size = uint32_t(value.size());
            bytes.append(reinterpret_cast<const char*>(&id), 4);
            bytes.append(reinterpret_cast<const char*>(&size), 4);
            bytes.append(reinterpret_cast<const char*>(value.data()), size);
        }
        if (!WriteFileAtomically(ProfilePath(), bytes))
            Log("Profile: could not save {}; the previous version is kept", ProfilePath().string());
    }

    static uint32_t XamUserReadProfileSettings(uint32_t, uint32_t userIndex, uint32_t xuidCount, be<uint64_t>*, uint32_t settingCount,
        be<uint32_t>* settingIds, be<uint32_t>* bufferSize, uint8_t* buffer, uint32_t overlapped)
    {
        if (settingCount == 0 || settingCount > 32 || !bufferSize)
            return ERROR_INVALID_PARAMETER;
        const uint32_t entries = settingCount * std::max<uint32_t>(xuidCount, 1);

        uint32_t dataSize = 0;
        for (uint32_t i = 0; i < settingCount; i++)
        {
            uint32_t id = settingIds[i];
            uint32_t type = id >> 28;
            if (type == SettingWString || type == SettingBinary)
                dataSize += (id >> 16) & 0xFFF;
        }
        const uint32_t headerSize = 8 + entries * sizeof(ProfileSetting);
        const uint32_t needed = headerSize + dataSize * std::max<uint32_t>(xuidCount, 1);
        if (!buffer || *bufferSize < needed)
        {
            *bufferSize = needed;
            return ERROR_INSUFFICIENT_BUFFER;
        }
        if (!xuidCount && !IsSignedIn(userIndex))
            return Finish(overlapped, ERROR_NO_SUCH_USER);
        const bool guest = !xuidCount && userIndex != 0; // player 2 reads defaults

        memset(buffer, 0, needed);
        reinterpret_cast<be<uint32_t>*>(buffer)[0] = entries;
        reinterpret_cast<be<uint32_t>*>(buffer)[1] = ToGuest(buffer + 8);
        auto* out = reinterpret_cast<ProfileSetting*>(buffer + 8);
        uint32_t dataOffset = headerSize;

        std::lock_guard lock(g_profileMutex);
        for (uint32_t e = 0; e < entries; e++)
        {
            uint32_t id = settingIds[e % settingCount];
            uint32_t type = id >> 28;
            uint32_t size = (id >> 16) & 0xFFF;
            ProfileSetting& s = out[e];
            s.xuidOrUserIndex = xuidCount ? PlayerXuid : userIndex;
            s.settingId = id;
            s.data.type = uint8_t(type);

            auto stored = guest ? g_profile.end() : g_profile.find(id);
            s.from = stored != g_profile.end() ? 2 : 0;
            if (type == SettingWString || type == SettingBinary)
            {
                uint32_t n = stored != g_profile.end() ? std::min<uint32_t>(size, uint32_t(stored->second.size())) : 0;
                if (n)
                    memcpy(buffer + dataOffset, stored->second.data(), n);
                s.data.blob.size = n;
                s.data.blob.ptr = ToGuest(buffer + dataOffset);
                dataOffset += size;
            }
            else if (stored != g_profile.end() && stored->second.size() == 8)
                memcpy(&s.data.u64, stored->second.data(), 8);
        }
        return Finish(overlapped, ERROR_SUCCESS);
    }

    static uint32_t XamUserWriteProfileSettings(uint32_t, uint32_t userIndex, uint32_t settingCount, ProfileSetting* settings, uint32_t overlapped)
    {
        if (!IsSignedIn(userIndex))
            return Finish(overlapped, ERROR_NO_SUCH_USER);
        if (userIndex != 0)
            return Finish(overlapped, ERROR_SUCCESS); // the guest's settings are not kept
        {
            std::lock_guard lock(g_profileMutex);
            for (uint32_t i = 0; i < settingCount; i++)
            {
                const ProfileSetting& s = settings[i];
                uint32_t id = s.settingId;
                uint32_t type = id >> 28;
                std::vector<uint8_t> value;
                if (type == SettingWString || type == SettingBinary)
                {
                    const uint8_t* p = Translate<uint8_t>(s.data.blob.ptr);
                    value.assign(p, p + uint32_t(s.data.blob.size));
                }
                else
                {
                    value.resize(8);
                    memcpy(value.data(), &s.data.u64, 8);
                }
                g_profile[id] = std::move(value);
            }
            SaveProfile();
        }
        return Finish(overlapped, ERROR_SUCCESS);
    }

    // --- Enumerators: achievements from the title's table and this profile's unlocks; stats empty ---

    struct Enumerator : HostObject
    {
        std::vector<std::vector<uint8_t>> items; // each item's bytes, strings following the fixed part
        size_t itemSize = 0;                     // fixed part, laid out first in the buffer
        size_t next = 0;
    };

    // Appends UTF-8 text as null-terminated UTF-16BE.
    static void AppendUtf16BE(std::vector<uint8_t>& out, const std::string& text)
    {
        auto put = [&](uint32_t unit) { out.push_back(uint8_t(unit >> 8)); out.push_back(uint8_t(unit)); };
        for (size_t i = 0; i < text.size();)
        {
            const uint8_t c = uint8_t(text[i]);
            const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
            uint32_t code = extra ? c & (0x3F >> extra) : c;
            for (int k = 1; k <= extra && i + k < text.size(); k++)
                code = code << 6 | (uint8_t(text[i + k]) & 0x3F);
            i += 1 + extra;
            if (code >= 0x10000)
            {
                code -= 0x10000;
                put(0xD800 | (code >> 10));
                put(0xDC00 | (code & 0x3FF));
            }
            else
                put(code);
        }
        put(0);
    }

    // XACHIEVEMENT_DETAILS (0x24 bytes: id, label, description, unachieved, image id, gamerscore,
    // unlock FILETIME, flags), followed in the buffer by the requested strings.
    constexpr uint32_t AchievementDetailsSize = 0x24;
    constexpr uint32_t AchievementStringsSize = 3 * 2 * 256; // label, description, unachieved
    constexpr uint32_t DetailsAchieved = 0x20000;

    static uint32_t XamUserCreateAchievementEnumerator(uint32_t, uint32_t, uint32_t, uint32_t flags, uint32_t offset, uint32_t count, be<uint32_t>* bufferSize, be<uint32_t>* handle)
    {
        if (bufferSize)
            *bufferSize = count * (AchievementDetailsSize + ((flags & 7) ? AchievementStringsSize : 0));
        auto enumerator = std::make_shared<Enumerator>();
        enumerator->itemSize = AchievementDetailsSize;
        const auto& all = achievements::All();
        for (size_t i = offset; i < all.size() && i < size_t(offset) + count; i++)
        {
            const achievements::Achievement& a = all[i];
            std::vector<uint8_t> item(AchievementDetailsSize);
            auto put32 = [&](size_t at, uint32_t v) { *reinterpret_cast<be<uint32_t>*>(item.data() + at) = v; };
            put32(0x00, a.id);
            put32(0x10, a.imageId);
            put32(0x14, a.gamerscore);
            const int64_t unlocked = achievements::UnlockTime(a.id);
            const uint64_t fileTime = unlocked ? (uint64_t(unlocked) + 11644473600ull) * 10000000ull : 0;
            put32(0x18, uint32_t(fileTime >> 32));
            put32(0x1C, uint32_t(fileTime));
            put32(0x20, a.flags | (unlocked ? DetailsAchieved : 0));
            // Requested strings follow; their offsets (0x04 label, 0x08 description, 0x0C unachieved) are
            // made absolute when the item is copied out.
            const std::string* strings[3] = { &a.name, &a.description, &a.unachieved };
            for (int s = 0; s < 3; s++)
            {
                if (!(flags & (1u << s)))
                    continue;
                put32(0x04 + 4 * s, uint32_t(item.size()));
                AppendUtf16BE(item, *strings[s]);
            }
            enumerator->items.push_back(std::move(item));
        }
        *handle = CreateHandle(0, enumerator);
        return ERROR_SUCCESS;
    }

    static uint32_t XamUserCreateStatsEnumerator(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t count, be<uint32_t>* bufferSize, be<uint32_t>* handle)
    {
        if (bufferSize)
            *bufferSize = count * 0x40;
        if (handle)
            *handle = CreateHandle(0, std::make_shared<Enumerator>());
        return ERROR_SUCCESS;
    }

    // Fills the buffer with as many items as fit: fixed parts first, their strings after them.
    static uint32_t XamEnumerate(uint32_t handle, uint32_t, uint8_t* buffer, uint32_t bufferSize, be<uint32_t>* itemsReturned, uint32_t overlapped)
    {
        auto enumerator = HandleTo<Enumerator>(handle);
        uint32_t returned = 0;
        if (enumerator && buffer)
        {
            const size_t remaining = enumerator->items.size() - enumerator->next;
            size_t fit = std::min<size_t>(remaining, enumerator->itemSize ? bufferSize / enumerator->itemSize : 0);
            auto stringBytes = [&](size_t n) {
                size_t total = 0;
                for (size_t i = 0; i < n; i++)
                    total += enumerator->items[enumerator->next + i].size() - enumerator->itemSize;
                return total;
            };
            while (fit && fit * enumerator->itemSize + stringBytes(fit) > bufferSize)
                fit--;
            uint32_t strings = uint32_t(fit * enumerator->itemSize);
            for (size_t i = 0; i < fit; i++)
            {
                const std::vector<uint8_t>& item = enumerator->items[enumerator->next + i];
                uint8_t* out = buffer + i * enumerator->itemSize;
                memcpy(out, item.data(), enumerator->itemSize);
                memcpy(buffer + strings, item.data() + enumerator->itemSize, item.size() - enumerator->itemSize);
                for (size_t field = 0x04; field <= 0x0C && enumerator->itemSize == AchievementDetailsSize; field += 4)
                {
                    auto* pointer = reinterpret_cast<be<uint32_t>*>(out + field);
                    if (uint32_t at = *pointer)
                        *pointer = ToGuest(buffer) + strings + (at - enumerator->itemSize);
                }
                strings += uint32_t(item.size() - enumerator->itemSize);
            }
            enumerator->next += fit;
            returned = uint32_t(fit);
        }
        if (itemsReturned)
            *itemsReturned = returned;
        return Finish(overlapped, returned ? ERROR_SUCCESS : ERROR_NO_MORE_FILES);
    }

    // --- Content (saves) -----------------------------------------------------------------------

    struct ContentData
    {
        be<uint32_t> deviceId;
        be<uint32_t> contentType;
        be<uint16_t> displayName[128];
        char fileName[42];
        uint8_t padding[2];
    };
    static_assert(sizeof(ContentData) == 0x134);

    constexpr uint32_t HardDriveDeviceId = 1;

    static std::mutex g_replacedMutex;
    static std::map<std::string, std::filesystem::path> g_replaced; // root name -> old content kept aside

    // A ".replaced" folder left from a session that ended mid-save: the content it replaced was never
    // finished, so the previous version comes back.
    static void RecoverInterruptedContent()
    {
        std::error_code ec;
        for (auto& type : std::filesystem::directory_iterator(g_userData / "content", ec))
        {
            for (auto& entry : std::filesystem::directory_iterator(type.path(), ec))
            {
                const std::string name = entry.path().filename().string();
                if (!name.ends_with(".replaced"))
                    continue;
                const std::filesystem::path content = entry.path().parent_path() / name.substr(0, name.size() - 9);
                Log("Content {}: restoring the version before an interrupted save", content.string());
                std::filesystem::remove_all(content, ec);
                std::filesystem::rename(entry.path(), content, ec);
            }
        }
    }

    static std::filesystem::path ContentDirectory(const ContentData* data)
    {
        std::string name(data->fileName, strnlen(data->fileName, sizeof(data->fileName)));
        return g_userData / "content" / fmt::format("{:08X}", uint32_t(data->contentType)) / name;
    }

    static uint32_t XamContentCreateEx(uint32_t, const char* rootName, ContentData* data, uint32_t flags, be<uint32_t>* disposition,
        be<uint32_t>* licenseMask, uint32_t, uint64_t, uint32_t overlapped)
    {
        // flags & 0xF: 1 CREATE_NEW, 2 CREATE_ALWAYS, 3 OPEN_EXISTING, 4 OPEN_ALWAYS, 5 TRUNCATE_EXISTING
        if (!rootName)
            return Finish(overlapped, ERROR_INVALID_PARAMETER);
        std::filesystem::path dir = ContentDirectory(data);
        uint32_t mode = flags & 0xF;
        std::error_code existsError;
        bool exists = std::filesystem::exists(dir, existsError);
        uint32_t result = ERROR_SUCCESS;
        uint32_t created = 0;

        if (mode == 1 && exists)
            result = ERROR_ALREADY_EXISTS;
        else if ((mode == 3 || mode == 5) && !exists)
            result = ERROR_PATH_NOT_FOUND;
        else
        {
            std::error_code ec;
            // Replacing content keeps the old version aside until the new one is closed; an interrupted
            // save is rolled back on the next launch (RecoverInterruptedContent).
            if ((mode == 2 || mode == 5) && exists)
            {
                const std::filesystem::path replaced = dir.string() + ".replaced";
                std::filesystem::remove_all(replaced, ec);
                std::filesystem::rename(dir, replaced, ec);
                if (ec)
                    std::filesystem::remove_all(dir, ec);
                std::lock_guard lock(g_replacedMutex);
                g_replaced[rootName] = replaced;
            }
            created = std::filesystem::create_directories(dir, ec) ? 1 : 0;
            Mount(std::string(rootName) + ":", dir, true);
            Log("Content {}: mounted {} ({})", rootName, dir.string(), created ? "created" : "opened");
        }
        if (disposition)
            *disposition = created ? 1 : 2;
        if (licenseMask)
            *licenseMask = 0xFFFFFFFF;
        return Finish(overlapped, result);
    }

    static uint32_t XamContentClose(const char* rootName, uint32_t overlapped)
    {
        // The mount stays registered; reopening the same content replaces it.
        Log("Content {}: closed", rootName ? rootName : "");
        std::lock_guard lock(g_replacedMutex);
        if (auto it = g_replaced.find(rootName ? rootName : ""); it != g_replaced.end())
        {
            std::error_code ec;
            std::filesystem::remove_all(it->second, ec);
            g_replaced.erase(it);
        }
        return Finish(overlapped, ERROR_SUCCESS);
    }

    static uint32_t XamContentDelete(uint32_t, ContentData* data, uint32_t overlapped)
    {
        std::error_code ec;
        std::filesystem::remove_all(ContentDirectory(data), ec);
        return Finish(overlapped, ERROR_SUCCESS);
    }

    struct ContentDeviceData
    {
        be<uint32_t> deviceId;
        be<uint32_t> deviceType;
        be<uint64_t> totalBytes;
        be<uint64_t> freeBytes;
        be<uint16_t> name[28];
    };
    static_assert(sizeof(ContentDeviceData) == 0x50);

    static uint32_t XamContentGetDeviceData(uint32_t deviceId, ContentDeviceData* data)
    {
        if (deviceId != HardDriveDeviceId)
            return ERROR_DEVICE_NOT_CONNECTED;
        memset(data, 0, sizeof(*data));
        data->deviceId = HardDriveDeviceId;
        data->deviceType = 1;
        data->totalBytes = 20ull << 30;
        data->freeBytes = 10ull << 30;
        const char* name = "Hard Drive";
        for (size_t i = 0; name[i]; i++)
            data->name[i] = uint16_t(name[i]);
        return ERROR_SUCCESS;
    }

    static uint32_t XamContentGetLicenseMask(be<uint32_t>* mask, uint32_t overlapped)
    {
        *mask = 0xFFFFFFFF; // full game
        return Finish(overlapped, ERROR_SUCCESS);
    }

    static uint32_t XamShowDeviceSelectorUI(uint32_t, uint32_t, uint32_t, uint64_t, be<uint32_t>* deviceId, uint32_t overlapped)
    {
        *deviceId = HardDriveDeviceId;
        return Finish(overlapped, ERROR_SUCCESS);
    }

    // --- System UI: there is no dashboard overlay; requests complete as if dismissed ------------

    static uint32_t XamShowSigninUI(uint32_t, uint32_t)
    {
        BroadcastNotification(0x0A, 1); // XN_SYS_SIGNINCHANGED
        return ERROR_SUCCESS;
    }

    // Null-terminated UTF-16BE guest text as UTF-8.
    static std::string ReadUtf16BE(uint32_t address)
    {
        std::string out;
        if (!address)
            return out;
        const auto* p = Translate<be<uint16_t>>(address);
        for (size_t i = 0; p[i] != 0; i++)
        {
            uint32_t code = p[i];
            if (code >= 0xD800 && code < 0xDC00 && p[i + 1] >= 0xDC00 && p[i + 1] < 0xE000)
                code = 0x10000 + ((code - 0xD800) << 10) + (uint32_t(p[++i]) - 0xDC00);
            if (code < 0x80)
                out += char(code);
            else if (code < 0x800)
                out += { char(0xC0 | code >> 6), char(0x80 | (code & 0x3F)) };
            else if (code < 0x10000)
                out += { char(0xE0 | code >> 12), char(0x80 | ((code >> 6) & 0x3F)), char(0x80 | (code & 0x3F)) };
            else
                out += { char(0xF0 | code >> 18), char(0x80 | ((code >> 12) & 0x3F)), char(0x80 | ((code >> 6) & 0x3F)), char(0x80 | (code & 0x3F)) };
        }
        return out;
    }

    // A system message box: shown as an alert on the game window. The title is told a system UI is
    // showing until it is answered; the chosen button goes to result.
    PPC_FUNC(XamShowMessageBoxUIExImpl)
    {
        // (user, title, text, buttonCount, buttons, activeButton, flags, result*, overlapped, ...)
        const std::string title = ReadUtf16BE(ctx.r4.u32), text = ReadUtf16BE(ctx.r5.u32);
        std::vector<std::string> buttons;
        for (uint32_t i = 0; i < ctx.r6.u32 && ctx.r7.u32; i++)
            buttons.push_back(ReadUtf16BE(*Translate<be<uint32_t>>(ctx.r7.u32 + i * 4)));
        const uint32_t activeButton = ctx.r8.u32, result = ctx.r10.u32;
        const uint32_t overlapped = uint32_t(IntArg(ctx, 8, 4));
        Log("XamShowMessageBoxUIEx: {} / {}", title, text);

        SystemUIShown(true);
        auto answered = std::make_shared<std::promise<void>>();
        ShowMessageBox(title, text, buttons, int(activeButton), [=](int chosen) {
            if (result)
                *Translate<be<uint32_t>>(result) = uint32_t(chosen);
            SystemUIShown(false);
            if (overlapped)
                CompleteOverlapped(overlapped, ERROR_SUCCESS);
            answered->set_value();
        });
        if (overlapped)
        {
            ctx.r3.u64 = ERROR_IO_PENDING;
            return;
        }
        answered->get_future().wait();
        ctx.r3.u64 = ERROR_SUCCESS;
    }

    static uint32_t ShowUnavailableUI() { return ERROR_SUCCESS; }

    // The Guide's achievements list: a window listing the title's achievements and this profile's unlocks.
    static uint32_t XamShowAchievementsUI(uint32_t)
    {
        ShowAchievements();
        return ERROR_SUCCESS;
    }

    // --- Execution info, sessions, voice, loader -----------------------------------------------

    static uint32_t XamGetExecutionId(be<uint32_t>* info)
    {
        static uint32_t executionInfo;
        if (!executionInfo)
        {
            // XEX execution info: media id, version, base version, title id, platform, executable table, disc, disc count, savegame id.
            executionInfo = g_systemHeap.Alloc(0x18);
            auto* p = Translate<be<uint32_t>>(executionInfo);
            p[0] = 0x4906ADEC;
            p[1] = 0x00000004;
            p[2] = 0x00000004;
            p[3] = 0x58411436;
            *Translate<uint8_t>(executionInfo + 0x10) = 0; // platform
            *Translate<uint8_t>(executionInfo + 0x12) = 1; // disc number
            *Translate<uint8_t>(executionInfo + 0x13) = 1; // disc count
        }
        *info = executionInfo;
        return ERROR_SUCCESS;
    }

    // The title's Exit (back to the dashboard on the console) quits the app.
    static void XamLoaderTerminateTitle()
    {
        Log("XamLoaderTerminateTitle: title exit");
        QuitApplication();
        while (true)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    static uint32_t XamSessionCreateHandle(be<uint32_t>* handle)
    {
        *handle = CreateHandle(CreateDispatcherObject(TypeNotificationEvent, 0x10, 0));
        return ERROR_SUCCESS;
    }

    static uint32_t XamSessionRefObjByHandle(uint32_t handle, be<uint32_t>* object)
    {
        *object = HandleToObject(handle);
        return *object ? ERROR_SUCCESS : ERROR_INVALID_HANDLE;
    }

    static uint32_t XamVoiceIsActiveProcess() { return 0; }
    static uint32_t XamVoiceCreate(uint32_t, uint32_t, be<uint32_t>* voice)
    {
        *voice = 0;
        return ERROR_ACCESS_DENIED; // no voice chat
    }
    static uint32_t XamVoiceClose(uint32_t) { return 0; }
    static uint32_t XamVoiceHeadsetPresent(uint32_t) { return 0; }
    static uint32_t XamVoiceSubmitPacket(uint32_t, uint32_t, uint32_t) { return 0; }

    // --- XMsg: system apps ---------------------------------------------------------------------

    constexpr uint32_t XmpApp = 0xFA;

    static uint32_t XMsgInProcessCall(uint32_t app, uint32_t message, uint32_t arg1, uint32_t arg2)
    {
        if (app == XmpApp && message == 0x0007001B) // XMPGetPlaybackController: the title owns music
        {
            auto* args = Translate<be<uint32_t>>(arg1);
            *Translate<be<uint32_t>>(args[1]) = 0;
            *Translate<be<uint32_t>>(args[2]) = 0;
            return ERROR_SUCCESS;
        }
        constexpr uint32_t XgiApp = 0xFB;
        if (app == XgiApp)
        {
            // Stats, presence context, sessions and achievements go to Xbox Live; offline they succeed.
            if (message == 0x000B0008) // XGIUserWriteAchievements(count, achievements)
            {
                auto* args = Translate<be<uint32_t>>(arg1);
                auto* list = Translate<be<uint32_t>>(args[1]);
                for (uint32_t i = 0; i < args[0]; i++)
                {
                    if (const achievements::Achievement* a = achievements::Unlock(uint16_t(list[i * 3 + 1])))
                    {
                        Log("Achievement unlocked: {} ({}G)", a->name, a->gamerscore);
                        ShowAchievementUnlocked(*a);
                    }
                }
            }
            return ERROR_SUCCESS;
        }
        Log("XMsgInProcessCall(app {:X}, message {:X}, {:X}, {:X}): not handled", app, message, arg1, arg2);
        return ERROR_NOT_FOUND;
    }

    static uint32_t XMsgStartIORequest(uint32_t app, uint32_t message, uint32_t overlapped, uint32_t buffer, uint32_t length)
    {
        return Finish(overlapped, XMsgInProcessCall(app, message, buffer, length));
    }

    static uint32_t XMsgStartIORequestEx(uint32_t app, uint32_t message, uint32_t overlapped, uint32_t buffer, uint32_t length, uint32_t)
    {
        return XMsgStartIORequest(app, message, overlapped, buffer, length);
    }

    static uint32_t XMsgCancelIORequest(uint32_t, uint32_t)
    {
        return ERROR_SUCCESS;
    }

    void InitXam(const std::filesystem::path& userData)
    {
        g_userData = userData;
        std::error_code ec;
        std::filesystem::create_directories(userData / "content", ec);
        RecoverInterruptedContent();
        LoadProfile();
    }
}

using namespace rt;
GUEST_IMPORT(XamNotifyCreateListener, XamNotifyCreateListener)
GUEST_IMPORT(XNotifyGetNext, XNotifyGetNext)
GUEST_IMPORT(XamUserGetSigninState, XamUserGetSigninState)
GUEST_IMPORT(XamUserGetSigninInfo, XamUserGetSigninInfo)
GUEST_IMPORT(XamUserGetXUID, XamUserGetXUID)
GUEST_IMPORT(XamUserGetName, XamUserGetName)
GUEST_IMPORT(XamUserCheckPrivilege, XamUserCheckPrivilege)
GUEST_IMPORT(XamUserAreUsersFriends, XamUserAreUsersFriends)
GUEST_IMPORT(XamUserGetDeviceContext, XamUserGetDeviceContext)
GUEST_IMPORT(XamUserReadProfileSettings, XamUserReadProfileSettings)
GUEST_IMPORT(XamUserWriteProfileSettings, XamUserWriteProfileSettings)
GUEST_IMPORT(XamUserCreateAchievementEnumerator, XamUserCreateAchievementEnumerator)
GUEST_IMPORT(XamUserCreateStatsEnumerator, XamUserCreateStatsEnumerator)
GUEST_IMPORT(XamEnumerate, XamEnumerate)
GUEST_IMPORT(XamContentCreateEx, XamContentCreateEx)
GUEST_IMPORT(XamContentClose, XamContentClose)
GUEST_IMPORT(XamContentDelete, XamContentDelete)
GUEST_IMPORT(XamContentGetDeviceData, XamContentGetDeviceData)
GUEST_IMPORT(XamContentGetLicenseMask, XamContentGetLicenseMask)
GUEST_IMPORT(XamShowDeviceSelectorUI, XamShowDeviceSelectorUI)
GUEST_IMPORT(XamShowSigninUI, XamShowSigninUI)
GUEST_IMPORT(XamShowAchievementsUI, XamShowAchievementsUI)
GUEST_IMPORT(XamShowFriendsUI, ShowUnavailableUI)
GUEST_IMPORT(XamShowGamerCardUIForXUID, ShowUnavailableUI)
GUEST_IMPORT(XamShowMarketplaceUI, ShowUnavailableUI)
GUEST_IMPORT(XamGetExecutionId, XamGetExecutionId)
GUEST_IMPORT(XamLoaderTerminateTitle, XamLoaderTerminateTitle)
GUEST_IMPORT(XamSessionCreateHandle, XamSessionCreateHandle)
GUEST_IMPORT(XamSessionRefObjByHandle, XamSessionRefObjByHandle)
GUEST_IMPORT(XamVoiceIsActiveProcess, XamVoiceIsActiveProcess)
GUEST_IMPORT(XamVoiceCreate, XamVoiceCreate)
GUEST_IMPORT(XamVoiceClose, XamVoiceClose)
GUEST_IMPORT(XamVoiceHeadsetPresent, XamVoiceHeadsetPresent)
GUEST_IMPORT(XamVoiceSubmitPacket, XamVoiceSubmitPacket)
GUEST_IMPORT(XMsgInProcessCall, XMsgInProcessCall)
GUEST_IMPORT(XMsgStartIORequest, XMsgStartIORequest)
GUEST_IMPORT(XMsgStartIORequestEx, XMsgStartIORequestEx)
GUEST_IMPORT(XMsgCancelIORequest, XMsgCancelIORequest)
PPC_FUNC(__imp__XamShowMessageBoxUIEx) { rt::XamShowMessageBoxUIExImpl(ctx, base); }
