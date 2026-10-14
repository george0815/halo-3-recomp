// Reading the game's objects and players in guest memory, for decompiled
// code (stage "decompiled in place": the game state is still the guest's).
//
// Both are data arrays reached through the thread's TLS (the TLS array at
// r13 + 0, a data array header pointer per slot; header +0x44 = elements):
//   kTlsObjects  object headers, 16 bytes: +12 the object's data (guest
//                address); the data starts with its definition's datum
//   kTlsPlayers  players, 880 bytes: +40 the player's unit (object index)
// Indexes are datum indexes; like the game's own accessors these use the
// low 16 bits only (no salt check).

#pragma once

#include "memory.h"

#include <ppc_recomp_shared.h>

#include <cstdint>

namespace h3::native::objects
{
    constexpr uint32_t kNone = 0xFFFFFFFF;
    constexpr uint32_t kTlsObjects = 168;
    constexpr uint32_t kTlsPlayers = 96;
    constexpr uint32_t kDataArrayElements = 68;  // data array header: guest address of the elements
    constexpr uint32_t kObjectHeaderBytes = 16;
    constexpr uint32_t kObjectHeaderData = 12;
    constexpr uint32_t kPlayerBytes = 880;
    constexpr uint32_t kPlayerUnit = 40;
    // Unit object data: its weapons (object indexes, NONE for an empty slot).
    constexpr uint32_t kUnitWeapons = 608;
    constexpr uint32_t kUnitWeaponSlots = 4;

    inline uint32_t Load32(uint32_t address) { return GuestRef<uint32_t>(address); }

    // Guest address of element index of the data array in TLS slot.
    inline uint32_t DataArrayElement(const PPCContext& ctx, uint32_t tlsSlot, uint32_t index, uint32_t elementBytes)
    {
        uint32_t array = Load32(Load32(ctx.r13.u32) + tlsSlot);
        return Load32(array + kDataArrayElements) + (index & 0xFFFF) * elementBytes;
    }

    // An object's data (guest address) and its definition's datum index.
    inline uint32_t ObjectData(const PPCContext& ctx, uint32_t object)
    {
        return Load32(DataArrayElement(ctx, kTlsObjects, object, kObjectHeaderBytes) + kObjectHeaderData);
    }
    inline uint32_t ObjectDefinition(const PPCContext& ctx, uint32_t object) { return Load32(ObjectData(ctx, object)); }

    inline uint32_t PlayerAddress(const PPCContext& ctx, uint32_t player)
    {
        return DataArrayElement(ctx, kTlsPlayers, player, kPlayerBytes);
    }
}
