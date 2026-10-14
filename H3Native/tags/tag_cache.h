// The tag cache: the loaded map's tags in host layout, for decompiled code
// (docs/ROADMAP.md 6.3, native milestone A). Recompiled code keeps reading
// the guest copy the game loaded; this converts from that copy, so both see
// the same data.
//
// The whole map is converted when its tags finish loading (OnTagsLoaded,
// from the decompiled tag load: ~25 ms and about the tag section's size in
// memory) and dropped at unload; a tag not converted yet (H3_ORIGINAL=
// cache_file_tags) is converted on first use, and a changed tag index
// header or checksum (another map) starts over. The tag section's bounds come from the cache file in
// game:\maps with the loaded index's checksum (cache/cache_file.h).
//
// Fields the game changes at runtime (H3EK marks many with '!') go stale in
// the host copy: decompiled code must only take static fields from here.
// H3_CHECK_TAGS=1 reports which bytes differ from guest memory a while after
// a map loads (RuntimeChangeReport).

#pragma once

#ifdef H3_HAVE_TAG_LAYOUTS

#include "tags/generated/tag_host.h"

#include <cstdint>

namespace h3::native::tags
{
    // The host copy of the tag in this slot (datum index & 0xFFFF), as its
    // group's <name>_tag struct; nullptr for an empty slot or a group the
    // layouts don't know. No salt or group check (callers that the
    // original ran without them use this).
    const void* TagBySlot(uint32_t slot);

    // The tag at datum if it's a T (the group, or one whose parent or
    // grandparent it is, as tag_get checks), else nullptr. The host struct
    // returned is the tag's own group's: T must be that group's _tag type
    // (a parent type's prefix is not laid out the same).
    template <typename T>
    const T* TagGet(uint32_t datum);

    // The host copy of the tag in this slot if it's a T (its own group is
    // T's), else nullptr: for code that, like its original, indexes the tag
    // table by slot without checking the salt.
    template <typename T>
    const T* TagBySlotOf(uint32_t slot);

    // The group tag of the tag in this slot, or 0 for an empty slot.
    uint32_t SlotGroupTag(uint32_t slot);

    // The part that ancestor group partGroup contributes to the tag in this
    // slot, if the tag's group is (or derives from) it: e.g. the unit part
    // (host::unit_struct_definition) of a biped, vehicle or giant. nullptr
    // otherwise. Part is the struct that group adds.
    template <typename Part>
    const Part* TagPartBySlot(uint32_t slot, uint32_t partGroup)
    {
        uint32_t group = SlotGroupTag(slot);
        return group ? static_cast<const Part*>(host::TagPart(TagBySlot(slot), group, partGroup)) : nullptr;
    }

    // Internal: the slot's group tag if datum names a live tag of a group
    // that is (or descends from) group, else 0.
    uint32_t CheckedGroup(uint32_t datum, uint32_t group);

    template <typename T>
    const T* TagBySlotOf(uint32_t slot)
    {
        return SlotGroupTag(slot) == T::kGroup ? static_cast<const T*>(TagBySlot(slot)) : nullptr;
    }

    template <typename T>
    const T* TagGet(uint32_t datum)
    {
        if (CheckedGroup(datum, T::kGroup) != T::kGroup)
            return nullptr;
        return static_cast<const T*>(TagBySlot(datum & 0xFFFF));
    }

    // The loaded map's global scenario and game globals ('scnr', 'matg', as
    // the tag load published them), or nullptr.
    const host::scenario_tag* GlobalScenario();
    const host::globals_tag* GlobalGameGlobals();

    // Called once per presented frame: H3_CHECK_TAGS reports (tags.cpp).
    void RuntimeChangeReport();
}

#endif
