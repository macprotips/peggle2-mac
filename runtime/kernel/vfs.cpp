#include "vfs.h"
#include "../log.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

namespace rt
{
    struct MountPoint
    {
        std::filesystem::path root;
        bool writable;
    };

    static std::mutex g_vfsMutex;
    static std::map<std::string, MountPoint> g_mounts;      // lower-case device -> mount
    static std::map<std::string, std::string> g_links;      // lower-case link -> target

    static std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(tolower(c)); });
        return s;
    }

    static std::string Normalize(std::string path)
    {
        std::replace(path.begin(), path.end(), '/', '\\');
        if (path.rfind("\\??\\", 0) == 0)
            path = path.substr(4);
        while (path.size() > 1 && path.back() == '\\')
            path.pop_back();
        return path;
    }

    void Mount(const std::string& device, const std::filesystem::path& root, bool writable)
    {
        std::lock_guard lock(g_vfsMutex);
        g_mounts[Lower(Normalize(device))] = { root, writable };
    }

    void CreateSymbolicLink(const std::string& link, const std::string& target)
    {
        std::lock_guard lock(g_vfsMutex);
        g_links[Lower(Normalize(link))] = Normalize(target);
    }

    void DeleteSymbolicLink(const std::string& link)
    {
        std::lock_guard lock(g_vfsMutex);
        g_links.erase(Lower(Normalize(link)));
    }

    // Longest case-insensitive prefix match that ends on a separator or the end of the path.
    template<typename Map>
    static typename Map::const_iterator MatchPrefix(const Map& map, const std::string& lowerPath)
    {
        auto best = map.end();
        size_t bestLength = 0;
        for (auto it = map.begin(); it != map.end(); ++it)
        {
            const std::string& prefix = it->first;
            if (prefix.size() <= bestLength || lowerPath.compare(0, prefix.size(), prefix) != 0)
                continue;
            if (lowerPath.size() > prefix.size() && lowerPath[prefix.size()] != '\\' && prefix.back() != ':' && prefix.back() != '\\')
                continue;
            best = it;
            bestLength = prefix.size();
        }
        return best;
    }

    static std::filesystem::path FindCaseInsensitive(const std::filesystem::path& root, const std::vector<std::string>& parts)
    {
        std::filesystem::path current = root;
        for (const std::string& part : parts)
        {
            std::filesystem::path exact = current / part;
            std::error_code ec;
            if (std::filesystem::exists(exact, ec) || !std::filesystem::is_directory(current, ec))
            {
                current = exact;
                continue;
            }
            std::filesystem::path match = exact;
            for (auto& entry : std::filesystem::directory_iterator(current, ec))
            {
                if (Lower(entry.path().filename().string()) == Lower(part))
                {
                    match = entry.path();
                    break;
                }
            }
            current = match;
        }
        return current;
    }

    std::optional<ResolvedPath> ResolvePath(const std::string& guestPath)
    {
        std::lock_guard lock(g_vfsMutex);
        std::string path = Normalize(guestPath);

        // Follow symbolic links (a few levels, links may point at other links).
        for (int depth = 0; depth < 8; depth++)
        {
            auto link = MatchPrefix(g_links, Lower(path));
            if (link == g_links.end())
                break;
            path = link->second + path.substr(link->first.size());
        }

        auto mount = MatchPrefix(g_mounts, Lower(path));
        if (mount == g_mounts.end())
            return std::nullopt;

        std::vector<std::string> parts;
        std::string rest = path.substr(mount->first.size());
        size_t start = 0;
        while (start <= rest.size())
        {
            size_t end = rest.find('\\', start);
            if (end == std::string::npos)
                end = rest.size();
            std::string part = rest.substr(start, end - start);
            if (part == "..")
            {
                if (!parts.empty())
                    parts.pop_back();
            }
            else if (!part.empty() && part != ".")
                parts.push_back(part);
            start = end + 1;
        }
        return ResolvedPath{ FindCaseInsensitive(mount->second.root, parts), mount->second.writable };
    }
}
