// The guest file system: maps guest paths (devices and symbolic links like
// d:\, game:\, cache0:\) onto host folders under private/.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace h3::kernel
{
    // Guest file system. Guest paths name a device ("\Device\Cdrom0\maps\x"),
    // or go through a symbolic link ("d:\maps\x", "cache0:\..."). Devices are
    // mounted on host directories:
    //
    //   \Device\Cdrom0             the disc contents (read-only); game: and d:
    //                              link here
    //   \Device\Harddisk0\...      the user data root, writable (cache
    //                              partitions, Partition1)
    //
    // The title adds its own links with ObCreateSymbolicLink.

    // Host folders for the disc contents (game:\, d:\, read-only) and the
    // hard drive (cache partitions, saves; writable). Set before booting.
    void SetContentRoot(std::filesystem::path root);
    void SetUserDataRoot(std::filesystem::path root);

    // ObCreateSymbolicLink / ObDeleteSymbolicLink. Link names are like "d:".
    void AddSymbolicLink(std::string_view link, std::string_view target);
    bool RemoveSymbolicLink(std::string_view link);

    struct HostPath
    {
        std::filesystem::path path;
        bool exists = false;      // the file or directory itself
        bool writable = false;    // on a writable mount
        std::string devicePath;   // the guest path with links resolved
    };

    // Maps a guest path to the host. Path components match case-insensitively,
    // as on the 360; a missing last component keeps the guest's spelling.
    // Returns nothing for unknown devices or when a parent directory is
    // missing.
    std::optional<HostPath> MapGuestPath(std::string_view guestPath);

    // MapGuestPath for files that must exist.
    std::optional<std::filesystem::path> ResolveGuestPath(std::string_view guestPath);

    // ASCII case-insensitive comparison (360 paths ignore case).
    bool EqualsIgnoreCase(std::string_view a, std::string_view b);
}
