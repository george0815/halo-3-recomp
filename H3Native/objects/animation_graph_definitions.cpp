// Animation graph lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).
//
// An animation channel (game state, guest memory) names its animation as
// a graph tag datum (+0) and an animation id (+8: s16 inheritance index,
// -1 for the graph itself, then s16 animation index into that graph's
// Animations), and has a byte at +17 set while it plays one.

#ifdef H3_HAVE_TAG_LAYOUTS

#include "memory.h"
#include "objects/object_access.h"
#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    using h3::GuestRef;

    constexpr uint32_t kChannelGraph = 0;          // u32: animation graph ('jmad') datum
    constexpr uint32_t kChannelInheritance = 8;    // s16: inheritance list index, -1 = the graph itself
    constexpr uint32_t kChannelAnimation = 10;     // s16: animation index
    constexpr uint32_t kChannelActive = 17;        // u8: nonzero while an animation is set
    constexpr uint32_t kBlendScreenBytes = 0x1C;   // blend_screens_block_360
    constexpr uint32_t kBlendScreenParameters = 4; // Right Yaw Per Frame: the parameters after the label

    const host::model_animation_graph_v0_struct_definition* Graph(uint32_t datum)
    {
        const host::model_animation_graph_v0_tag* tag =
            h3::native::tags::TagBySlotOf<host::model_animation_graph_v0_tag>(datum & 0xFFFF);
        return tag ? &tag->model_animation_graph_v0 : nullptr;
    }
}

// animation_channel_get_blend_screen(r3 animation channel) -> guest
// address of the parameters of its animation's blend screen (Blend
// Screens[i] + 4: yaw and pitch per frame and frame counts), or 0 if the
// channel has no animation or the animation no blend screen. The original
// is this check plus a tail call of sub_820EDDC0 (graph tag, animation
// id), done here too.
H3_REPLACE_INT(820EDD58, animation_channel_get_blend_screen, {
    uint32_t channel = ctx.r3.u32;
    if (uint8_t(GuestRef<uint8_t>(channel + kChannelActive)) == 0)
        return 0u;
    uint32_t graphDatum = GuestRef<uint32_t>(channel + kChannelGraph);
    if (graphDatum == h3::native::objects::kNone)
        return 0u;
    int32_t animation = int16_t(uint16_t(GuestRef<uint16_t>(channel + kChannelAnimation)));
    if (animation < 0)
        return 0u;
    int32_t inheritance = int16_t(uint16_t(GuestRef<uint16_t>(channel + kChannelInheritance)));

    const host::model_animation_graph_v0_struct_definition* graph = Graph(graphDatum);
    if (!graph)
        return std::nullopt;
    if (inheritance != -1)
    {
        // An inherited graph; the original goes on with a null graph (and
        // reads near address 0) when there is none.
        const auto& inherited = graph->inheritance_list;
        if (inheritance < 0 || inheritance >= inherited.storedCount || inheritance >= inherited.count)
            return std::nullopt;
        uint32_t datum = inherited[inheritance].inherited_graph.datum;
        if (datum == h3::native::objects::kNone)
            return std::nullopt;
        graph = Graph(datum);
        if (!graph)
            return std::nullopt;
    }
    // No bounds check in the original.
    if (animation >= graph->animations.storedCount || animation >= graph->animations.count)
        return std::nullopt;
    int32_t blendScreen = graph->animations[animation].blend_screen_index;
    if (blendScreen == -1)
        return 0u;
    return graph->blend_screens.address + uint32_t(blendScreen) * kBlendScreenBytes + kBlendScreenParameters;
})

#endif
