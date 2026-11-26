// Scenario block lookups (globals, zones and areas, objectives, soft
// ceilings, script point sets, zone sets), decompiled to read the host tag
// cache (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).
// The originals read the global scenario pointer (0x827CFF08) without a
// check; GlobalScenario() is its host copy.

#ifdef H3_HAVE_TAG_LAYOUTS

#include "game/game_state_layout.h"
#include "guest_call.h"
#include "objects/object_reads.h"
#include "replace.h"
#include "tags/tag_cache.h"

#include <bit>

PPC_EXTERN_FUNC(sub_820B0178);  // objective_task_reference_valid (native, below)

namespace
{
    namespace host = h3::native::tags::host;
    namespace objects = h3::native::objects;

    // The active zone set's index (next to the scenario's datum; see
    // scenario_zone_sets.cpp) and two game state masks the zone set
    // checks compare with: loaded BSPs (structures/structure_bsps.h) and
    // loaded designer zones (probably).
    constexpr uint32_t kZoneSetIndex = 0x8274B79C;
    constexpr uint32_t kActiveBspMask = 0x828A1ECC;
    constexpr uint32_t kActiveDesignerZoneMask = 0x828A1ED4;

    constexpr uint32_t kZoneSetBytes = 36;
    constexpr uint32_t kZoneSetRuntimeBspZoneFlags = 12;  // read from guest memory (may be set at load)

    // The scenario's zone set at index, or nullptr when it's past the
    // block (the originals don't check).
    const host::zone_sets_block_360* ZoneSet(const host::scenario_tag* scenario, int32_t index)
    {
        const auto& zoneSets = scenario->scenario.zone_sets;
        if (index < 0 || index >= zoneSets.storedCount || index >= zoneSets.count)
            return nullptr;
        return &zoneSets[index];
    }
    uint32_t ZoneSetRuntimeBspFlags(const host::scenario_tag* scenario, int32_t index)
    {
        return objects::Load32(scenario->scenario.zone_sets.address + uint32_t(index) * kZoneSetBytes + kZoneSetRuntimeBspZoneFlags);
    }
}

// hs_global_get_type(r3 global index) -> the global's type: for an
// external global (bit 15) the s16 in its definition (the pointer table at
// 0x82742678), else the scenario's Globals element's Type (u16; no range
// check in the original).
H3_REPLACE_INT(820AB9C0, hs_global_get_type, {
    constexpr uint32_t kExternalGlobals = 0x82742678;
    uint32_t index = ctx.r3.u32;
    if (index & 0x8000)
        return uint32_t(int32_t(objects::LoadS16(objects::Load32(kExternalGlobals + (index & 0x7FFF) * 4) + 2)));
    const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
    if (!scenario)
        return std::nullopt;
    const auto& globals = scenario->scenario.globals;
    int32_t i = int32_t(index & 0x7FFF);
    if (i >= globals.storedCount || i >= globals.count)
        return std::nullopt;
    return uint32_t(uint16_t(globals[i].type));
})

// zone_get_area(r3 s16 zone, r4 s16 area) -> guest address of the
// scenario zone's area, or 0 for either index out of range (name
// unconfirmed).
H3_REPLACE_INT(820CC0C0, zone_get_area, {
    constexpr uint32_t kAreaBytes = 108;
    int32_t zone = int16_t(uint16_t(ctx.r3.u32));
    if (zone < 0)
        return 0u;
    const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
    if (!scenario)
        return std::nullopt;
    const auto& zones = scenario->scenario.zones;
    if (zone >= zones.storedCount)
        return 0u;
    if (zone >= zones.count)
        return std::nullopt;
    int32_t area = int16_t(uint16_t(ctx.r4.u32));
    const auto& areas = zones[zone].areas;
    if (area < 0 || area >= areas.storedCount)
        return 0u;
    return areas.address + uint32_t(area) * kAreaBytes;
})

// zone_area_contains_firing_position(r3 s16 zone, r4 s16 area, r5
// firing position reference: high s16 index, low s16 zone) -> bool: the
// reference is set (low half without bit 15, not the NONE value at
// 0x8272D278), names this zone, and its index is in the area's firing
// position range (start, count) (name unconfirmed).
H3_REPLACE_INT(820CC190, zone_area_contains_firing_position, {
    constexpr uint32_t kNoFiringPosition = 0x8272D278;  // u32, read-only
    uint32_t reference = ctx.r5.u32;
    if (reference & 0x8000)
        return 0u;
    if (reference == objects::Load32(kNoFiringPosition))
        return 0u;
    int32_t zone = int16_t(uint16_t(ctx.r3.u32));
    int32_t area = int16_t(uint16_t(ctx.r4.u32));
    if (zone == -1 || area == -1)
        return 0u;
    if (zone != int16_t(uint16_t(reference)))
        return 0u;
    const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
    if (!scenario)
        return std::nullopt;
    const auto& zones = scenario->scenario.zones;
    if (zone < 0 || zone >= zones.storedCount || zone >= zones.count)
        return std::nullopt;  // no range checks in the original
    const auto& areas = zones[zone].areas;
    if (area < 0 || area >= areas.storedCount || area >= areas.count)
        return std::nullopt;
    int32_t index = int16_t(uint16_t(reference >> 16));
    int32_t start = areas[area].firing_position_start_index;
    if (index < start)
        return 0u;
    return index < start + areas[area].firing_position_count ? 1u : 0u;
})

// objective_task_reference_valid(r3 reference: +96 s16 objective, +98 s16
// task) -> bool: the objective is in the scenario's AI Objectives and the
// task (not NONE) in its tasks (name unconfirmed).
H3_REPLACE_INT(820B0178, objective_task_reference_valid, {
    int32_t objective = objects::LoadS16(ctx.r3.u32 + 96);
    if (objective < 0)
        return 0u;
    const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
    if (!scenario)
        return std::nullopt;
    const auto& objectives = scenario->scenario.ai_objectives;
    if (objective >= objectives.storedCount)
        return 0u;
    int32_t task = objects::LoadS16(ctx.r3.u32 + 98);
    if (task == -1)
        return 0u;
    if (task < 0)
        return 0u;
    if (objective >= objectives.count)
        return std::nullopt;
    return task < objectives[objective].tasks.storedCount ? 1u : 0u;
})

// scenario_soft_ceiling_get_enabled(r3 name string id) -> guest address of
// the first scenario Soft Ceiling with that name if its bit in the game
// state's enabled soft ceilings (TLS 644) is set, else 0 (name
// unconfirmed).
H3_REPLACE_INT(82227F30, scenario_soft_ceiling_get_enabled, {
    constexpr uint32_t kTlsSoftCeilingsEnabled = 644;
    constexpr uint32_t kSoftCeilingBytes = 12;
    const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
    if (!scenario)
        return std::nullopt;
    const auto& ceilings = scenario->scenario.soft_ceilings;
    if (ceilings.storedCount <= 0)
        return 0u;
    if (ceilings.count != ceilings.storedCount)
        return std::nullopt;
    for (int32_t i = 0; i < ceilings.count; i++)
    {
        if (ceilings[i].name != ctx.r3.u32)
            continue;
        uint32_t bits = objects::Load32(objects::TlsSlot(ctx, kTlsSoftCeilingsEnabled) + uint32_t(i >> 5) * 4);
        return (bits & (1u << (i & 31))) ? ceilings.address + uint32_t(i) * kSoftCeilingBytes : 0u;
    }
    return 0u;
})

// cs_point_reference_valid(r3 point reference: high u16 point set, low s16
// point) -> bool: false for NONE; the point set is in the scenario's
// Scripting Data's (first element) point sets and the point in its
// points (name unconfirmed).
H3_REPLACE_INT(824FD380, cs_point_reference_valid, {
    uint32_t reference = ctx.r3.u32;
    if (reference == 0xFFFFFFFF)
        return 0u;
    const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
    if (!scenario)
        return std::nullopt;
    const auto& data = scenario->scenario.scripting_data;
    if (data.storedCount <= 0)
        return 0u;
    if (data.count <= 0)
        return std::nullopt;
    int32_t pointSet = int32_t(reference >> 16);
    const auto& pointSets = data[0].point_sets;
    if (pointSet >= pointSets.storedCount)
        return 0u;
    int32_t point = int16_t(uint16_t(reference));
    if (point < 0)
        return 0u;
    if (pointSet >= pointSets.count)
        return std::nullopt;
    return point < pointSets[pointSet].points.storedCount ? 1u : 0u;
})

// scenario_zone_set_fully_loaded(r3 zone set index) -> bool: it's the
// active zone set and the loaded BSP and designer zone masks include its
// Runtime BSP Zone Flags and Required Designer Zones (both empty for
// NONE) (name unconfirmed).
H3_REPLACE_INT(82117F70, scenario_zone_set_fully_loaded, {
    int32_t index = int32_t(ctx.r3.u32);
    if (index != int32_t(objects::Load32(kZoneSetIndex)))
        return 0u;
    uint32_t bsps = 0, zones = 0;
    if (index != -1)
    {
        const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
        if (!scenario)
            return std::nullopt;
        const host::zone_sets_block_360* zoneSet = ZoneSet(scenario, index);
        if (!zoneSet)
            return std::nullopt;
        bsps = ZoneSetRuntimeBspFlags(scenario, index);
        zones = zoneSet->required_designer_zones;
    }
    return (bsps & objects::Load32(kActiveBspMask)) == bsps && (zones & objects::Load32(kActiveDesignerZoneMask)) == zones
        ? 1u
        : 0u;
})

// zone_set_state_needs_switch(r3 zone set state: +4 zone set index, +12
// BSP mask, +24 cinematic zone mask, r4 zone set index) -> bool: the
// state is for another zone set and lacks some of the zone set's Runtime
// BSP Zone Flags or Cinematic Zones (the original also tests the
// Forbidden Designer Zones in a way that can't fail) (name unconfirmed).
H3_REPLACE_INT(82117FF8, zone_set_state_needs_switch, {
    uint32_t state = ctx.r3.u32;
    int32_t index = int32_t(ctx.r4.u32);
    if (int32_t(objects::Load32(state + 4)) == index)
        return 0u;
    const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
    if (!scenario)
        return std::nullopt;
    const host::zone_sets_block_360* zoneSet = ZoneSet(scenario, index);
    if (!zoneSet)
        return std::nullopt;  // the original reads before or past the block (NONE included)
    uint32_t bsps = ZoneSetRuntimeBspFlags(scenario, index);
    if ((objects::Load32(state + 12) & bsps) != bsps)
        return 1u;
    uint32_t cinematic = zoneSet->cinematic_zones;
    if ((objects::Load32(state + 24) & cinematic) != cinematic)
        return 1u;
    return 0u;
})

// zone_set_portal_map_offset(r3 count, r4 base) -> base plus the Game
// Portal to Portal Map counts of the first count Portal=>Device Mapping
// elements of the active zone set's PVS (name unconfirmed).
H3_REPLACE_INT(822002E8, zone_set_portal_map_offset, {
    int32_t count = int32_t(ctx.r3.u32);
    if (count <= 0)
        return ctx.r4.u32;
    const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
    if (!scenario)
        return std::nullopt;
    const host::zone_sets_block_360* zoneSet = ZoneSet(scenario, int32_t(objects::Load32(kZoneSetIndex)));
    if (!zoneSet)
        return std::nullopt;
    const auto& sets = scenario->scenario.zone_set_potentially_visible_sets;
    int32_t pvs = zoneSet->pvs_index;
    if (pvs < 0 || pvs >= sets.storedCount || pvs >= sets.count)
        return std::nullopt;
    const auto& mappings = sets[pvs].portal_device_mapping;
    if (count > mappings.count || mappings.count != mappings.storedCount)
        return std::nullopt;
    uint32_t sum = 0;
    for (int32_t i = 0; i < count; i++)
        sum += uint32_t(mappings[i].game_portal_to_portal_map.storedCount);
    return sum + ctx.r4.u32;
})

namespace
{
    // An AI Objectives element's tasks, or nullptr when the objective is
    // out of range (the originals don't check) or unreadable.
    const host::Block<host::tasks_block>* ObjectiveTasks(const host::scenario_tag* scenario, int32_t objective)
    {
        const auto& objectives = scenario->scenario.ai_objectives;
        if (objective < 0 || objective >= objectives.storedCount || objective >= objectives.count)
            return nullptr;
        return &objectives[objective].tasks;
    }
}

// objective_task_get(r3 reference: +96 s16 objective, +98 s16 task) ->
// guest address of the task, or 0 when objective_task_reference_valid
// says no (name unconfirmed).
H3_REPLACE_INT(820B57F0, objective_task_get, {
    uint32_t reference = ctx.r3.u32;
    const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
    if (!scenario)
        return std::nullopt;
    if (h3::native::Call(ctx, base, sub_820B0178, reference) == 0)
        return 0u;
    const auto* tasks = ObjectiveTasks(scenario, objects::LoadS16(reference + 96));
    if (!tasks)
        return std::nullopt;
    constexpr uint32_t kTaskBytes = 204;
    return tasks->address + uint32_t(int32_t(objects::LoadS16(reference + 98))) * kTaskBytes;
})

// objective_task_has_exclusive_vehicle_filter(r3 objective, r4 task) ->
// bool: neither is NONE, the task's Filter is a vehicle one ("vehicle"
// through "mongoose", 34-46) and its filter flags have Exclusive (bit 0)
// (name unconfirmed).
H3_REPLACE_INT(82512108, objective_task_has_exclusive_vehicle_filter, {
    if (ctx.r3.u32 == 0xFFFFFFFF || ctx.r4.u32 == 0xFFFFFFFF)
        return 0u;
    const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
    if (!scenario)
        return std::nullopt;
    const auto* tasks = ObjectiveTasks(scenario, int16_t(uint16_t(ctx.r3.u32)));
    int32_t task = int16_t(uint16_t(ctx.r4.u32));
    if (!tasks || task < 0 || task >= tasks->storedCount || task >= tasks->count)
        return std::nullopt;  // no range checks in the original
    const host::tasks_block& element = (*tasks)[task];
    if (element.filter < 34 || element.filter > 46)
        return 0u;
    return (element.filter_flags & 1) ? 1u : 0u;
})

// objective_task_handle_get_state_value(r3 task handle: top 3 bits 0b101,
// bits 16-28 task, low s16 objective) -> s16 at +4 of the task's 56-byte
// runtime entry (TLS 732; first entry per objective: +2 of its 10-byte
// element in the TLS 368 array), or 0 when the handle isn't a task, AI
// is off (TLS 112 +1) or the objective or task is out of range (name
// unconfirmed).
H3_REPLACE_INT(82507EC0, objective_task_handle_get_state_value, {
    constexpr uint32_t kTlsObjectiveStates = 368;
    constexpr uint32_t kTlsTaskStates = 732;
    uint32_t handle = ctx.r3.u32;
    if ((handle & 0xE0000000) != 0xA0000000)
        return 0u;
    if (objects::Load8(objects::TlsSlot(ctx, h3::native::game::tls::kAiGlobals) + 1) == 0)
        return 0u;
    int32_t objective = int16_t(uint16_t(handle));
    if (objective < 0)
        return 0u;
    const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
    if (!scenario)
        return std::nullopt;
    if (objective >= scenario->scenario.ai_objectives.storedCount)
        return 0u;
    const auto* tasks = ObjectiveTasks(scenario, objective);
    if (!tasks)
        return std::nullopt;
    int32_t task = int32_t((handle >> 16) & 0x1FFF);
    if (task >= tasks->storedCount)
        return 0u;
    uint32_t objectiveStates = objects::Load32(objects::TlsSlot(ctx, kTlsObjectiveStates) + 68);
    uint32_t first = objects::Load16(objectiveStates + uint32_t(objective) * 10 + 2);
    int32_t entry = int16_t(uint16_t(first + uint32_t(task)));
    uint32_t address = objects::TlsSlot(ctx, kTlsTaskStates) + uint32_t(entry) * 56;
    return uint32_t(int32_t(objects::LoadS16(address + 4)));
})

namespace
{
    // The firing position NONE values (u32, read-only): the first means
    // none, the second the shared placeholder position below.
    constexpr uint32_t kFiringPositionNone = 0x8272D274;    // lis -32141, -11660
    constexpr uint32_t kFiringPositionShared = 0x8272D278;  // lis -32141, -11656
    constexpr uint32_t kSharedFiringPositionEnabled = 0x828C4956;  // u8; lis -32116, +18774
    constexpr uint32_t kSharedFiringPosition = 0x828A1E84;  // lis -32118, +7812
    constexpr uint32_t kTlsDynamicFiringPositions = 408;
}

// firing_position_get(r3 reference: high s16 index, low u16 zone, or with
// bit 15 set a dynamic firing position set (TLS 408 data array, low 15
// bits)) -> guest address of the firing position (40 bytes; a dynamic
// set's at +68, if its bit in the set's mask at +64 is set), the shared
// position (0x828A1E84, when its flag is set) for the second NONE value,
// or 0 (name unconfirmed).
H3_REPLACE_INT(820C5078, firing_position_get, {
    uint32_t reference = ctx.r3.u32;
    if (reference == objects::Load32(kFiringPositionNone))
        return 0u;
    if (reference == objects::Load32(kFiringPositionShared))
        return objects::Load8(kSharedFiringPositionEnabled) != 0 ? kSharedFiringPosition : 0u;
    uint32_t low = reference & 0xFFFF;
    int32_t index = int16_t(uint16_t(reference >> 16));
    if (low & 0x8000)
    {
        int32_t set = int32_t(low & 0x7FFF);
        uint32_t array = objects::TlsSlot(ctx, kTlsDynamicFiringPositions);
        if (set >= int32_t(objects::Load32(array + 56)))
            return 0u;
        uint32_t element = objects::Load32(array + 68) + objects::Load32(array + 36) * uint32_t(set);
        if (objects::Load16(element) == 0)
            return 0u;
        if (index < 0 || index >= objects::LoadS16(element + 8))
            return 0u;
        uint32_t bits = objects::Load32(element + 64 + uint32_t(index >> 5) * 4);
        if ((bits & (1u << (index & 31))) == 0)
            return 0u;
        return element + uint32_t(index) * 40 + 68;
    }
    const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
    if (!scenario)
        return std::nullopt;
    const auto& zones = scenario->scenario.zones;
    int32_t zone = int32_t(low);
    if (zone >= zones.storedCount || zone >= zones.count)
        return std::nullopt;  // the original doesn't check the zone
    if (index < 0)
        return 0u;
    const auto& positions = zones[zone].firing_positions;
    if (index >= positions.storedCount)
        return 0u;
    constexpr uint32_t kFiringPositionBytes = 40;
    return positions.address + uint32_t(index) * kFiringPositionBytes;
})

// squad_starting_location_find_object(r3 starting location handle: top 3
// bits 0b100, bits 16-28 squad, bits 8-15 fire team, low byte starting
// location) -> the first object in the squad's runtime list (squad +116,
// objects linked through +1284) whose +1288 is the starting location's
// +4 word, or NONE (also when AI is off or an index is out of range)
// (name unconfirmed).
H3_REPLACE_INT(82505EB0, squad_starting_location_find_object, {
    constexpr uint32_t kSquadStateBytes = 176;
    constexpr uint32_t kSquadStateFirstObject = 116;
    constexpr uint32_t kObjectNext = 1284;
    constexpr uint32_t kObjectStartingLocation = 1288;
    constexpr uint32_t kStartingLocationBytes = 136;
    namespace tls = h3::native::game::tls;
    uint32_t handle = ctx.r3.u32;
    if (objects::Load8(objects::TlsSlot(ctx, tls::kAiGlobals) + 1) == 0)
        return objects::kNone;
    if ((handle & 0xE0000000) != 0x80000000)
        return objects::kNone;
    int32_t squad = int32_t((handle >> 16) & 0x1FFF);
    const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
    if (!scenario)
        return std::nullopt;
    const auto& squads = scenario->scenario.squads;
    if (squad >= squads.storedCount)
        return objects::kNone;
    if (squad >= squads.count)
        return std::nullopt;
    int32_t fireTeam = int32_t((handle >> 8) & 0xFF);
    const auto& fireTeams = squads[squad].fire_teams;
    if (fireTeam >= fireTeams.storedCount)
        return objects::kNone;
    if (fireTeam >= fireTeams.count)
        return std::nullopt;
    int32_t location = int32_t(handle & 0xFF);
    const auto& locations = fireTeams[fireTeam].starting_locations;
    if (location >= locations.storedCount)
        return objects::kNone;
    uint32_t object = objects::Load32(objects::DataArrayElement(ctx, tls::kSquads, uint32_t(squad), kSquadStateBytes) + kSquadStateFirstObject);
    if (object == objects::kNone)
        return objects::kNone;
    uint32_t wanted = objects::Load32(locations.address + uint32_t(location) * kStartingLocationBytes + 4);
    for (int i = 0; i < 65536; i++)
    {
        uint32_t data = objects::ObjectData(ctx, object);
        if (objects::Load32(data + kObjectStartingLocation) == wanted)
            return object;
        object = objects::Load32(data + kObjectNext);
        if (object == objects::kNone)
            return objects::kNone;
    }
    return std::nullopt;  // a cycle: the original never returns
})

// reference_frame_find(r3 object, r4 s16 node) -> s16 index of the first
// scenario Reference Frame whose object id matches the object's (its data
// +148: same unique id, type and source, and for source 0 the same origin
// BSP) and whose node index is the node, or NONE (name unconfirmed).
H3_REPLACE_INT(8250CC18, reference_frame_find, {
    const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
    if (!scenario)
        return std::nullopt;
    const auto& frames = scenario->scenario.reference_frames;
    if (frames.storedCount > 0 && frames.count != frames.storedCount)
        return std::nullopt;
    uint32_t id = objects::ObjectData(ctx, ctx.r3.u32) + 148;
    int32_t uniqueId = int32_t(objects::Load32(id));
    int16_t originBsp = objects::LoadS16(id + 4);
    int8_t type = objects::LoadS8(id + 6);
    int8_t source = objects::LoadS8(id + 7);
    int16_t node = int16_t(uint16_t(ctx.r4.u32));
    for (int32_t i = 0; i < frames.storedCount; i++)
    {
        const host::scenario_object_id_struct& frame = frames[i].object_id;
        bool same = frame.type == type && frame.source == source && frame.unique_id == uniqueId;
        if (same && frame.source == 0)
            same = frame.origin_bsp_index == originBsp;
        if (same && frames[i].node_index == node)
            return uint32_t(int32_t(int16_t(i)));
    }
    return objects::kNone;
})

// path_machine_reference_open(r3 path data: +16 16-byte entries (+6 s16
// reference), +76 20-byte references (+0 u16 type, +8 machine index), r4
// entry) -> bool: false only when the entry's reference is a machine (type
// 10) in the scenario's Machines whose object (object globals +644 +
// (+616 first + index) * 4) is a machine (object type 7) with +372 not
// above 0.5 and either +360 not above 0 or bit 0 of +552 set (name
// unconfirmed).
H3_REPLACE_INT(820CF038, path_machine_reference_open, {
    constexpr uint32_t kRealZero = 0x8202251C;  // lis -32254, +9500
    constexpr uint32_t kRealHalf = 0x82022DC0;  // lis -32254, +9500 +2212
    namespace tls = h3::native::game::tls;
    uint32_t data = ctx.r3.u32;
    uint32_t entry = objects::Load32(data + 16) + (ctx.r4.u32 << 4);
    int32_t reference = objects::LoadS16(entry + 6);
    if (reference == -1)
        return 1u;
    uint32_t element = objects::Load32(data + 76) + uint32_t(reference) * 20;
    if (objects::Load16(element) != 10)
        return 1u;
    int32_t machine = int32_t(objects::Load32(element + 8));
    if (machine < 0)
        return 1u;
    const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
    if (!scenario)
        return std::nullopt;
    if (machine >= scenario->scenario.machines.storedCount)
        return 1u;
    uint32_t objectGlobals = objects::TlsSlot(ctx, tls::kObjectGlobals);
    int32_t first = int32_t(objects::Load32(objectGlobals + 616));
    if (first == -1)
        return 1u;
    uint32_t object = objects::Load32(objectGlobals + (uint32_t(first + machine + 161) << 2));
    if (object == objects::kNone)
        return 1u;
    uint32_t device = objects::ObjectData(ctx, object);
    if ((objects::ShiftBit(uint32_t(int32_t(objects::LoadS8(device + 154)))) & 0x80) == 0)
        return 1u;
    if (objects::LoadFloat(device + 372) > objects::LoadFloat(kRealHalf))
        return 1u;
    if (objects::LoadFloat(device + 360) <= objects::LoadFloat(kRealZero))
        return 0u;
    return (objects::Load32(device + 552) & 1) ? 0u : 1u;
})

// scenario_single_bsp_value_positive(r3 struct) -> bool: the scenario has
// one Structure BSP, or exactly one BSP is loaded (0x828A1ECC mask), and
// the struct's s32 at +712 is above 0 (name unconfirmed).
H3_REPLACE_INT(82290230, scenario_single_bsp_value_positive, {
    constexpr uint32_t kActiveBspMask = 0x828A1ECC;  // lis -32118, +7884
    const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
    if (!scenario)
        return std::nullopt;  // the original takes the +712 test with no scenario
    uint32_t value = ctx.r3.u32;
    if (scenario->scenario.structure_bsps.storedCount != 1
        && std::popcount(objects::Load32(kActiveBspMask)) != 1)  // sub_821B6FA8
        return 0u;
    return int32_t(objects::Load32(value + 712)) > 0 ? 1u : 0u;
})

#endif
