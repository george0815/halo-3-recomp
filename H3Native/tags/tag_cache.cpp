// The tag cache (tag_cache.h): host copies of the loaded map's tags,
// converted from guest memory on first use.

#include "tags/tag_cache.h"

#ifdef H3_HAVE_TAG_LAYOUTS

#include "cache/cache_file.h"
#include "config/env.h"
#include "memory.h"
#include "tags/tags.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

namespace h3::native::tags
{
    namespace
    {
        // The game's loaded tag index in guest memory (tags.cpp has the
        // layout): header +4 group table, +8 tag count; 8-byte tag entries.
        constexpr uint32_t kTagInstances = 0x827CFF0C;  // u32: guest address of the tag table
        constexpr uint32_t kHeaderGroups = 4;
        constexpr uint32_t kHeaderTagCount = 8;
        constexpr uint32_t kInstanceBytes = 8;
        constexpr uint32_t kInstanceAddress = 4;
        constexpr uint32_t kGroupBytes = 16;
        constexpr uint32_t kEmptyGroup = 0xFFFF;

        // One loaded map's host tags.
        struct LoadedTags
        {
            uint32_t header = 0, checksum = 0;  // which load this is
            uint32_t sectionBase = 0, sectionSize = 0;  // the tag section in guest memory
            host::Arena arena;
            std::vector<const void*> slots;     // host tag per slot (nullptr: not converted yet)
            std::vector<uint8_t> converted;     // per slot: conversion attempted
            uint32_t errors = 0;
        };

        // Readers never block (they run on game threads with their own
        // timing; a 25-60 ms stall behind a map's conversion upset the audio
        // thread): a fully converted map is published through g_published
        // and never changed afterwards, so lookups in it take no lock.
        // g_mutex guards building and the lazy path, which only try_locks
        // (busy: the reader declines and its original runs). An unloaded
        // map's copy is retired, not freed, until the next unload, in case a
        // reader is still inside a call.
        std::mutex g_mutex;
        std::unique_ptr<LoadedTags> g_loaded;
        std::unique_ptr<LoadedTags> g_retired;
        std::atomic<LoadedTags*> g_published{ nullptr };

        // Takes g_loaded out of service (caller holds g_mutex).
        void RetireLoaded()
        {
            if (g_published.load() == g_loaded.get())
                g_published.store(nullptr, std::memory_order_release);
            if (g_loaded)
                g_retired = std::move(g_loaded);
        }

        // Reads guest memory inside the tag section (the converter's callback).
        const uint8_t* ReadGuest(void* context, uint32_t address, uint32_t bytes)
        {
            auto* loaded = static_cast<LoadedTags*>(context);
            if (address < loaded->sectionBase || uint64_t(address - loaded->sectionBase) + bytes > loaded->sectionSize)
                return nullptr;
            return GuestPtr<uint8_t>(address);
        }

        // The current map's tags (a new, empty set after a map load), or
        // nullptr if no known map is loaded. Caller holds g_mutex.
        LoadedTags* Current()
        {
            uint32_t header = LoadedIndexHeader();
            if (!header)
                return nullptr;
            uint32_t checksum = LoadedIndexChecksum();
            if (g_loaded && g_loaded->header == header && g_loaded->checksum == checksum)
                return g_loaded.get();
            const cache::CacheFile* file = LoadedCacheFile();
            if (!file)
                return nullptr;
            auto loaded = std::make_unique<LoadedTags>();
            loaded->header = header;
            loaded->checksum = checksum;
            loaded->sectionBase = file->GetHeader().virtualBase;
            loaded->sectionSize = file->GetHeader().sections[size_t(cache::Section::Tags)].size;
            uint32_t count = GuestRef<uint32_t>(header + kHeaderTagCount);
            loaded->slots.assign(count, nullptr);
            loaded->converted.assign(count, 0);
            static const bool log = EnvFlag("H3_CHECK_TAGS");
            if (log)
                fprintf(stderr, "[tags] tag cache: %s, %u tags\n", file->GetHeader().name.c_str(), count);
            RetireLoaded();
            g_loaded = std::move(loaded);
            return g_loaded.get();
        }

        // The slot's group tag, or 0 for an empty or out-of-range slot.
        uint32_t SlotGroup(uint32_t slot, uint32_t* parent = nullptr, uint32_t* grandparent = nullptr)
        {
            uint32_t header = LoadedIndexHeader();
            if (!header || slot >= uint32_t(GuestRef<uint32_t>(header + kHeaderTagCount)))
                return 0;
            uint32_t instance = GuestRef<uint32_t>(kTagInstances) + slot * kInstanceBytes;
            uint16_t groupIndex = GuestRef<uint16_t>(instance);
            if (groupIndex == kEmptyGroup)
                return 0;
            uint32_t group = GuestRef<uint32_t>(header + kHeaderGroups) + groupIndex * kGroupBytes;
            if (parent)
                *parent = GuestRef<uint32_t>(group + 4);
            if (grandparent)
                *grandparent = GuestRef<uint32_t>(group + 8);
            return GuestRef<uint32_t>(group);
        }
    }

    uint32_t SlotGroupTag(uint32_t slot) { return SlotGroup(slot); }

    namespace
    {
        // Published by the tag load (cache/cache_file_tags.cpp).
        constexpr uint32_t kScenarioDatum = 0x8274B794;
        constexpr uint32_t kGlobalsDatum = 0x8274B798;
    }

    const host::scenario_tag* GlobalScenario()
    {
        uint32_t datum = GuestRef<uint32_t>(kScenarioDatum);
        return datum == host::kNone ? nullptr : TagBySlotOf<host::scenario_tag>(datum & 0xFFFF);
    }

    const host::globals_tag* GlobalGameGlobals()
    {
        uint32_t datum = GuestRef<uint32_t>(kGlobalsDatum);
        return datum == host::kNone ? nullptr : TagBySlotOf<host::globals_tag>(datum & 0xFFFF);
    }

    // A map's tags just loaded (cache/cache_file_tags.cpp): convert them all
    // now, once, rather than as they're first used.
    void OnTagsLoaded()
    {
        static const bool log = EnvFlag("H3_CHECK_TAGS");
        std::lock_guard lock(g_mutex);
        auto start = std::chrono::steady_clock::now();
        LoadedTags* loaded = Current();
        if (!loaded)
            return;
        uint32_t converted = 0;
        uint32_t table = GuestRef<uint32_t>(kTagInstances);
        for (uint32_t slot = 0; slot < loaded->slots.size(); slot++)
        {
            loaded->converted[slot] = 1;
            uint32_t group = SlotGroup(slot);
            const host::GroupConverter* converter = group ? host::FindGroupConverter(group) : nullptr;
            uint32_t address = GuestRef<uint32_t>(table + slot * kInstanceBytes + kInstanceAddress);
            if (!converter || !address)
                continue;
            host::Converter c(loaded->arena, ReadGuest, loaded);
            loaded->slots[slot] = converter->convert(address, c);
            loaded->errors += c.Errors();
            converted++;
        }
        g_published.store(loaded, std::memory_order_release);
        if (log)
        {
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            fprintf(stderr, "[tags] tag cache: %u tags converted at load in %.0f ms (%zu MB, %u errors)\n", converted, ms,
                loaded->arena.TotalBytes() >> 20, loaded->errors);
        }
    }

    // The tags are being unloaded: drop the host copies.
    void OnTagsUnloaded()
    {
        std::lock_guard lock(g_mutex);
        RetireLoaded();
    }

    uint32_t CheckedGroup(uint32_t datum, uint32_t group)
    {
        uint32_t slot = datum & 0xFFFF;
        uint32_t header = LoadedIndexHeader();
        if (!header || datum == host::kNone || slot >= uint32_t(GuestRef<uint32_t>(header + kHeaderTagCount)))
            return 0;
        uint32_t instance = GuestRef<uint32_t>(kTagInstances) + slot * kInstanceBytes;
        if (uint16_t(GuestRef<uint16_t>(instance + 2)) != uint16_t(datum >> 16))
            return 0;
        uint32_t parent = 0, grandparent = 0;
        uint32_t own = SlotGroup(slot, &parent, &grandparent);
        return own && (own == group || parent == group || grandparent == group) ? own : 0;
    }

    const void* TagBySlot(uint32_t slot)
    {
        // The published map, if it's the one loaded: no lock.
        if (LoadedTags* published = g_published.load(std::memory_order_acquire))
            if (published->header == LoadedIndexHeader() && published->checksum == LoadedIndexChecksum())
                return slot < published->slots.size() ? published->slots[slot] : nullptr;
        std::unique_lock lock(g_mutex, std::try_to_lock);
        if (!lock.owns_lock())
            return nullptr;
        LoadedTags* loaded = Current();
        if (!loaded || slot >= loaded->slots.size())
            return nullptr;
        if (loaded->converted[slot])
            return loaded->slots[slot];
        loaded->converted[slot] = 1;
        uint32_t group = SlotGroup(slot);
        const host::GroupConverter* converter = group ? host::FindGroupConverter(group) : nullptr;
        uint32_t address = GuestRef<uint32_t>(GuestRef<uint32_t>(kTagInstances) + slot * kInstanceBytes + kInstanceAddress);
        if (!converter || !address)
            return nullptr;
        host::Converter c(loaded->arena, ReadGuest, loaded);
        loaded->slots[slot] = converter->convert(address, c);
        loaded->errors += c.Errors();
        return loaded->slots[slot];
    }

    // H3_CHECK_TAGS: kReportDelay frames after a map's tags are first used,
    // converts every tag, writes it back (the cache file layout) into
    // scratch memory and compares that with guest memory: bytes that
    // differ are fields the game changed at runtime, which the host copy
    // doesn't follow. Reported per group, with the first address.
    void RuntimeChangeReport()
    {
        static const bool enabled = EnvFlag("H3_CHECK_TAGS");
        static uint32_t reportedChecksum = 0, frames = 0;
        if (!enabled)
            return;
        uint32_t checksum = LoadedIndexChecksum();
        if (!checksum || checksum == reportedChecksum)
        {
            frames = 0;
            return;
        }
        constexpr uint32_t kReportDelay = 600;  // ~10 s: the level has started
        if (++frames < kReportDelay)
            return;
        reportedChecksum = checksum;

        std::lock_guard lock(g_mutex);
        LoadedTags* loaded = Current();
        if (!loaded)
            return;
        struct Scratch
        {
            std::vector<uint8_t> bytes;
            std::vector<std::pair<uint32_t, uint32_t>> written;  // (address, bytes) for the current tag
            uint32_t base;
        } scratch{ std::vector<uint8_t>(loaded->sectionSize), {}, loaded->sectionBase };
        auto write = [](void* context, uint32_t address, uint32_t bytes) -> uint8_t* {
            auto* s = static_cast<Scratch*>(context);
            if (address < s->base || uint64_t(address - s->base) + bytes > s->bytes.size())
                return nullptr;
            s->written.push_back({ address, bytes });
            return s->bytes.data() + (address - s->base);
        };
        struct GroupReport
        {
            uint32_t bytes = 0, tags = 0, first = 0;
        };
        std::vector<std::pair<uint32_t, GroupReport>> groups;
        auto start = std::chrono::steady_clock::now();
        host::Arena arena;
        uint32_t tags = 0;
        for (uint32_t slot = 0; slot < loaded->slots.size(); slot++)
        {
            uint32_t group = SlotGroup(slot);
            const host::GroupConverter* converter = group ? host::FindGroupConverter(group) : nullptr;
            uint32_t address = GuestRef<uint32_t>(GuestRef<uint32_t>(kTagInstances) + slot * kInstanceBytes + kInstanceAddress);
            if (!converter || !address)
                continue;
            scratch.written.clear();
            host::Converter c(arena, ReadGuest, loaded);
            host::Unconverter u(write, &scratch);
            converter->unconvert(converter->convert(address, c), address, u);
            tags++;
            GroupReport* report = nullptr;
            for (auto& [g, r] : groups)
                if (g == group)
                    report = &r;
            // Overlapping writes (shared blocks) are compared more than once;
            // the counts are an upper bound.
            for (auto [at, n] : scratch.written)
                for (uint32_t i = 0; i < n; i++)
                    if (scratch.bytes[at - scratch.base + i] != *GuestPtr<uint8_t>(at + i))
                    {
                        if (!report)
                        {
                            groups.push_back({ group, {} });
                            report = &groups.back().second;
                        }
                        if (!report->bytes)
                            report->first = at + i;
                        report->bytes++;
                    }
            if (report)
                report->tags++;
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        fprintf(stderr, "[tags] runtime changes: %u tags checked in %.0f ms; %zu groups have bytes the game changed%s\n", tags, ms,
            groups.size(), groups.empty() ? "" : ":");
        for (const auto& [group, r] : groups)
            fprintf(stderr, "[tags]   %s: %u bytes in %u tags (first at %08X)\n", cache::GroupTagString(group).c_str(), r.bytes, r.tags,
                r.first);
    }
}

#else

namespace h3::native::tags
{
    void RuntimeChangeReport() {}
    void OnTagsLoaded() {}
    void OnTagsUnloaded() {}
}

#endif
