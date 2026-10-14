// The tag system, decompiled in place (docs/ROADMAP.md 6.3): C++ that
// still reads the game's tables in guest memory (big-endian, 32-bit guest
// addresses), replacing the recompiled functions one at a time.
//
// The game loads a map's tag section into memory at the cache file's
// virtual base, so the cache file's tag index (H3Native/cache/
// cache_file.h) is in guest memory as is, and two globals point into it:
//
//   kTagIndexHeader  the loaded tag index header (+4 group table, +8 tag
//                    count), cache_file.h's layout
//   kTagInstances    the tag table (8-byte entries: group index, salt,
//                    data address)
//
// Replaced so far:
//
//   sub_82087AF0  tag_get(group, datum): the tag's data address if datum
//                 names a tag of that group (or of a group whose parent or
//                 grandparent it is), else 0. Most of the game inlines the
//                 same lookup; 26 call sites use the function.
//
// Switches:
//   H3_ORIGINAL=tag_get   run the recompiled original instead
//   H3_CHECK_NATIVE=1     run both on every call and report differences
//   H3_CHECK_TAGS=1       when a map's tag index is loaded, compare it
//                         with the native cache file reader's (CheckTagIndex)

#include "tags/tags.h"
#include "cache/cache_file.h"
#include "config/env.h"
#include "native.h"
#include "kernel/vfs.h"
#include "memory.h"

#include <ppc_recomp_shared.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace h3::native::tags
{
    using h3::EnvFlag;
    using h3::EnvString;
    using h3::GuestRef;

    namespace
    {
        constexpr uint32_t kTagIndexHeader = 0x82948F60;  // u32: guest address of the tag index header
        constexpr uint32_t kTagInstances = 0x827CFF0C;    // u32: guest address of the tag table
        constexpr uint32_t kHeaderGroups = 4;             // in the tag index header
        constexpr uint32_t kHeaderTagCount = 8;
        constexpr uint32_t kHeaderChecksum = 0x20;
        constexpr uint32_t kInstanceBytes = 8;   // group index (s16), salt (s16), data address
        constexpr uint32_t kInstanceAddress = 4;
        constexpr uint32_t kGroupBytes = 16;     // tag, parent, grandparent, name
        constexpr uint32_t kSlotMask = 0xFFFF;
    }

    uint32_t LoadedIndexHeader() { return GuestRef<uint32_t>(kTagIndexHeader); }

    uint32_t LoadedIndexChecksum()
    {
        uint32_t header = LoadedIndexHeader();
        return header ? uint32_t(GuestRef<uint32_t>(header + kHeaderChecksum)) : 0;
    }

    const cache::CacheFile* LoadedCacheFile()
    {
        // Every map with a tag index, opened once (headers and tables only).
        static const std::vector<std::unique_ptr<cache::CacheFile>> maps = [] {
            std::vector<std::unique_ptr<cache::CacheFile>> files;
            std::optional<std::filesystem::path> folder = kernel::ResolveGuestPath("game:\\maps");
            if (!folder)
                return files;
            for (const auto& entry : std::filesystem::directory_iterator(*folder))
                if (entry.path().extension() == ".map")
                    if (auto file = cache::CacheFile::Open(entry.path().string()))
                        if (file->GetHeader().tagIndexAddress)
                            files.push_back(std::move(file));
            return files;
        }();
        uint32_t header = LoadedIndexHeader(), checksum = LoadedIndexChecksum();
        for (const auto& map : maps)
            if (header && map->Index().checksum == checksum && map->GetHeader().tagIndexAddress == header)
                return map.get();
        return nullptr;
    }

    // tag_get, as the original computes it: the slot and salt are compared
    // as signed 16-bit values and the slot unsigned against the count, so
    // NONE (0xFFFFFFFF) and out-of-range slots give 0. An empty slot's group
    // index (-1) indexes the group table at -1 like the original does; its
    // salt never matches a live datum, so that read doesn't decide anything.
    uint32_t TagGet(uint32_t groupTag, uint32_t datum)
    {
        uint32_t header = GuestRef<uint32_t>(kTagIndexHeader);
        int32_t slot = int16_t(datum & kSlotMask);
        if (uint32_t(slot) >= uint32_t(GuestRef<uint32_t>(header + kHeaderTagCount)))
            return 0;
        uint32_t instance = GuestRef<uint32_t>(kTagInstances) + uint32_t(slot) * kInstanceBytes;
        if (int16_t(datum >> 16) != int16_t(GuestRef<uint16_t>(instance + 2)))
            return 0;
        int32_t groupIndex = int16_t(GuestRef<uint16_t>(instance));
        uint32_t group = GuestRef<uint32_t>(header + kHeaderGroups) + uint32_t(groupIndex) * kGroupBytes;
        for (uint32_t level = 0; level < 3; level++)  // the group, its parent, its grandparent
            if (GuestRef<uint32_t>(group + level * 4) == groupTag)
                return GuestRef<uint32_t>(instance + kInstanceAddress);
        return 0;
    }

    // H3_CHECK_TAGS: compares the loaded tag index (once per map load) with
    // the native reader's parse of the same cache file, found among
    // game:\maps by its tag index checksum. Called once per presented frame.
    void CheckTagIndex()
    {
        static const bool enabled = EnvFlag("H3_CHECK_TAGS");
        static uint32_t checkedHeader = 0, checkedChecksum = 0;
        if (!enabled)
            return;
        uint32_t header = GuestRef<uint32_t>(kTagIndexHeader);
        if (!header)
            return;
        uint32_t checksum = GuestRef<uint32_t>(header + kHeaderChecksum);
        if (header == checkedHeader && checksum == checkedChecksum)
            return;
        checkedHeader = header;
        checkedChecksum = checksum;

        const cache::CacheFile* file = LoadedCacheFile();
        if (!file)
        {
            fprintf(stderr, "[tags] no map in game:\\maps has a tag index at %08X with checksum %08X\n", header, checksum);
            return;
        }

        const auto& tags = file->Index().tags;
        uint32_t count = GuestRef<uint32_t>(header + kHeaderTagCount), table = GuestRef<uint32_t>(kTagInstances);
        uint32_t differences = 0, checked = 0;
        if (count != tags.size())
            fprintf(stderr, "[tags] %s: %u tags loaded, %zu in the file\n", file->GetHeader().name.c_str(), count, tags.size());
        for (uint32_t slot = 0; slot < count && slot < tags.size(); slot++)
        {
            uint32_t instance = table + slot * kInstanceBytes;
            const cache::TagInstance& native = tags[slot];
            uint16_t groupIndex = GuestRef<uint16_t>(instance), salt = GuestRef<uint16_t>(instance + 2);
            uint32_t address = GuestRef<uint32_t>(instance + kInstanceAddress);
            checked++;
            if (groupIndex == native.groupIndex && salt == native.salt && address == native.address)
            {
                // Both agree on the entry; tag_get must agree with the reader too.
                if (native.Empty())
                    continue;
                uint32_t groupTag = file->Index().groups[native.groupIndex].tag;
                if (TagGet(groupTag, file->Datum(slot)) == native.address)
                    continue;
            }
            if (differences++ < 10)
                fprintf(stderr, "[tags] slot %u: loaded %04X %04X %08X, file %04X %04X %08X (%s)\n", slot, groupIndex, salt,
                    address, native.groupIndex, native.salt, native.address, file->TagName(slot).c_str());
        }
        fprintf(stderr, "[tags] %s: %u of %u tags match the native reader\n", file->GetHeader().name.c_str(), checked - differences,
            checked);
    }
}

// tag_get (r3 = group tag, r4 = datum index; returns the data address).
PPC_FUNC_IMPL(__imp__sub_82087AF0);
PPC_FUNC(sub_82087AF0)
{
    using namespace h3::native::tags;
    static const bool original = h3::native::UseOriginal("tag_get");
    static const bool check = h3::native::CheckNative();
    static h3::native::NativeCheck checker("tag_get");
    if (original)
    {
        __imp__sub_82087AF0(ctx, base);
        return;
    }
    uint32_t group = ctx.r3.u32, datum = ctx.r4.u32;
    uint32_t result = TagGet(group, datum);
    if (check)
    {
        PPCContext copy = ctx;
        __imp__sub_82087AF0(copy, base);
        checker.Compare(copy.r3.u32 == result, [&] {
            char text[96];
            snprintf(text, sizeof(text), "tag_get(%08X, %08X): native %08X, original %08X", group, datum, result, copy.r3.u32);
            return std::string(text);
        });
        result = copy.r3.u32;  // the game gets the original's
    }
    // The original leaves r3 = the result and clobbers only volatile
    // registers, which callers don't rely on.
    ctx.r3.u64 = result;
}
