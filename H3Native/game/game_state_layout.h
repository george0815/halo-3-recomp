// Game state layout: TLS slots, data array headers and object fields, for
// decompiled code reading the game state in guest memory (stage
// "decompiled in place"). Only fields confirmed by the retail code are
// here (tools/re/data_arrays.py lists the slots of a recorded snapshot),
// cross-checked against the x64 builds' allocations where noted.
//
// The game state is reached through the thread block: the word at r13 + 0
// points at the TLS array, one guest address per slot (the slot is the
// byte offset). Most slots hold a data array (header below) or a globals
// structure allocated with a name; objects/object_access.h reads them.

#pragma once

#include <cstdint>

namespace h3::native::game
{
    // TLS slots: data arrays ("name", element bytes).
    namespace tls
    {
        constexpr uint32_t kActors = 92;          // "actor", 2700
        constexpr uint32_t kPlayers = 96;         // "players", 880
        constexpr uint32_t kPropReferences = 104; // "prop_ref", 60
        constexpr uint32_t kSquads = 132;         // "squad", 176
        constexpr uint32_t kEffects = 136;        // "effect", 156
        constexpr uint32_t kProps = 144;          // "prop", 196
        constexpr uint32_t kCommandScripts = 148; // "command scripts", 392
        constexpr uint32_t kObjects = 168;        // "object", 16 (headers)
        constexpr uint32_t kLights = 232;         // "lights", 228
        constexpr uint32_t kCloth = 320;          // "cloth", 5892
        constexpr uint32_t kSquadGroups = 324;    // "squad group", 36
        constexpr uint32_t kHsGlobals = 456;      // "hs globals", 8
        constexpr uint32_t kAntennas = 508;       // "antenna", 100

        // TLS slots: globals structures (allocation name, bytes).
        constexpr uint32_t kGameEngineGlobals = 100;     // "game engine globals"
        constexpr uint32_t kAiGlobals = 112;             // "ai globals"
        constexpr uint32_t kPlayerControlGlobals = 120;  // "player control globals"
        constexpr uint32_t kPlayersGlobals = 124;        // "players globals", 284
        constexpr uint32_t kGameGlobals = 128;           // "game globals"
        constexpr uint32_t kObjectGlobals = 176;         // "object globals"
        constexpr uint32_t kGameTimeGlobals = 188;       // "game time globals", 44
        constexpr uint32_t kPlayerMappingGlobals = 216;  // "player mapping globals", 232
        constexpr uint32_t kPhysicsConstants = 228;      // "physics constants"
        constexpr uint32_t kObjectBroadphase = 192;      // "object_broadphase"
        constexpr uint32_t kGameStateTiming = 208;       // "timing samples" (game state), 20
        constexpr uint32_t kChudUserData = 220;          // "persistent user data", 4 x 10368
        constexpr uint32_t kCortanaEffect = 248;         // "cortana effect", 16
        constexpr uint32_t kRasterizerGameStates = 284;  // "rasterizer game states", 520
        constexpr uint32_t kImpactGlobals = 316;         // "impact globals", 140
        constexpr uint32_t kChudGlobalData = 348;        // "persistent global data", 432
        constexpr uint32_t kLocalGameEngineGlobals = 376;// "local game engine globals", 196
        constexpr uint32_t kImplicitGeometry = 468;      // "implicit geometry data", 0x2104
        constexpr uint32_t kRenderTiming = 472;          // "timing samples" (render), 20
        constexpr uint32_t kActorFiringPositions = 576;  // actor firing-position owners, 0x2040
        constexpr uint32_t kDirectorScripting = 604;     // "director scripting", 1
        constexpr uint32_t kBinkGameState = 608;         // "bink game state", 8
        constexpr uint32_t kParticleEmitterStorage = 664;// GPU particle emitters storage, 0x8C000
        constexpr uint32_t kContrailStorage = 668;       // GPU contrails storage, 0x28000
        constexpr uint32_t kSoundDeterministic = 720;    // "deterministic game sound globals", 0x1300
        constexpr uint32_t kLightData = 736;             // "nondet_light_data", 0x2580

        // Other slots.
        constexpr uint32_t kObjectPool = 8;              // "objects" memory pool, 0x180054
        constexpr uint32_t kCollideableClusterFirst = 20;    // cluster partition first references, 0x4000
        constexpr uint32_t kNoncollideableClusterFirst = 32; // (each before its two data arrays)
        constexpr uint32_t kLightClusterFirst = 44;
        constexpr uint32_t kHeapBases = 48;              // TLS[48 + 4 * heap]: game state heap 0..3 base (2 null on the main thread)
        constexpr uint32_t kThreadIndex = 116;           // registered thread index, main thread = 1
    }

    // Cache file globals (fixed address).
    namespace cache_file
    {
        constexpr uint32_t kGlobals = 0x82945F50;
        constexpr uint32_t kHeader = 0x10;       // 0x3000-byte cache file header ('head')
        constexpr uint32_t kTagsHeader = 0x3010; // guest address of the tags header
    }

    // Game globals (TLS kGameGlobals).
    namespace game_globals
    {
        constexpr uint32_t kOptions = 16;               // game options; +0 s32 game mode
        constexpr uint32_t kCampaignDifficulty = 332;   // s16
        constexpr int32_t kGameModeCampaign = 1;
        constexpr int32_t kGameModeMultiplayer = 2;
    }

    // Player mapping globals (TLS kPlayerMappingGlobals).
    namespace player_mapping
    {
        constexpr uint32_t kActiveOutputUserCount = 180;  // s16
        constexpr uint32_t kOutputUserPlayers = 184;      // player datum per output user
    }

    // Data array header (any TLS data array).
    namespace data_array
    {
        constexpr uint32_t kName = 0x00;             // char[32]
        constexpr uint32_t kMaximumCount = 0x20;     // s32
        constexpr uint32_t kElementBytes = 0x24;     // s32
        constexpr uint32_t kSignature = 0x2C;        // 'd@t@'
        constexpr uint32_t kFirstUnallocated = 0x38; // s32
        constexpr uint32_t kActualCount = 0x3C;      // s32
        constexpr uint32_t kElements = 0x44;         // guest address
        constexpr uint32_t kSignatureValue = 0x64407440;
    }

    // Object data (the guest address in the object header's +12).
    namespace object
    {
        constexpr uint32_t kDefinition = 0;    // tag datum
        constexpr uint32_t kNextSibling = 8;   // object datum, NONE at the end
        constexpr uint32_t kFirstChild = 12;   // object datum, NONE if none
        constexpr uint32_t kParent = 16;       // object datum, NONE if none
        constexpr uint32_t kType = 154;        // s8: object type (below)

        // Object types (kType, and the object header's byte +3).
        enum Type : int8_t
        {
            kBiped = 0,
            kVehicle = 1,
            kWeapon = 2,
            kEquipment = 3,
            kTerminal = 4,
            kProjectile = 5,
            kScenery = 6,
            kMachine = 7,
            kControl = 8,
            kSoundScenery = 9,
            kCrate = 10,
            kCreature = 11,
            kGiant = 12,
            kEffectScenery = 13,
        };
    }

    // Unit object data (bipeds, vehicles, giants).
    namespace unit
    {
        constexpr uint32_t kParentSeat = 582;     // s16: seat index in the parent's unit seats, NONE if not seated
        constexpr uint32_t kCurrentWeapon = 602;  // s8: index into the weapons (objects::kUnitWeapons), NONE for none
    }

    // Weapon object data.
    namespace weapon
    {
        // Per trigger (definition's new triggers block), 12 bytes each:
        // +0 u8 state.
        constexpr uint32_t kTriggerStates = 540;
        constexpr uint32_t kTriggerStateBytes = 12;
    }

    // Actor (TLS kActors element).
    namespace actor
    {
        constexpr uint32_t kCharacter = 92;            // 'char' tag datum
        constexpr uint32_t kLastGuardianSurge = 760;   // s32 game time, -1 if never
    }

    // Game time globals (TLS kGameTimeGlobals).
    namespace game_time
    {
        constexpr uint32_t kTicksPerSecond = 4;  // u16
        constexpr uint32_t kSecondsPerTick = 8;  // real
        constexpr uint32_t kElapsedTicks = 12;   // s32: the game time
        constexpr uint32_t kSpeed = 16;          // real
    }
}
