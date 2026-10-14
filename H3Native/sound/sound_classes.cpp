// Sound class lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).
//
// The game globals' Sound Globals block (one element) references the
// sound classes tag ('sncl'); its one block holds the classes (0x98 bytes
// each), indexed by a sound's class.

#ifdef H3_HAVE_TAG_LAYOUTS

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

#endif
