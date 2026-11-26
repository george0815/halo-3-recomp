// Terminal ('term' device) queries, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism). A
// terminal's definition numbers it (terminal number, 1-based; the
// campaign has 7).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "objects/object_reads.h"
#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    namespace objects = h3::native::objects;
    constexpr uint32_t kTlsPlayersGlobals = 124;
    constexpr uint32_t kPlayersGlobalsTerminalMask = 212;  // s16: a bit per terminal number - 1
    constexpr uint32_t kTerminalReals = 0x82024820;        // read-only real[7], one per terminal

    // The terminal number of a terminal object's definition, or nothing.
    std::optional<int32_t> TerminalNumber(const PPCContext& ctx, uint32_t object)
    {
        uint32_t definition = objects::ObjectDefinition(ctx, object);
        const host::device_terminal_tag* terminal =
            h3::native::tags::TagBySlotOf<host::device_terminal_tag>(definition & 0xFFFF);
        if (!terminal)
            return std::nullopt;
        return int32_t(terminal->device_terminal.terminal_number);
    }
}

// terminal_is_unread(r3 terminal object) -> bool: the players globals'
// terminal mask (+212) doesn't have the bit for this terminal's number
// (true for number 0 or 33+: the shift gives no bit). Name from the mask's
// likely use (terminals read).
H3_REPLACE_INT(82365E38, terminal_is_unread, {
    std::optional<int32_t> number = TerminalNumber(ctx, ctx.r3.u32);
    if (!number)
        return std::nullopt;
    int32_t mask = objects::LoadS16(objects::TlsSlot(ctx, kTlsPlayersGlobals) + kPlayersGlobalsTerminalMask);
    uint32_t bit = objects::ShiftBit(uint32_t(*number - 1));
    return (bit & uint32_t(mask)) == 0 ? 1u : 0u;
})

// terminal_get_number_real(r3 structure with the terminal object at
// +6792) -> real: a read-only per-terminal value (table at 0x82024820)
// for the terminal's number, entry 0 for numbers outside 1-7.
H3_REPLACE_FLOAT(82495F20, terminal_get_number_real, {
    uint32_t object = objects::Load32(ctx.r3.u32 + 6792);
    std::optional<int32_t> number = TerminalNumber(ctx, object);
    if (!number)
        return std::nullopt;
    int32_t index = *number - 1;
    if (index < 0 || index >= 7)
        index = 0;
    return objects::LoadFloat(kTerminalReals + uint32_t(index) * 4);
})

#endif
