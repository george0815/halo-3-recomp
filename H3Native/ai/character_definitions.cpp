// Character definition lookups (AI), decompiled to read the host tag cache
// (H3Native/tags/tag_cache.h; docs/ROADMAP.md 6.3 milestone A).
//
// A character tag ('char') can leave a property block empty and inherit it
// from its parent character (+0x04, a tag reference). The game asks for a
// property block through one small function per block, which walks the
// parent chain to the first character whose block isn't empty:
//
//   sub_820ADCF0  character_get_general_properties   (block at +0x6C)
//   sub_820E9A90  character_get_vitality_properties  (block at +0x78)
//   sub_820AE150  character_get_movement_properties  (block at +0xA8)
//   sub_82518ED8  character_get_morph_properties     (block at +0x1A4)
//
// (found as the functions with exactly this shape). All take the character's datum index (r3) and return the guest address
// of the block's first element, or 0 if no character in the chain has one
// (or datum is NONE). They index the tag table by slot only (no salt or
// group check). Recompiled callers read the element from guest memory, so
// the replacements return the guest address the host copy kept.
//
// If the host copy can't answer (no generated layouts, the map isn't known,
// a slot isn't a character), the original runs. H3_ORIGINAL=<name> and
// H3_CHECK_NATIVE=1 as usual (native.h).

#include "native.h"
#include "replace.h"
#include "tags/tag_cache.h"

#include <ppc_recomp_shared.h>

#include <optional>
#include <string>

#ifdef H3_HAVE_TAG_LAYOUTS

namespace h3::native::ai
{
    namespace
    {
        namespace host = tags::host;

        // Longer parent chains than this are a loop (the original would
        // spin forever; none exist in the shipped maps).
        constexpr int kMaxParents = 64;

        // The first character in datum's parent chain whose block (selected
        // by member) has elements: its guest address; 0 if none. Nothing
        // if a link isn't in the host cache.
        template <typename Block>
        std::optional<uint32_t> FirstInParentChain(uint32_t datum, Block host::character_struct_definition::*member)
        {
            for (int depth = 0; depth < kMaxParents && datum != host::kNone; depth++)
            {
                const host::character_tag* tag = tags::TagBySlotOf<host::character_tag>(datum & 0xFFFF);
                if (!tag)
                    return std::nullopt;
                const Block& block = tag->character.*member;
                if (block.storedCount > 0)  // the original tests the stored count, signed
                    return block.address;
                datum = tag->character.parent_character.datum;
            }
            return datum == host::kNone ? std::optional<uint32_t>(0) : std::nullopt;
        }

        // The common shape of both replacements.
        template <typename Block>
        void Replace(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), const char* name,
            NativeCheck& checker, Block host::character_struct_definition::*member)
        {
            static_cast<void>(base);
            if (UseOriginal(name))
            {
                original(ctx, base);
                return;
            }
            uint32_t datum = ctx.r3.u32;
            std::optional<uint32_t> result = FirstInParentChain(datum, member);
            if (!result)
            {
                original(ctx, base);
                return;
            }
            if (CheckNative())
            {
                PPCContext copy = ctx;
                original(copy, base);
                checker.Compare(copy.r3.u32 == *result, [&] {
                    char text[96];
                    snprintf(text, sizeof(text), "%08X: native %08X, original %08X", datum, *result, copy.r3.u32);
                    return std::string(text);
                });
                result = copy.r3.u32;
            }
            ctx.r3.u64 = *result;
        }
    }
}

// character_get_general_properties (r3 = character datum).
PPC_FUNC_IMPL(__imp__sub_820ADCF0);
PPC_FUNC(sub_820ADCF0)
{
    static h3::native::NativeCheck checker("character_get_general_properties");
    h3::native::ai::Replace(ctx, base, __imp__sub_820ADCF0, "character_get_general_properties", checker,
        &h3::native::tags::host::character_struct_definition::general_properties);
}

// character_get_movement_properties (r3 = character datum).
PPC_FUNC_IMPL(__imp__sub_820AE150);
PPC_FUNC(sub_820AE150)
{
    static h3::native::NativeCheck checker("character_get_movement_properties");
    h3::native::ai::Replace(ctx, base, __imp__sub_820AE150, "character_get_movement_properties", checker,
        &h3::native::tags::host::character_struct_definition::movement_properties);
}

#endif

#ifdef H3_HAVE_TAG_LAYOUTS
// character_get_vitality_properties (r3 = character datum).
PPC_FUNC_IMPL(__imp__sub_820E9A90);
PPC_FUNC(sub_820E9A90)
{
    static h3::native::NativeCheck checker("character_get_vitality_properties");
    h3::native::ai::Replace(ctx, base, __imp__sub_820E9A90, "character_get_vitality_properties", checker,
        &h3::native::tags::host::character_struct_definition::vitality_properties);
}

// character_get_morph_properties (r3 = character datum).
PPC_FUNC_IMPL(__imp__sub_82518ED8);
PPC_FUNC(sub_82518ED8)
{
    static h3::native::NativeCheck checker("character_get_morph_properties");
    h3::native::ai::Replace(ctx, base, __imp__sub_82518ED8, "character_get_morph_properties", checker,
        &h3::native::tags::host::character_struct_definition::morph_properties);
}
#endif

#ifdef H3_HAVE_TAG_LAYOUTS
namespace
{
    namespace host = h3::native::tags::host;
    constexpr uint32_t kCharacterWeaponBytes = 0xE0;
}

// character_get_weapon_properties(r3 character, r4 weapon definition) ->
// guest address of the first weapons-properties entry for that weapon,
// looking through parent characters; 0 if none.
H3_REPLACE_INT(820C0430, character_get_weapon_properties, {
    uint32_t datum = ctx.r3.u32;
    uint32_t weapon = ctx.r4.u32;
    for (int depth = 0; depth < 64 && datum != host::kNone; depth++)
    {
        const host::character_tag* tag = h3::native::tags::TagBySlotOf<host::character_tag>(datum & 0xFFFF);
        if (!tag)
            return std::nullopt;
        const auto& weapons = tag->character.weapons_properties;
        if (weapons.storedCount > weapons.count)
            return std::nullopt;
        for (int32_t i = 0; i < weapons.count; i++)
            if (weapons[i].weapon.datum == weapon)
                return weapons.address + uint32_t(i) * kCharacterWeaponBytes;
        datum = tag->character.parent_character.datum;
    }
    return datum == host::kNone ? std::optional<uint32_t>(0u) : std::nullopt;
})
#endif
