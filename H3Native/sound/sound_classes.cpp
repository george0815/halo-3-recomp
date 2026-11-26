// Sound class lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).
//
// The game globals' Sound Globals block (one element) references the
// sound classes tag ('sncl'); its one block holds the classes (0x98 bytes
// each), indexed by a sound's class.

#ifdef H3_HAVE_TAG_LAYOUTS

#include "objects/object_access.h"
#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    constexpr uint32_t kSoundClassBytes = 0x98;
}

// sound_class_get(s16 class): guest address of that class's definition.
// No range check, as in the original.
H3_REPLACE_INT(820F8500, sound_class_get, {
    const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
    if (!globals || globals->globals.sound_globals.count <= 0)
        return std::nullopt;  // the original reads through a null element then
    uint32_t datum = globals->globals.sound_globals[0].sound_classes.datum;
    const auto* classes = h3::native::tags::TagBySlotOf<host::sound_classes_tag>(datum & 0xFFFF);
    if (!classes)
        return std::nullopt;
    int32_t index = int16_t(ctx.r3.u32);
    return classes->sound_classes.sound_classes.address + uint32_t(index * int32_t(kSoundClassBytes));
})

// sound_class_get_reverb(s16 class, bool underwater): guest address of that
// class's air (reverb gain, direct path gain, base obstruction, occlusion)
// or underwater parameters. No range check, as in the original.
H3_REPLACE_INT(8213BAB0, sound_class_get_reverb, {
    const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
    if (!globals || globals->globals.sound_globals.count <= 0)
        return std::nullopt;  // the original reads through a null element then
    uint32_t datum = globals->globals.sound_globals[0].sound_classes.datum;
    const auto* classes = h3::native::tags::TagBySlotOf<host::sound_classes_tag>(datum & 0xFFFF);
    if (!classes)
        return std::nullopt;
    int32_t index = int16_t(ctx.r3.u32);
    uint32_t entry = classes->sound_classes.sound_classes.address + uint32_t(index * int32_t(kSoundClassBytes));
    return entry + ((ctx.r4.u32 & 0xFF) != 0 ? 0x20u : 0x10u);
})

// sound_class_get_promoted_priority(r3 proximity, s16 class) -> the class's
// priority, raised by 2 when proximity < 4 or by 1 when < 100 (at most 8)
// unless it's already 8 or more or the class has "don't promote priority by
// proximity". No range check on the class, as in the original.
H3_REPLACE_INT(821A53C8, sound_class_get_promoted_priority, {
    constexpr uint16_t kDontPromoteByProximity = 1u << 13;
    constexpr int32_t kMaxPriority = 8;
    const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
    if (!globals || globals->globals.sound_globals.count <= 0)
        return std::nullopt;  // the original reads through a null element then
    uint32_t datum = globals->globals.sound_globals[0].sound_classes.datum;
    const auto* classes = h3::native::tags::TagBySlotOf<host::sound_classes_tag>(datum & 0xFFFF);
    if (!classes)
        return std::nullopt;
    int32_t index = int16_t(ctx.r4.u32);
    const auto& block = classes->sound_classes.sound_classes;
    if (index < 0 || index >= block.count)
        return std::nullopt;
    const auto& soundClass = block[index];
    int32_t priority = soundClass.priority;
    if (priority >= kMaxPriority || (soundClass.flags & kDontPromoteByProximity))
        return uint32_t(priority);
    int32_t proximity = int32_t(ctx.r3.u32);
    if (proximity < 4)
        priority += 2;
    else if (proximity < 100)
        priority += 1;
    return uint32_t(priority > kMaxPriority ? kMaxPriority : priority);
})

// sound_class_is_valid(r3 class) -> whether the class can be used: there
// are sound classes, the class is one the game names (0-53, with a
// non-empty name in its static name table), at most 16 sounds per tag and
// per object, and its first internal flag (valid) is set.
H3_REPLACE_INT(821FAE50, sound_class_is_valid, {
    constexpr uint32_t kClassNames = 0x82745F58;  // char*[54]
    constexpr uint32_t kLastClass = 53;
    const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
    if (!globals)
        return std::nullopt;
    const auto& soundGlobals = globals->globals.sound_globals;
    if (soundGlobals.storedCount == 0 || soundGlobals.address == 0)
        return 0u;
    if (soundGlobals.count <= 0)
        return std::nullopt;
    uint32_t datum = soundGlobals[0].sound_classes.datum;
    if (datum == 0xFFFFFFFFu)
        return 0u;
    uint32_t index = ctx.r3.u32;
    if (index > kLastClass)
        return 0u;
    uint32_t name = h3::native::objects::Load32(kClassNames + index * 4);
    if (uint8_t(h3::GuestRef<uint8_t>(name)) == 0)
        return 0u;
    const auto* classes = h3::native::tags::TagBySlotOf<host::sound_classes_tag>(datum & 0xFFFF);
    if (!classes)
        return std::nullopt;
    const auto& block = classes->sound_classes.sound_classes;
    if (int32_t(index) >= block.count)
        return std::nullopt;
    const auto& soundClass = block[index];
    return soundClass.max_sounds_per_tag_1_16 <= 16 && soundClass.max_sounds_per_object_1_16 <= 16 &&
                   (soundClass.internal_flags & 1)
               ? 1u
               : 0u;
})

#endif
