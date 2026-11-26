// Animation graph lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).
//
// An animation channel (game state, guest memory) names its animation as
// a graph tag datum (+0) and an animation id (+8: s16 inheritance index,
// -1 for the graph itself, then s16 animation index into that graph's
// Animations), and has a byte at +17 set while it plays one.

#ifdef H3_HAVE_TAG_LAYOUTS

#include "memory.h"
#include "guest_call.h"
#include "objects/object_reads.h"
#include "replace.h"
#include "tags/tag_cache.h"

PPC_EXTERN_FUNC(sub_82088C60);

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

// object_animation_graph_has_mode(r3 object, r4 mode string id) -> bool:
// the object's animation manager (at the object data's s16 offset +336)
// has a graph (+124) whose Modes (sorted by Label) contain the label: the
// original's binary search (sub_820E8850), step for step, so an unsorted
// block answers the same (name unconfirmed).
H3_REPLACE_INT(820E8BF0, object_animation_graph_has_mode, {
    namespace objects = h3::native::objects;
    constexpr uint32_t kObjectAnimationOffset = 336;  // s16
    constexpr uint32_t kAnimationGraph = 124;         // 'jmad' datum
    uint32_t data = objects::ObjectData(ctx, ctx.r3.u32);
    uint32_t manager = data + uint32_t(int32_t(int16_t(GuestRef<uint16_t>(data + kObjectAnimationOffset))));
    uint32_t graph = objects::Load32(manager + kAnimationGraph);
    if (graph == objects::kNone)
        return 0u;
    const host::model_animation_graph_v0_tag* tag = h3::native::tags::TagBySlotOf<host::model_animation_graph_v0_tag>(graph & 0xFFFF);
    if (!tag)
        return std::nullopt;
    const auto& modes = tag->model_animation_graph_v0.modes;
    if (modes.storedCount != modes.count)
        return std::nullopt;
    int32_t key = int32_t(ctx.r4.u32);
    int32_t low = 0, high = modes.count - 1;
    bool last = false;
    while (!last && high >= low)
    {
        int32_t middle = (high + low) >> 1;
        last = high - low == 0;
        int32_t label = int32_t(modes[middle].label);
        if (label == key)
            return 1u;
        if (label > key)
            high = middle - 1;
        else
            low = middle + 1;
    }
    return 0u;
})

namespace
{
    namespace objects = h3::native::objects;

    // An animation found by FindAnimation: its host copy and guest address,
    // or {nullptr, 0} where the original finds none.
    struct AnimationRef
    {
        const host::animations_block_360* animation = nullptr;
        uint32_t address = 0;
        const host::model_animation_graph_v0_struct_definition* graph = nullptr;  // the graph it's in
    };
    constexpr uint32_t kAnimationBytes = 0x88;  // animations_block_360

    // sub_822FC408 / sub_820E0028: the animation an id names (high s16:
    // inheritance list index, -1 = the graph itself; low s16: animation
    // index) in the graph in this tag table slot (no salt or group check).
    // None for a negative animation index, an inheritance index past the
    // list, or an inherited graph that is NONE or an empty slot. nullopt
    // where the original reads through null or past a block (no bounds
    // check on the animation index).
    std::optional<AnimationRef> FindAnimation(uint32_t graphDatum, uint32_t id)
    {
        int32_t animation = int16_t(uint16_t(id));
        if (animation < 0)
            return AnimationRef{};
        const host::model_animation_graph_v0_struct_definition* graph = Graph(graphDatum);
        if (!graph)
            return std::nullopt;
        int32_t inheritance = int16_t(uint16_t(id >> 16));
        if (inheritance != -1)
        {
            const auto& inherited = graph->inheritance_list;
            if (inheritance >= inherited.storedCount)
                return AnimationRef{};
            if (inheritance < 0 || inheritance >= inherited.count)
                return std::nullopt;
            uint32_t datum = inherited[inheritance].inherited_graph.datum;
            if (datum == objects::kNone || objects::RawTagAddress(datum) == 0)
                return AnimationRef{};
            graph = Graph(datum);
            if (!graph)
                return std::nullopt;
        }
        const auto& animations = graph->animations;
        if (animation >= animations.storedCount || animation >= animations.count)
            return std::nullopt;
        return AnimationRef{ &animations[animation], animations.address + uint32_t(animation) * kAnimationBytes, graph };
    }

    // The animation a channel plays, for the channel getters below: none
    // (nullptr) when the channel isn't active, has no graph or a negative
    // animation index (they return their default then); nullopt when the
    // original would read through null (no animation found) or can't be
    // answered.
    std::optional<const host::animations_block_360*> ChannelAnimation(uint32_t channel)
    {
        if (objects::Load8(channel + kChannelActive) == 0 || objects::Load32(channel + kChannelGraph) == objects::kNone ||
            objects::Load16(channel + kChannelAnimation) >= 0x8000)
            return nullptr;
        std::optional<AnimationRef> found =
            FindAnimation(objects::Load32(channel + kChannelGraph), objects::Load32(channel + kChannelInheritance));
        if (!found || !found->animation)
            return std::nullopt;
        return found->animation;
    }

    constexpr uint32_t kChannelFrame = 20;     // real: current frame (probably)
    constexpr uint32_t kChannelFrame24 = 24;   // real: a frame position (name unknown)
    // Read-only reals the originals load.
    constexpr uint32_t kRealZero = 0x8202251C;
    constexpr uint32_t kRealOne = 0x820221FC;
    constexpr uint32_t kRealSecondsPerTick = 0x8202AED4;  // frame -> seconds (1/30 probably)
    constexpr uint32_t kRealMinimumRate = 0x8202490C;
    constexpr uint32_t kRealRateScale = 0x8202AF7C;
    constexpr uint32_t kRealNoKeyframe = 0x82032938;

    // Frame event type 0: primary keyframe.
    constexpr uint16_t kFrameEventPrimaryKeyframe = 0;
}

// animation_channel_get_length_seconds(r3 channel) -> real: its
// animation's Frame Count times a read-only real (seconds per frame,
// probably); 0 when the channel plays nothing (name unconfirmed).
H3_REPLACE_FLOAT(822FBE70, animation_channel_get_length_seconds, {
    std::optional<const host::animations_block_360*> animation = ChannelAnimation(ctx.r3.u32);
    if (!animation)
        return std::nullopt;
    if (!*animation)
        return objects::LoadFloat(kRealZero);
    return float((*animation)->frame_count) * objects::LoadFloat(kRealSecondsPerTick);
})

// animation_channel_get_frames_remaining(r3 channel) -> real: its
// animation's last frame (Frame Count - 1) minus the channel's real at
// +24; 0 when the channel plays nothing (name unconfirmed).
H3_REPLACE_FLOAT(822FBA78, animation_channel_get_frames_remaining, {
    std::optional<const host::animations_block_360*> animation = ChannelAnimation(ctx.r3.u32);
    if (!animation)
        return std::nullopt;
    if (!*animation)
        return objects::LoadFloat(kRealZero);
    return float(int32_t((*animation)->frame_count) - 1) - objects::LoadFloat(ctx.r3.u32 + kChannelFrame24);
})

// animation_channel_get_duration(r3 channel) -> real: its animation's
// Frame Count divided by the channel's rate (+20, scaled by a read-only
// real, at least another); 0 when the channel plays nothing (name
// unconfirmed).
H3_REPLACE_FLOAT(822FB998, animation_channel_get_duration, {
    std::optional<const host::animations_block_360*> animation = ChannelAnimation(ctx.r3.u32);
    if (!animation)
        return std::nullopt;
    if (!*animation)
        return objects::LoadFloat(kRealZero);
    float rate = objects::LoadFloat(ctx.r3.u32 + kChannelFrame) * objects::LoadFloat(kRealRateScale);
    float minimum = objects::LoadFloat(kRealMinimumRate);
    if (!(rate > minimum))
        rate = minimum;
    return float((*animation)->frame_count) / rate;
})

// animation_channel_get_progress(r3 channel) -> real: the channel's frame
// (+20) over its animation's Frame Count, clamped to [0, 1] (0 for a
// count not above 0, NaN, or no animation) (name unconfirmed).
H3_REPLACE_FLOAT(822FCE78, animation_channel_get_progress, {
    std::optional<const host::animations_block_360*> animation = ChannelAnimation(ctx.r3.u32);
    if (!animation)
        return std::nullopt;
    float zero = objects::LoadFloat(kRealZero);
    if (!*animation)
        return zero;
    float count = float((*animation)->frame_count);
    if (!(count > zero))
        return zero;
    float progress = objects::LoadFloat(ctx.r3.u32 + kChannelFrame) / count;
    if (!(progress > zero))
        return zero;
    float one = objects::LoadFloat(kRealOne);
    return progress >= one ? one : progress;
})

// animation_channel_get_primary_keyframe_time(r3 channel) -> real: the
// frame of its animation's first Primary Keyframe frame event times the
// seconds-per-frame real; a read-only default when there is none (or a
// frame of -1) or the channel plays nothing (name unconfirmed).
H3_REPLACE_FLOAT(822FB898, animation_channel_get_primary_keyframe_time, {
    std::optional<const host::animations_block_360*> animation = ChannelAnimation(ctx.r3.u32);
    if (!animation)
        return std::nullopt;
    float none = objects::LoadFloat(kRealNoKeyframe);
    if (!*animation)
        return none;
    const auto& events = (*animation)->frame_events;
    if (events.storedCount > 0 && events.count != events.storedCount)
        return std::nullopt;
    int32_t frame = -1;
    for (const auto& event : events)
        if (uint16_t(event.type) == kFrameEventPrimaryKeyframe)
        {
            frame = event.frame;
            break;
        }
    if (frame == -1)
        return none;
    return float(frame) * objects::LoadFloat(kRealSecondsPerTick);
})

// animation_channel_lacks_playback_flag_1(r3 channel) -> bool: its
// animation doesn't have Playback Flags bit 1; true when the channel
// plays nothing (name unconfirmed).
H3_REPLACE_INT(822FBC10, animation_channel_lacks_playback_flag_1, {
    std::optional<const host::animations_block_360*> animation = ChannelAnimation(ctx.r3.u32);
    if (!animation)
        return std::nullopt;
    if (!*animation)
        return 1u;
    return ((*animation)->playback_flags & 2) ? 0u : 1u;
})

namespace
{
    // An animation manager (or similar) blends up to three animation
    // ids: the base one (+8) and two with s16 weights out of 8192.
    constexpr uint32_t kBlendBaseId = 8;
    constexpr uint32_t kBlendFirstId = 32;
    constexpr uint32_t kBlendSecondId = 36;
    constexpr uint32_t kBlendFirstWeight = 40;
    constexpr uint32_t kBlendSecondWeight = 42;

    // sub_820E00E8: the id with the largest weight (the base one's is 8192
    // minus the others'); an id whose animation index half is >= 0x8000
    // (negative) is skipped, ties keep the earlier.
    uint32_t DominantAnimationId(uint32_t blend)
    {
        uint32_t chosen = objects::Load32(blend + kBlendBaseId);
        int32_t weight = 0;
        int32_t baseWeight = 8192;
        if (objects::Load16(blend + kBlendFirstId + 2) < 0x8000)
        {
            int32_t first = objects::LoadS16(blend + kBlendFirstWeight);
            if (first > 0)
            {
                chosen = objects::Load32(blend + kBlendFirstId);
                weight = first;
                baseWeight = int16_t(8192 - first);
            }
        }
        if (objects::Load16(blend + kBlendSecondId + 2) < 0x8000)
        {
            int32_t second = objects::LoadS16(blend + kBlendSecondWeight);
            if (second > weight)
            {
                chosen = objects::Load32(blend + kBlendSecondId);
                weight = second;
                baseWeight = int16_t(baseWeight - second);
            }
        }
        if (baseWeight > weight)
            chosen = objects::Load32(blend + kBlendBaseId);
        return chosen;
    }
}

// animation_get_dominant_animation(r3 blend state: +0 graph datum) ->
// guest address of the animation (Animations element) with the largest
// blend weight, or 0 if its index is negative or it isn't found (name
// unconfirmed).
H3_REPLACE_INT(821022B8, animation_get_dominant_animation, {
    uint32_t id = DominantAnimationId(ctx.r3.u32);
    if (int16_t(uint16_t(id)) < 0)
        return 0u;
    std::optional<AnimationRef> found = FindAnimation(objects::Load32(ctx.r3.u32 + kChannelGraph), id);
    if (!found)
        return std::nullopt;
    return found->address;
})

// object_animation_has_playback_flag(r3 object, r4 flag bit) -> bool:
// the animation of the object's animation manager's first channel (at
// the manager's start), or of a second channel the manager's kind
// (+116, 0-2) places, has the Playback Flags bit (slw: 32-63 test
// nothing) (name unconfirmed).
H3_REPLACE_INT(820DBDC8, object_animation_has_playback_flag, {
    constexpr uint32_t kObjectAnimationOffset = 336;  // s16
    constexpr uint32_t kManagerKind = 116;
    constexpr uint32_t kManagerKinds = 0x8279D458;    // 300 bytes each: +28 the second channel's offset
    constexpr uint32_t kManagerKindBytes = 300;
    constexpr uint32_t kKindSecondOffset = 28;
    constexpr uint32_t kSecondChannel = 176;
    uint32_t bit = objects::ShiftBit(ctx.r4.u32);
    uint32_t data = objects::ObjectData(ctx, ctx.r3.u32);
    uint32_t manager = data + uint32_t(int32_t(objects::LoadS16(data + kObjectAnimationOffset)));
    // A channel's animation has the bit (false for no animation).
    auto has = [&](uint32_t channel) -> std::optional<bool> {
        if (objects::Load16(channel + kChannelAnimation) >= 0x8000)
            return false;
        std::optional<AnimationRef> found =
            FindAnimation(objects::Load32(channel + kChannelGraph), objects::Load32(channel + kChannelInheritance));
        if (!found)
            return std::nullopt;
        return found->animation && (found->animation->playback_flags & bit) != 0;
    };
    std::optional<bool> first = has(manager);
    if (!first)
        return std::nullopt;
    uint32_t kind = objects::Load32(manager + kManagerKind);
    if (kind > 2)
        return std::nullopt;  // the original reads address 28
    uint32_t offset = objects::Load32(kManagerKinds + kind * kManagerKindBytes + kKindSecondOffset);
    std::optional<bool> second = has(manager + offset + kSecondChannel);
    if (!second)
        return std::nullopt;
    return *first || *second ? 1u : 0u;
})

namespace
{
    // sub_8210A9F8 (on the graph's datum instead of its tag data): whether
    // the animation an id names has its data in its resource: its Resource
    // Group's resource (sub_82088C60, guest memory), member Resource
    // Group Member Index (48 bytes each), s16 at +14 not 0. nullopt where
    // the original reads through null (no animation, group -1).
    std::optional<bool> AnimationHasResourceData(PPCContext& ctx, uint8_t* base, uint32_t graphDatum, uint32_t id)
    {
        constexpr uint32_t kResourceGroupBytes = 12;
        constexpr uint32_t kResourceGroupHandle = 4;
        constexpr uint32_t kResourceMembers = 4;
        constexpr uint32_t kResourceMemberBytes = 48;
        constexpr uint32_t kResourceMemberFrames = 14;  // s16 (name unknown)
        std::optional<AnimationRef> found = FindAnimation(graphDatum, id);
        if (!found || !found->animation)
            return std::nullopt;
        int32_t group = found->animation->resource_group_index;
        int32_t member = found->animation->resource_group_member_index;
        const auto& groups = found->graph->tag_resource_groups;
        if (group < 0 || group >= groups.storedCount || group >= groups.count)
            return std::nullopt;
        uint32_t resource = h3::native::Call(ctx, base, sub_82088C60,
            groups.address + uint32_t(group) * kResourceGroupBytes + kResourceGroupHandle);
        uint32_t data = objects::Load32(resource + kResourceMembers) + uint32_t(member) * kResourceMemberBytes;
        return objects::LoadS16(data + kResourceMemberFrames) != 0;
    }
}

// animation_channel_has_resource_data(r3 channel) -> bool: the channel
// is active and its animation (or one blended in with a weight above 0)
// has its data in its resource (AnimationHasResourceData); its own
// animation doesn't count when the blended ones take all the weight
// (sub_820E2418: 8192 minus their weights) (name unconfirmed).
H3_REPLACE_INT(822FAE20, animation_channel_has_resource_data, {
    uint32_t channel = ctx.r3.u32;
    if (objects::Load8(channel + kChannelActive) == 0 || objects::Load32(channel + kChannelGraph) == objects::kNone ||
        objects::Load16(channel + kChannelAnimation) >= 0x8000)
        return 0u;
    uint32_t graph = objects::Load32(channel + kChannelGraph);
    std::optional<bool> result = AnimationHasResourceData(ctx, base, graph, objects::Load32(channel + kBlendBaseId));
    if (!result)
        return std::nullopt;
    int32_t weight = 8192;
    if (objects::Load16(channel + kBlendFirstId + 2) < 0x8000 && objects::LoadS16(channel + kBlendFirstWeight) > 0)
        weight = int16_t(8192 - objects::LoadS16(channel + kBlendFirstWeight));
    if (objects::Load16(channel + kBlendSecondId + 2) < 0x8000 && objects::LoadS16(channel + kBlendSecondWeight) > 0)
        weight = int16_t(weight - objects::Load16(channel + kBlendSecondWeight));
    if (weight == 8192)
        return *result ? 1u : 0u;
    bool any = weight != 0 && *result;
    const uint32_t ids[2] = { kBlendFirstId, kBlendSecondId };
    const uint32_t weights[2] = { kBlendFirstWeight, kBlendSecondWeight };
    for (int i = 0; i < 2; i++)
    {
        if (objects::Load16(channel + ids[i] + 2) >= 0x8000 || objects::LoadS16(channel + weights[i]) <= 0)
            continue;
        std::optional<bool> blended = AnimationHasResourceData(ctx, base, graph, objects::Load32(channel + ids[i]));
        if (!blended)
            return std::nullopt;
        any = any || *blended;
    }
    return any ? 1u : 0u;
})

PPC_EXTERN_FUNC(sub_822FAE20);  // animation_channel_has_resource_data (native, above)

// animation_state_has_resource_data(r3 state: two channels, at +52 and +0)
// -> animation_channel_has_resource_data of the first active one (active
// byte, graph not NONE, animation index below 0x8000), else false (name
// unconfirmed).
H3_REPLACE_INT(8210A968, animation_state_has_resource_data, {
    uint32_t state = ctx.r3.u32;
    for (uint32_t channel : { state + 52, state })
    {
        if (objects::Load8(channel + kChannelActive) == 0 || objects::Load32(channel + kChannelGraph) == objects::kNone ||
            objects::Load16(channel + kChannelAnimation) >= 0x8000)
            continue;
        return h3::native::Call(ctx, base, sub_822FAE20, channel);
    }
    return 0u;
})

// animation_channel_get_seconds_remaining(r3 channel) -> real: the frames
// remaining (animation_channel_get_frames_remaining) divided by the
// channel's rate (+20) times 30, or by 0.0001 when that isn't above it
// (constants: lis -32254 = 0x82020000, + 18700 = 0x8202490C (0.0001), +
// 26224 = 0x8202AF7C (30)) (name unconfirmed).
H3_REPLACE_FLOAT(822FBB38, animation_channel_get_seconds_remaining, {
    constexpr uint32_t kRealMinimumRate = 0x8202490C;
    constexpr uint32_t kRealFramesPerSecond = 0x8202AF7C;
    namespace objects = h3::native::objects;
    float rate = float(double(objects::LoadFloat(ctx.r3.u32 + 20)) * double(objects::LoadFloat(kRealFramesPerSecond)));
    h3::native::Call(ctx, base, sub_822FBA78, ctx.r3.u32);
    double frames = ctx.f1.f64;
    float minimum = objects::LoadFloat(kRealMinimumRate);
    return float(frames / double(rate > minimum ? rate : minimum));
})

#endif
