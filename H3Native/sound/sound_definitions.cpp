// Sound ('snd!') and sound cache file gestalt ('ugh!') lookups, decompiled
// to read the host tag cache (docs/ROADMAP.md 6.3 milestone A; replace.h
// for the mechanism).
//
// A sound tag keeps little itself: its codec, pitch ranges, playbacks and
// custom playbacks are indexes into the map's one sound gestalt, whose tag
// data the game keeps a pointer to (kSoundGestaltGlobals, with a byte
// after it that picks where pitch ranges keep their permutation counts).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "guest_call.h"
#include "memory.h"
#include "objects/object_reads.h"
#include "replace.h"
#include "tags/tag_cache.h"

PPC_EXTERN_FUNC(sub_82127B38);
PPC_EXTERN_FUNC(sub_820B5108);  // f1 -> f1 (math; dB to amplitude presumably), pure
PPC_EXTERN_FUNC(sub_820F9528);  // f1 -> r3 real bits, pure
PPC_EXTERN_FUNC(sub_820F9810);  // r3 sound class -> r3 real bits, pure

namespace
{
    namespace host = h3::native::tags::host;
    using h3::GuestRef;
    using h3::native::objects::Load32;
    constexpr uint32_t kNone = 0xFFFFFFFF;

    // u32: guest address of the loaded sound gestalt's tag data; u8 at +4:
    // which 6 bits of a pitch range's encoded counts are its permutation
    // count (bits 26-31 when set, 20-25 otherwise).
    constexpr uint32_t kSoundGestaltGlobals = 0x827E3640;
    constexpr uint32_t kSoundGestaltHighCounts = kSoundGestaltGlobals + 4;

    constexpr uint32_t kSoundGestaltGroup = host::sound_cache_file_gestalt_tag::kGroup;
    constexpr uint32_t kSoundGroup = host::sound_tag::kGroup;

    // The host copy of the sound gestalt the game points at, or nullptr if
    // there is none or no 'ugh!' tag has its address. The slot is looked up
    // once per map load and gestalt address (per thread: no locking).
    const host::sound_cache_file_gestalt_tag* SoundGestalt()
    {
        uint32_t address = Load32(kSoundGestaltGlobals);
        uint32_t identity = h3::native::tags::LoadedTagsIdentity();
        if (!address || !identity)
            return nullptr;
        thread_local uint32_t cachedIdentity = 0, cachedAddress = 0, cachedSlot = kNone;
        if (identity != cachedIdentity || address != cachedAddress)
        {
            cachedSlot = kNone;
            uint32_t count = h3::native::tags::TagSlotCount();
            for (uint32_t slot = 0; slot < count; slot++)
                if (h3::native::tags::SlotGroupTag(slot) == kSoundGestaltGroup &&
                    h3::native::tags::TagSlotAddress(slot) == address)
                {
                    cachedSlot = slot;
                    break;
                }
            cachedIdentity = identity;
            cachedAddress = address;
        }
        if (cachedSlot == kNone)
            return nullptr;
        return h3::native::tags::TagBySlotOf<host::sound_cache_file_gestalt_tag>(cachedSlot);
    }

    // A pitch range's permutation count (from its encoded first permutation
    // and counts), as the gestalt globals' byte selects.
    int32_t PitchRangePermutationCount(int32_t encoded)
    {
        uint32_t bits = uint32_t(encoded);
        return int32_t(uint8_t(GuestRef<uint8_t>(kSoundGestaltHighCounts)) != 0 ? (bits >> 26) & 0x3F : (bits >> 20) & 0x3F);
    }

    // The game globals' sound classes entry for a sound's class (the s8 at
    // the sound tag data's +2; no range check in the originals), or
    // nullptr if the host copy can't say (no Sound Globals: the originals
    // read through null then).
    const host::sound_class_block* SoundClassOfSound(uint32_t soundTagData)
    {
        const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
        if (!globals)
            return nullptr;
        const auto& soundGlobals = globals->globals.sound_globals;
        if (soundGlobals.storedCount == 0 || soundGlobals.count <= 0)
            return nullptr;
        uint32_t datum = soundGlobals[0].sound_classes.datum;
        if (datum == kNone)
            return nullptr;  // the original reads slot 0xFFFF
        const auto* classes = h3::native::tags::TagBySlotOf<host::sound_classes_tag>(datum & 0xFFFF);
        if (!classes)
            return nullptr;
        int32_t index = int8_t(uint8_t(GuestRef<uint8_t>(soundTagData + 2)));
        const auto& block = classes->sound_classes.sound_classes;
        if (index < 0 || index >= block.count)
            return nullptr;
        return &block[index];
    }

    // Sound class flag 5: deterministic.
    constexpr uint16_t kSoundClassDeterministic = 1u << 5;
}

// sound_pitch_range_get_permutation_byte(r3 gestalt pitch range, r4 sound
// tag data) -> the low byte of the pitch range's encoded permutation data
// (s8), or NONE for a sound of a deterministic class.
H3_REPLACE_INT(821F6888, sound_pitch_range_get_permutation_byte, {
    const host::sound_class_block* soundClass = SoundClassOfSound(ctx.r4.u32);
    if (!soundClass)
        return std::nullopt;
    if (soundClass->flags & kSoundClassDeterministic)
        return kNone;
    return uint32_t(int32_t(int8_t(uint8_t(GuestRef<uint8_t>(ctx.r3.u32 + 5)))));
})

// sound_pitch_range_get_first_runtime_permutation_flag(r3 gestalt pitch
// range, r4 sound tag data) -> the pitch range's first runtime permutation
// flag index (u16), or NONE when it's 0xFFFF or the sound's class does
// NOT have flag 5 (kSoundClassDeterministic; the name of that flag is
// unconfirmed: 821F6888 tests it the other way).
H3_REPLACE_INT(821F6900, sound_pitch_range_get_first_runtime_permutation_flag, {
    const host::sound_class_block* soundClass = SoundClassOfSound(ctx.r4.u32);
    if (!soundClass)
        return std::nullopt;
    // The opposite test from 821F6888: NONE unless the class has flag 5.
    if (!(soundClass->flags & kSoundClassDeterministic))
        return kNone;
    uint16_t index = GuestRef<uint16_t>(ctx.r3.u32 + 6);
    return index == 0xFFFF ? kNone : uint32_t(index);
})

// sound_global_propagation_get(r3 bool) -> guest address of the game
// globals' Sound Propagation tag ('sgp!', its datum unchecked for group),
// or 0: when r3 is false, the player mapping globals' s16 at +180 is more
// than 1, or the reference is NONE.
H3_REPLACE_INT(821275A0, sound_global_propagation_get, {
    constexpr uint32_t kTlsPlayerMappingGlobals = 216;
    constexpr uint32_t kMappingCount = 180;  // s16 (local player count?)
    uint32_t mapping = Load32(Load32(ctx.r13.u32) + kTlsPlayerMappingGlobals);
    if (int16_t(uint16_t(GuestRef<uint16_t>(mapping + kMappingCount))) > 1)
        return 0u;
    if ((ctx.r3.u32 & 0xFF) == 0)
        return 0u;
    const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
    if (!globals)
        return std::nullopt;
    const auto& soundGlobals = globals->globals.sound_globals;
    if (soundGlobals.storedCount == 0 || soundGlobals.count <= 0)
        return std::nullopt;  // the original reads through null then
    uint32_t datum = soundGlobals[0].sound_propagation.datum;
    if (datum == kNone)
        return 0u;
    uint32_t address = h3::native::tags::TagSlotAddress(datum & 0xFFFF);
    return address ? std::optional<uint32_t>(address) : std::nullopt;
})

// sound_get_playback(r3 sound datum, r4 encoded playback: bits 30-31 the
// kind, the rest an index) -> guest address of the playback parameters:
//   kind 0 (or r4 NONE): the sound's custom playback in the gestalt, 0 if
//     it has none or r3 isn't a sound (0 for r3 NONE with r4 NONE);
//   kind 1 / 2: the sound effect at the index in the game globals' / the
//     scenario's sound effect collection (its playback, +4; no range
//     check), 0 if there's no collection;
//   kind 3: 0.
H3_REPLACE_INT(820F8688, sound_get_playback, {
    constexpr uint32_t kPlatformPlaybackBytes = 76;
    constexpr uint32_t kPlatformPlayback = 4;
    constexpr uint32_t kCustomPlaybackBytes = 72;
    uint32_t sound = ctx.r3.u32;
    uint32_t encoded = ctx.r4.u32;
    uint32_t kind = encoded == kNone ? 0 : encoded >> 30;
    if (encoded == kNone && sound == kNone)
        return 0u;
    if (kind == 0)
    {
        if (sound & 0x8000)
            return std::nullopt;  // the original indexes the tag table with a negative short
        uint32_t slot = sound & 0xFFFF;
        uint32_t group = h3::native::tags::SlotGroupTag(slot);
        if (group == 0)
            return std::nullopt;  // empty slot: the original reads the group table at -1
        if (group != kSoundGroup)
            return 0u;
        const auto* tag = h3::native::tags::TagBySlotOf<host::sound_tag>(slot);
        if (!tag)
            return std::nullopt;
        int32_t custom = tag->sound.ugh_custom_playback_index;
        if (custom == -1)
            return 0u;
        const host::sound_cache_file_gestalt_tag* gestalt = SoundGestalt();
        if (!gestalt)
            return std::nullopt;
        return gestalt->sound_cache_file_gestalt.custom_playbacks.address + uint32_t(custom) * kCustomPlaybackBytes;
    }
    if (kind == 3)
        return 0u;
    uint32_t collection;
    if (kind == 1)
    {
        const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
        if (!globals)
            return std::nullopt;
        const auto& soundGlobals = globals->globals.sound_globals;
        if (soundGlobals.storedCount == 0 || soundGlobals.address == 0)
            return 0u;
        if (soundGlobals.count <= 0)
            return std::nullopt;
        collection = soundGlobals[0].sound_effects.datum;
    }
    else
    {
        const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
        if (!scenario)
            return std::nullopt;
        collection = scenario->scenario.sound_effect_collection.datum;
    }
    if (collection == kNone)
        return 0u;
    const auto* effects = h3::native::tags::TagBySlotOf<host::sound_effect_collection_tag>(collection & 0xFFFF);
    if (!effects)
        return std::nullopt;
    uint32_t index = encoded & 0x3FFFFFFF;
    return effects->sound_effect_collection.sound_effects.address + index * kPlatformPlaybackBytes + kPlatformPlayback;
})

// sound_permutation_reference_valid(r3 sound datum, r4 {s8 pitch range,
// s8 permutation}) -> bool: both indexes are in range for the sound (its
// pitch range count; that pitch range's permutation count in the gestalt).
// false for NONE; the datum's group isn't checked.
H3_REPLACE_INT(8223E0F0, sound_permutation_reference_valid, {
    uint32_t sound = ctx.r3.u32;
    if (sound == kNone)
        return 0u;
    int32_t pitchRange = int8_t(uint8_t(GuestRef<uint8_t>(ctx.r4.u32)));
    if (pitchRange < 0)
        return 0u;
    const auto* tag = h3::native::tags::TagBySlotOf<host::sound_tag>(sound & 0xFFFF);
    if (!tag)
        return std::nullopt;
    if (pitchRange >= tag->sound.pitch_range_count)
        return 0u;
    int32_t permutation = int8_t(uint8_t(GuestRef<uint8_t>(ctx.r4.u32 + 1)));
    if (permutation < 0)
        return 0u;
    const host::sound_cache_file_gestalt_tag* gestalt = SoundGestalt();
    if (!gestalt)
        return std::nullopt;
    const auto& pitchRanges = gestalt->sound_cache_file_gestalt.pitch_ranges;
    int32_t index = int32_t(tag->sound.ugh_first_pitch_range_index) + pitchRange;
    if (index < 0 || index >= pitchRanges.count)
        return std::nullopt;
    int32_t count = PitchRangePermutationCount(pitchRanges[index].encoded_first_permutation_and_counts);
    return permutation < count ? 1u : 0u;
})

namespace
{
    // The gestalt codec of a sound (no group check, like the originals), or
    // nullptr if the host copy can't say.
    const host::sound_gestalt_codec_block* SoundCodec(uint32_t sound)
    {
        const auto* tag = h3::native::tags::TagBySlotOf<host::sound_tag>(sound & 0xFFFF);
        if (!tag)
            return nullptr;
        const host::sound_cache_file_gestalt_tag* gestalt = SoundGestalt();
        if (!gestalt)
            return nullptr;
        const auto& codecs = gestalt->sound_cache_file_gestalt.codecs;
        int32_t index = tag->sound.ugh_codec_index;
        if (index < 0 || index >= codecs.count)
            return nullptr;
        return &codecs[index];
    }

    constexpr int8_t kEncodingCodec = 4;
    constexpr uint32_t kCompressionXmaMask = (1u << 6) | (1u << 7);  // xma, xma v2.0
}

// sound_is_xma(r3 sound datum) -> bool: the sound doesn't have flag 12 set,
// its codec's encoding isn't "codec", and its compression is xma or xma
// v2.0. The datum isn't checked for NONE or group.
H3_REPLACE_INT(82129A18, sound_is_xma, {
    if (ctx.r3.u32 == kNone)
        return std::nullopt;
    const auto* tag = h3::native::tags::TagBySlotOf<host::sound_tag>(ctx.r3.u32 & 0xFFFF);
    if (!tag)
        return std::nullopt;
    if (tag->sound.flags & 0x1000)
        return 0u;
    const host::sound_gestalt_codec_block* codec = SoundCodec(ctx.r3.u32);
    if (!codec)
        return std::nullopt;
    if (uint8_t(codec->encoding) == kEncodingCodec)
        return 0u;
    uint32_t shift = uint32_t(int32_t(codec->compression)) & 0x3F;  // slw: 32-63 give 0
    uint32_t bit = shift >= 32 ? 0 : 1u << shift;
    return (bit & kCompressionXmaMask) ? 1u : 0u;
})

// sound_codecs_match(r3 sound datum, r4 sound datum) -> bool: both sounds'
// gestalt codecs have the same encoding and compression. Neither datum is
// checked.
H3_REPLACE_INT(821A7CB8, sound_codecs_match, {
    if (ctx.r3.u32 == kNone || ctx.r4.u32 == kNone)
        return std::nullopt;
    const host::sound_gestalt_codec_block* a = SoundCodec(ctx.r3.u32);
    const host::sound_gestalt_codec_block* b = SoundCodec(ctx.r4.u32);
    if (!a || !b)
        return std::nullopt;
    return uint8_t(a->encoding) == uint8_t(b->encoding) && uint8_t(a->compression) == uint8_t(b->compression) ? 1u : 0u;
})

namespace
{
    // A distance of a sound's playback parameters (its gestalt playback,
    // or its sound class's when the playback's internal flag bit says to
    // use the class's): the playback field and the class field.
    std::optional<float> SoundDistance(uint32_t sound, uint32_t useClassFlag, float host::playbacks_block_360::*playbackField,
        float classValue(const host::sound_class_block&))
    {
        const auto* tag = h3::native::tags::TagBySlotOf<host::sound_tag>(sound & 0xFFFF);
        if (!tag)
            return std::nullopt;
        const host::sound_cache_file_gestalt_tag* gestalt = SoundGestalt();
        if (!gestalt)
            return std::nullopt;
        const auto& playbacks = gestalt->sound_cache_file_gestalt.playbacks;
        int32_t index = tag->sound.ugh_playback_index;
        if (index < 0 || index >= playbacks.count)
            return std::nullopt;  // no range check in the originals
        const host::playbacks_block_360& playback = playbacks[index];
        // Internal flags ('!': the game may set them) from guest memory.
        constexpr uint32_t kPlaybackBytes = 68;
        if (!(Load32(playbacks.address + uint32_t(index) * kPlaybackBytes) & useClassFlag))
            return playback.*playbackField;
        const host::sound_class_block* soundClass = SoundClassOfSound(h3::native::tags::TagSlotAddress(sound & 0xFFFF));
        if (!soundClass)
            return std::nullopt;
        return classValue(*soundClass);
    }
}

// sound_get_dont_play_distance(r3 sound datum) -> real: its playback's
// Don't Play Distance, or its sound class's (+0x34) when the playback has
// internal flag bit 0. The datum isn't checked.
H3_REPLACE_FLOAT(820F8C08, sound_get_dont_play_distance, {
    return SoundDistance(ctx.r3.u32, 1u << 0, &host::playbacks_block_360::don_t_play_distance,
        [](const host::sound_class_block& c) { return c.attack_bounds[0]; });
})

// sound_get_minimum_distance(r3 sound datum) -> real: its playback's
// Minimum Distance, or its sound class's (+0x3C) with internal flag bit 2.
H3_REPLACE_FLOAT(820F9298, sound_get_minimum_distance, {
    return SoundDistance(ctx.r3.u32, 1u << 2, &host::playbacks_block_360::minimum_distance,
        [](const host::sound_class_block& c) { return c.distance_bounds[0]; });
})

// sound_get_maximum_distance(r3 sound datum) -> real: its playback's
// Maximum Distance, or its sound class's (+0x40) with internal flag bit 3.
H3_REPLACE_FLOAT(820F8B88, sound_get_maximum_distance, {
    return SoundDistance(ctx.r3.u32, 1u << 3, &host::playbacks_block_360::maximum_distance,
        [](const host::sound_class_block& c) { return c.distance_bounds[1]; });
})

// sound_flag_5_applies_to_object(r3 sound datum, r4 object) -> bool: the
// sound has Flags bit 5 and the object is a weapon (object header type
// 2); false for either NONE. The datum's group isn't checked (name
// unconfirmed; bit 5 is "use huge sound transmission" in some flag lists).
H3_REPLACE_INT(821FC7D0, sound_flag_5_applies_to_object, {
    uint32_t sound = ctx.r3.u32;
    if (sound == kNone)
        return 0u;
    const auto* tag = h3::native::tags::TagBySlotOf<host::sound_tag>(sound & 0xFFFF);
    if (!tag)
        return std::nullopt;
    if (!(tag->sound.flags & (1u << 5)))
        return 0u;
    if (ctx.r4.u32 == kNone)
        return 0u;
    uint32_t header = h3::native::objects::DataArrayElement(ctx, h3::native::objects::kTlsObjects, ctx.r4.u32,
        h3::native::objects::kObjectHeaderBytes);
    uint32_t type = uint32_t(uint8_t(GuestRef<uint8_t>(header + 3))) & 63;  // slw: 32-63 give 0
    return (type < 32 && ((1u << type) & 4)) ? 1u : 0u;
})

// sound_source_audible_from_listener(r3 listener index, r4 sound source,
// f1 volume) -> bool: true for listener NONE; false for a source with
// flag bit 0 or whose kind (byte +3, high nibble) isn't 1; true when the
// source's cluster reference (+52: s8 bsp, u8 cluster) or the listener's
// (*0x827D1F10, 80 bytes each, +176) is unset (0xFF bsp); false when
// they're the same cluster; otherwise whether volume times the two
// clusters' audibility (sub_82127B38 on the bsp, 0 across bsps) reaches
// a read-only threshold (name unconfirmed).
H3_REPLACE_INT(820F9050, sound_source_audible_from_listener, {
    constexpr uint32_t kListeners = 0x827D1F10;  // guest address of the listeners (game state)
    constexpr uint32_t kListenerBytes = 80;
    constexpr uint32_t kListenerCluster = 176;
    constexpr uint32_t kSourceCluster = 52;
    constexpr uint32_t kRealZero = 0x8202251C;
    constexpr uint32_t kRealAudibleThreshold = 0x820332B0;
    using h3::native::objects::LoadFloat;
    uint32_t listener = ctx.r3.u32;
    uint32_t source = ctx.r4.u32;
    float volume = float(ctx.f1.f64);
    if (listener == kNone)
        return 1u;
    if (uint16_t(GuestRef<uint16_t>(source)) & 1)
        return 0u;
    if ((uint8_t(GuestRef<uint8_t>(source + 3)) & 0xF0) != 0x10)
        return 0u;
    uint32_t listenerCluster = Load32(kListeners) + listener * kListenerBytes + kListenerCluster;
    if (uint8_t(GuestRef<uint8_t>(source + kSourceCluster)) == 0xFF)
        return 1u;
    if (uint8_t(GuestRef<uint8_t>(listenerCluster)) == 0xFF)
        return 1u;
    uint16_t sourceReference = GuestRef<uint16_t>(source + kSourceCluster);
    uint16_t listenerReference = GuestRef<uint16_t>(listenerCluster);
    if (sourceReference == listenerReference)
        return 0u;
    int32_t bsp = int8_t(uint8_t(listenerReference >> 8));
    float audibility = LoadFloat(kRealZero);
    if (bsp == int8_t(uint8_t(sourceReference >> 8)))
    {
        const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
        if (!scenario)
            return std::nullopt;
        const auto& bsps = scenario->scenario.structure_bsps;
        if (bsp < 0 || bsp >= bsps.storedCount || bsp >= bsps.count)
            return std::nullopt;
        uint32_t data = h3::native::objects::RawTagAddress(bsps[bsp].structure_bsp.datum);
        if (!data)
            return std::nullopt;
        h3::native::Call(ctx, base, sub_82127B38, data, listenerReference & 0xFF, sourceReference & 0xFF);
        audibility = float(ctx.f1.f64);
    }
    return audibility * volume < LoadFloat(kRealAudibleThreshold) ? 0u : 1u;
})

// sound_get_gain(r3 sound tag data: +2 s8 class, +14 s16 gestalt entry, r4
// real bits, f1 interpolation) -> real bits: the class's Gain Bounds (game
// globals Sound Globals' sound classes) plus the gestalt entry's two reals
// (20-byte entries at the gestalt's +28; minimum at +0, maximum at +4),
// each scaled by the constant at 0x820009BC and passed through
// sub_820B5108, interpolated by f1, through sub_820F9528, plus
// sub_820F9810 of the class plus r4 (name unconfirmed). The float
// operations are the recompiled code's (single rounding of a double
// result).
H3_REPLACE_INT(820F9390, sound_get_gain, {
    constexpr uint32_t kRealScale = 0x820009BC;  // lis -32256, +2492
    constexpr uint32_t kGestaltEntries = 28;
    constexpr uint32_t kGestaltEntryBytes = 20;
    uint32_t sound = ctx.r3.u32;
    uint32_t extra = ctx.r4.u32;
    double interpolation = ctx.f1.f64;
    const host::sound_class_block* soundClass = SoundClassOfSound(sound);
    if (!soundClass)
        return std::nullopt;
    uint32_t gestalt = Load32(kSoundGestaltGlobals);
    int32_t index = int16_t(GuestRef<uint16_t>(sound + 14));
    uint32_t entry = Load32(gestalt + kGestaltEntries) + uint32_t(index) * kGestaltEntryBytes;
    auto real = [](uint32_t address) { return double(h3::native::objects::LoadFloat(address)); };
    double scale = real(kRealScale);
    double maximum = double(float(real(entry + 4) + double(soundClass->gain_bounds[1])));
    double minimum = double(float(real(entry) + double(soundClass->gain_bounds[0])));
    ctx.f1.f64 = double(float(minimum * scale));
    h3::native::Call(ctx, base, sub_820B5108);
    double low = double(float(ctx.f1.f64));
    ctx.f1.f64 = double(float(maximum * scale));
    h3::native::Call(ctx, base, sub_820B5108);
    double high = double(float(ctx.f1.f64));
    double span = double(float(high - low));
    ctx.f1.f64 = double(float(span * interpolation + low));
    uint32_t a = h3::native::Call(ctx, base, sub_820F9528);
    uint32_t b = h3::native::Call(ctx, base, sub_820F9810, uint32_t(int32_t(int8_t(GuestRef<uint8_t>(sound + 2)))));
    auto bitsReal = [](uint32_t bits) {
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return double(value);
    };
    float sum = float(bitsReal(a) + double(float(bitsReal(b) + bitsReal(extra))));
    uint32_t result;
    std::memcpy(&result, &sum, sizeof(result));
    return result;
})

#endif
