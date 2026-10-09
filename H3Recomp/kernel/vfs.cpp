#include "vfs.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

namespace h3::kernel
{
    namespace
    {
        struct Mount
        {
            std::string device;  // e.g. "\Device\Cdrom0"
            std::filesystem::path root;
            bool writable;
        };

        std::mutex g_mutex;
        std::vector<Mount> g_mounts;
        std::map<std::string, std::string> g_links;  // lowercase link -> target

        std::string Lower(std::string_view text)
        {
            std::string lower(text);
            for (char& c : lower)
                c = char(std::tolower(static_cast<unsigned char>(c)));
            return lower;
        }

        void SetMount(std::string device, std::filesystem::path root, bool writable)
        {
            std::lock_guard lock(g_mutex);
            std::erase_if(g_mounts, [&](const Mount& mount) { return EqualsIgnoreCase(mount.device, device); });
            g_mounts.push_back({ std::move(device), std::move(root), writable });
        }

        // Expands a leading "link:" (and drops the NT "\??\" prefix).
        std::string ResolveLinks(std::string_view path)
        {
            if (path.starts_with("\\??\\"))
                path.remove_prefix(4);

            std::string resolved(path);
            for (int depth = 0; depth < 8; depth++)
            {
                size_t colon = resolved.find(':');
                if (colon == std::string::npos || resolved.find('\\') < colon)
                    break;
                auto link = g_links.find(Lower(std::string_view(resolved).substr(0, colon + 1)));
                if (link == g_links.end())
                    break;
                std::string rest = resolved.substr(colon + 1);
                std::string target = link->second;
                if (!target.empty() && target.back() == '\\' && !rest.empty() && rest.front() == '\\')
                    target.pop_back();
                resolved = target + rest;
            }
            return resolved;
        }
    }

    bool EqualsIgnoreCase(std::string_view a, std::string_view b)
    {
        return std::ranges::equal(a, b, [](char x, char y)
        {
            return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
        });
    }

    void SetContentRoot(std::filesystem::path root)
    {
        SetMount("\\Device\\Cdrom0", std::move(root), false);
        AddSymbolicLink("game:", "\\Device\\Cdrom0");
        AddSymbolicLink("d:", "\\Device\\Cdrom0");
    }

    void SetUserDataRoot(std::filesystem::path root)
    {
        SetMount("\\Device\\Harddisk0", std::move(root), true);
    }

    void AddSymbolicLink(std::string_view link, std::string_view target)
    {
        std::lock_guard lock(g_mutex);
        g_links[Lower(link)] = std::string(target);
    }

    bool RemoveSymbolicLink(std::string_view link)
    {
        std::lock_guard lock(g_mutex);
        return g_links.erase(Lower(link)) != 0;
    }

    std::optional<HostPath> MapGuestPath(std::string_view guestPath)
    {
        std::string devicePath;
        std::optional<Mount> mount;
        {
            std::lock_guard lock(g_mutex);
            devicePath = ResolveLinks(guestPath);
            size_t best = 0;
            for (const Mount& candidate : g_mounts)
            {
                size_t length = candidate.device.size();
                bool matches = devicePath.size() >= length && EqualsIgnoreCase(std::string_view(devicePath).substr(0, length), candidate.device)
                    && (devicePath.size() == length || devicePath[length] == '\\');
                if (matches && length > best)
                {
                    best = length;
                    mount = candidate;
                }
            }
        }
        if (!mount)
            return std::nullopt;

        HostPath result;
        result.path = mount->root;
        result.writable = mount->writable;
        result.devicePath = devicePath;

        std::vector<std::string_view> components;
        std::string_view rest = std::string_view(devicePath).substr(mount->device.size());
        while (!rest.empty())
        {
            size_t separator = rest.find('\\');
            std::string_view component = rest.substr(0, separator);
            rest = separator == std::string_view::npos ? std::string_view() : rest.substr(separator + 1);
            if (!component.empty() && component != ".")
                components.push_back(component);
        }

        std::error_code error;
        for (size_t i = 0; i < components.size(); i++)
        {
            std::filesystem::path match;
            for (const auto& entry : std::filesystem::directory_iterator(result.path, error))
            {
                if (EqualsIgnoreCase(entry.path().filename().string(), components[i]))
                {
                    match = entry.path();
                    break;
                }
            }
            if (match.empty())
            {
                if (i + 1 != components.size())
                    return std::nullopt;  // missing parent directory
                result.path /= std::string(components[i]);
                result.exists = false;
                return result;
            }
            result.path = std::move(match);
        }
        result.exists = std::filesystem::exists(result.path, error);
        return result;
    }

    std::optional<std::filesystem::path> ResolveGuestPath(std::string_view guestPath)
    {
        auto mapped = MapGuestPath(guestPath);
        if (!mapped || !mapped->exists)
            return std::nullopt;
        return mapped->path;
    }
}
