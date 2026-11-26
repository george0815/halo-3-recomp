// Bitmap ('bitm') sequence and sprite lookups, decompiled to read the host
// tag cache (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).
//
// Sequence and frame indexes wrap around (C remainder, as the original's
// divw: a negative index stays negative).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "guest_call.h"
#include "replace.h"
#include "tags/tag_cache.h"

PPC_EXTERN_FUNC(sub_822A5F40);  // bitmap_get_sequence_bitmap_index (below)

namespace
{
    namespace host = h3::native::tags::host;
    constexpr uint32_t kNone = 0xFFFFFFFF;
    constexpr uint32_t kSpriteBytes = 0x20;
    constexpr uint32_t kSpriteBounds = 0x08;  // left, right, top, bottom
}

// bitmap_get_sequence_bitmap_index(r3 bitmap datum, s16 sequence, s16
// frame) -> the bitmap index of that frame of the sequence: its first
// bitmap plus the frame for a sequence of bitmaps, else the frame's
// sprite's bitmap; the frame itself where that's NONE or there are no
// sprites; NONE for a NONE bitmap, 0 if there are no sequences.
H3_REPLACE_INT(822A5F40, bitmap_get_sequence_bitmap_index, {
    uint32_t bitmap = ctx.r3.u32;
    if (bitmap == kNone)
        return kNone;
    const host::bitmap_tag* tag = h3::native::tags::TagBySlotOf<host::bitmap_tag>(bitmap & 0xFFFF);
    if (!tag)
        return std::nullopt;
    const auto& sequences = tag->bitmap.sequences;
    if (sequences.storedCount <= 0)
        return 0u;
    int32_t sequenceIndex = int32_t(int16_t(ctx.r4.u32)) % sequences.storedCount;
    if (sequenceIndex < 0 || sequenceIndex >= sequences.count)
        return std::nullopt;
    const auto& sequence = sequences[sequenceIndex];
    int32_t frame = int16_t(ctx.r5.u32);
    int32_t result;
    int32_t bitmapCount = sequence.bitmap_count;
    if (bitmapCount > 0)
        result = frame % bitmapCount + int32_t(sequence.first_bitmap_index);
    else
    {
        if (sequence.sprites.storedCount == 0)
            return uint32_t(frame);
        if (frame < 0 || frame >= sequence.sprites.count)
            return std::nullopt;
        result = sequence.sprites[frame].bitmap_index;
    }
    return uint32_t(result != -1 ? result : frame);
})

// bitmap_get_sequence_sprite_bounds(r3 bitmap datum, s16 sequence, s16
// frame) -> guest address of that frame's sprite's bounds (left, right,
// top, bottom), or 0 for a NONE bitmap or no sequences or sprites.
H3_REPLACE_INT(822A5EB8, bitmap_get_sequence_sprite_bounds, {
    uint32_t bitmap = ctx.r3.u32;
    if (bitmap == kNone)
        return 0u;
    const host::bitmap_tag* tag = h3::native::tags::TagBySlotOf<host::bitmap_tag>(bitmap & 0xFFFF);
    if (!tag)
        return std::nullopt;
    const auto& sequences = tag->bitmap.sequences;
    if (sequences.storedCount <= 0)
        return 0u;
    int32_t sequenceIndex = int32_t(int16_t(ctx.r4.u32)) % sequences.storedCount;
    if (sequenceIndex < 0 || sequenceIndex >= sequences.count)
        return std::nullopt;
    const auto& sprites = sequences[sequenceIndex].sprites;
    if (sprites.storedCount == 0)
        return 0u;
    int32_t frame = int32_t(int16_t(ctx.r5.u32)) % sprites.storedCount;
    return sprites.address + uint32_t(frame) * kSpriteBytes + kSpriteBounds;  // address only, unchecked like the original
})

// bitmap_get_sequence_image(r3 bitmap datum, s16 sequence, s16 frame) ->
// guest address of the bitmap (image) that frame of the sequence shows
// (bitmap_get_sequence_bitmap_index), or 0 when that index is NONE or
// outside the bitmap's Bitmaps block (name unconfirmed).
H3_REPLACE_INT(822A6000, bitmap_get_sequence_image, {
    constexpr uint32_t kImageBytes = 48;
    uint32_t bitmap = ctx.r3.u32;
    int32_t index = int32_t(h3::native::Call(ctx, base, sub_822A5F40, bitmap, ctx.r4.u32, ctx.r5.u32));
    if (index == -1 || index < 0)
        return 0u;
    const host::bitmap_tag* tag = h3::native::tags::TagBySlotOf<host::bitmap_tag>(bitmap & 0xFFFF);
    if (!tag)
        return std::nullopt;
    const auto& images = tag->bitmap.bitmaps;
    if (index >= images.storedCount)
        return 0u;
    return images.address + uint32_t(index) * kImageBytes;
})

#endif
