// The tag system decompiled in place (tags.cpp): lookups in the game's
// loaded tag index in guest memory.

#pragma once

#include <cstdint>

namespace h3::native::cache
{
    class CacheFile;
}

namespace h3::native::tags
{
    // tag_get: the guest address of the tag's data if datum names a tag of
    // groupTag (or of a group whose parent or grandparent it is), else 0.
    uint32_t TagGet(uint32_t groupTag, uint32_t datum);

    // The loaded tag index: its header's guest address (0: none yet) and
    // checksum. A change means another map was loaded.
    uint32_t LoadedIndexHeader();
    uint32_t LoadedIndexChecksum();

    // The cache file in game:\maps whose tag index the game has loaded
    // (found by checksum and address), or nullptr.
    const cache::CacheFile* LoadedCacheFile();

    // H3_CHECK_TAGS: once per map, compares the loaded tag index with the
    // native reader's (called once per presented frame).
    void CheckTagIndex();
}
