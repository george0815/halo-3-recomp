// Small guest-memory reads shared by the object, unit, weapon and AI
// readers (stage "decompiled in place": the game state stays in guest
// memory; objects/object_access.h has the data array lookups).

#pragma once

#include "objects/object_access.h"

#include <cstdint>
#include <cstring>

namespace h3::native::objects
{
    // Guest values of each width (big-endian), signed where the original
    // sign-extends (extsb / extsh).
    inline uint8_t Load8(uint32_t address) { return GuestRef<uint8_t>(address); }
    inline int8_t LoadS8(uint32_t address) { return int8_t(Load8(address)); }
    inline uint16_t Load16(uint32_t address) { return GuestRef<uint16_t>(address); }
    inline int16_t LoadS16(uint32_t address) { return int16_t(Load16(address)); }
    inline float LoadFloat(uint32_t address)
    {
        uint32_t bits = Load32(address);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    // The guest address in a TLS slot (a data array header or a globals
    // structure: game/game_state_layout.h).
    inline uint32_t TlsSlot(const PPCContext& ctx, uint32_t slot) { return Load32(Load32(ctx.r13.u32) + slot); }

    // The tag instance table (0x827CFF0C: 8 bytes per slot, +4 the tag's
    // data) read the way the original code reads it: by slot, no salt,
    // group or bounds check. Guest memory only, so it answers exactly what
    // the original would, including 0 for an empty slot.
    inline uint32_t RawTagAddress(uint32_t datum)
    {
        constexpr uint32_t kTagInstances = 0x827CFF0C;
        return Load32(Load32(kTagInstances) + (datum & 0xFFFF) * 8 + 4);
    }

    // slw 1 by a shift register: the low 6 bits count, 32-63 give 0.
    inline uint32_t ShiftBit(uint32_t shift)
    {
        shift &= 63;
        return shift < 32 ? 1u << shift : 0u;
    }

    // The object's header (16-byte element of the objects data array):
    // byte 2 flags, byte 3 object type, +12 the data.
    inline uint32_t ObjectHeader(const PPCContext& ctx, uint32_t object)
    {
        return DataArrayElement(ctx, kTlsObjects, object, kObjectHeaderBytes);
    }
}
