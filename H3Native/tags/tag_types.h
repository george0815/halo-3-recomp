// Field types of tag data in the Xbox 360 cache file layout (big-endian,
// 32-bit guest addresses), as decompiled-in-place code and the generated
// layouts (tag_layouts.h, from tools/tagdefs) use them. Sizes are the cache
// file's; every generated struct static_asserts its size.

#pragma once

#include <xbox.h>  // be<T> (XenonUtils)

#include <cstdint>

namespace h3::native::tags::layout
{
#pragma pack(push, 1)
    // A string id: namespace set in bits 16+, index in bits 0-15
    // (cache/cache_file.h resolves them).
    using string_id = be<uint32_t>;

    // A tag block: count elements of T at a guest address.
    template <typename T>
    struct tag_block
    {
        be<int32_t> count;
        be<uint32_t> address;
        be<uint32_t> definition;  // unused in cache files
    };
    static_assert(sizeof(tag_block<int>) == 12);

    // A reference to another tag: its group and datum index (-1: none).
    struct tag_reference
    {
        be<uint32_t> group;
        be<uint32_t> name;         // unused in cache files
        be<uint32_t> nameLength;   // unused in cache files
        be<uint32_t> datum;
    };
    static_assert(sizeof(tag_reference) == 16);

    // Raw bytes owned by the tag.
    struct tag_data
    {
        be<int32_t> size;
        be<uint32_t> flags;
        be<uint32_t> fileOffset;   // unused in cache files
        be<uint32_t> address;
        be<uint32_t> definition;   // unused in cache files
    };
    static_assert(sizeof(tag_data) == 20);

    // A resource (sound, bitmap, geometry, animation) streamed from the
    // cache file's resource section: an index into the resource gestalt.
    struct pageable_resource
    {
        be<uint32_t> handle;
        be<uint32_t> unused;
    };
    static_assert(sizeof(pageable_resource) == 8);
#pragma pack(pop)
}
