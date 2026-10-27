// Game state checksums, one per game tick: the oracle for "this film
// replays the same game" (docs/ROADMAP.md 6.1; tools/films/README.md).
//
// H3_STATE_HASH=<file>   appends "<copy number> <game tick> <hash>" per copy
// H3_STATE_MASK=<file>   leaves cosmetic state out of the hash (below)
// H3_STATE_DUMP=<t>,...  also saves the raw state at those level times
//                        (below; what compare_hashes.py reports) as
//                        state_<level time>.bin in the working directory.
//                        Not game ticks: a checkpoint revert replays ticks,
//                        which overwrote the first copy's dump
// H3_CAPTURE_LEVEL=<t>,... requests a GPU frame capture (as F9, or F10 with
//                        H3_CAPTURE_DEEP; gpu/frame_capture.h) at those
//                        level times: H3_CAPTURE_AT counts presented frames,
//                        which land on different moments from run to run
// H3_SNAPSHOT_LEVEL=<first>-<last>[/<every>] saves the frame presented
//                        after those level times as level_<t>.png (every
//                        level tick, or every <every>-th): screenshots at
//                        the same moments each run, unlike H3_SNAPSHOT_*
// H3_STATE_RECORD=<file> records every tick's raw state compactly: per
//                        copy "u32 tick, u32 n, n x (u32 word index, u32
//                        word as stored)" with the words that changed since
//                        the previous copy (the first copy: all of them).
//                        learn_mask.py --record rebuilds every tick from it
// H3_STATE_RECORD_TICKS=<first>-<last>[,<first>-<last>...]
//                        records only those spans of level time (below; the
//                        first copy recorded is complete; after a gap the
//                        next copy holds every word changed since the last
//                        one recorded), so a long film can be recorded
//                        around its divergences (~160 KB per tick). The record's tick field is then the level
//                        time. Copy numbers don't line up between runs;
//                        level times do.
//
// Level time counts the copies of the level (from the last drop of the
// tick counter to below kLevelStartTicks: the menus count from 0 too, and
// Ark and Crow's Nest reset it again ~6000 ticks in; compare_hashes.py
// counts from the last drop as well) whose
// tick differs from the previous copy's: game ticks played. Unlike the game
// tick it keeps going up when a checkpoint revert (the player died in the
// film) sends the tick counter back. Copies repeating the previous tick
// (Theater holding at the film's end) don't count and aren't recorded.
// tools/films/compare_hashes.py works out the same from H3_STATE_HASH.
//
// The game copies its whole 4 MB game state once per tick (checkpoints and
// revert), which is where this hooks in. Runs only replay alike with
// H3_RANDOM_SEED (hooks/random_seed.cpp) and H3_LOCKSTEP
// (hooks/tick_rate.cpp). Even then cosmetic state differs from run to run:
// effects and a few object fields depend on when asynchronous loads finish.
// The mask file (tools/films/learn_mask.py learns it from a few replays of
// a film) names what to leave out, one rule per line:
//
//   array <name>           a data array, header and elements
//   after <name>           the memory between that array and the next one
//   after <name> @ <off>   just the 64-byte block <off> bytes after its end
//                          (for big gaps holding other subsystems' state)
//   object <tag> <offset>  a 32-bit field of every object whose definition
//                          tag (the object's first word) is <tag>
//   word <offset>          one 32-bit word of the state (its globals)
//   free object memory     object memory that no live object uses (after
//                          the "object" array, past each object's size):
//                          deleted objects' stale bytes, not game state
//
// Tags and offsets are hex. Data arrays are found by their "d@t@"
// signature (header layout below); objects through the "object" array's
// handles.

#include "debug/state_hash.h"
#include "config/env.h"
#include "gpu/frame_capture.h"

#include <algorithm>
#include <utility>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace
{
    // Offset of the game's tick counter in the game state (big-endian;
    // ticks since the level started: game_time_globals' elapsed ticks).
    constexpr uint32_t kTickOffset = 0x220;
    // A tick counter drop to below this starts the level (compare_hashes.py
    // LEVEL_START_TICKS); a revert goes back to its checkpoint's tick.
    constexpr uint32_t kLevelStartTicks = 100;

    // Data array header (0x54 bytes, the elements follow it):
    //   +0x00 char[32] name     +0x20 s32 maximum count  +0x24 s32 element size
    //   +0x2C 'd@t@' signature  +0x44 elements' guest address
    //   +0x50 u32 total bytes (header, elements and in-use bit vector)
    constexpr uint32_t kArrayNameBytes = 32;
    constexpr uint32_t kArrayMaxCount = 0x20;
    constexpr uint32_t kArraySignature = 0x2C;
    constexpr uint32_t kArrayElements = 0x44;
    constexpr uint32_t kArrayTotalBytes = 0x50;
    constexpr uint32_t kArrayHeaderBytes = 0x54;
    constexpr uint32_t kSignature = 0x64407440;  // "d@t@"

    // The "object" array's elements are 16-byte handles; +12 is the guest
    // address of the object's data (0 = free slot).
    constexpr uint32_t kObjectHandleBytes = 16;
    constexpr uint32_t kObjectHandleData = 12;
    constexpr uint32_t kObjectHandleSize = 6;  // u16: the object's size in bytes

    uint32_t Load32(const uint8_t* p)
    {
        uint32_t value;
        memcpy(&value, p, 4);
        return __builtin_bswap32(value);
    }

    struct DataArray
    {
        uint32_t start, end;  // offsets in the state
        std::string name;
    };

    // Every data array in the state, by offset.
    std::vector<DataArray> FindArrays(const uint8_t* state, uint32_t bytes)
    {
        std::vector<DataArray> arrays;
        for (uint32_t at = kArraySignature; at + kArrayHeaderBytes <= bytes; at += 4)
        {
            if (Load32(state + at) != kSignature)
                continue;
            uint32_t start = at - kArraySignature;
            const char* name = reinterpret_cast<const char*>(state + start);
            uint32_t total = Load32(state + start + kArrayTotalBytes);
            arrays.push_back({ start, std::min(bytes, start + total), std::string(name, strnlen(name, kArrayNameBytes)) });
        }
        return arrays;
    }

    constexpr uint32_t kGapBlockBytes = 64;

    struct Mask
    {
        std::vector<std::string> arrays, after;
        std::vector<std::pair<std::string, uint32_t>> afterBlocks;  // array, offset past its end
        std::unordered_map<uint32_t, std::vector<uint32_t>> objectFields;  // tag -> offsets
        std::vector<uint32_t> words;
        bool freeObjectMemory = false;
        bool Empty() const
        {
            return arrays.empty() && after.empty() && afterBlocks.empty() && objectFields.empty() && words.empty() &&
                !freeObjectMemory;
        }
    };

    Mask ReadMask(const char* path)
    {
        Mask mask;
        FILE* file = fopen(path, "r");
        if (!file)
        {
            fprintf(stderr, "[state] can't read mask %s\n", path);
            return mask;
        }
        char line[256];
        while (fgets(line, sizeof(line), file))
        {
            std::string text(line);
            // A line longer than the buffer (learn_mask.py's header comment
            // lists the records): read the rest so it isn't taken for rules.
            while (!text.empty() && text.back() != '\n' && fgets(line, sizeof(line), file))
                text += line;
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
                text.pop_back();
            if (text.empty() || text[0] == '#')
                continue;
            size_t space = text.find(' ');
            std::string kind = text.substr(0, space), rest = space == std::string::npos ? "" : text.substr(space + 1);
            unsigned tag, offset;
            if (text == "free object memory")
                mask.freeObjectMemory = true;
            else if (kind == "array")
                mask.arrays.push_back(rest);
            else if (kind == "after" && rest.find(" @ ") != std::string::npos)
                mask.afterBlocks.push_back({ rest.substr(0, rest.find(" @ ")), uint32_t(strtoul(rest.c_str() + rest.find(" @ ") + 3, nullptr, 16)) });
            else if (kind == "after")
                mask.after.push_back(rest);
            else if (kind == "object" && sscanf(rest.c_str(), "%x %x", &tag, &offset) == 2)
                mask.objectFields[tag].push_back(offset & ~3u);
            else if (kind == "word" && sscanf(rest.c_str(), "%x", &offset) == 1)
                mask.words.push_back(offset & ~3u);
            else
                fprintf(stderr, "[state] mask: can't parse \"%s\"\n", text.c_str());
        }
        fclose(file);
        return mask;
    }

    bool Contains(const std::vector<std::string>& names, const std::string& name)
    {
        for (const std::string& n : names)
            if (n == name)
                return true;
        return false;
    }

    // Zeroes what the mask names in out (a copy of the state at address).
    // "free object memory": zeroes the object memory after the "object"
    // array (up to the next array) that no live object covers, from the
    // first live object on (what comes before is the pool's own header).
    void ClearFreeObjectMemory(const std::vector<DataArray>& arrays, const DataArray& objects, uint8_t* out,
        uint32_t bytes, uint32_t address)
    {
        uint32_t gapEnd = bytes;
        for (const DataArray& other : arrays)
            if (other.start >= objects.end && other.start < gapEnd)
                gapEnd = other.start;
        static std::vector<std::pair<uint32_t, uint32_t>> live;  // start, end
        live.clear();
        int32_t count = int32_t(Load32(out + objects.start + kArrayMaxCount));
        for (int32_t slot = 0; slot < count; slot++)
        {
            uint32_t handle = objects.start + kArrayHeaderBytes + uint32_t(slot) * kObjectHandleBytes;
            if (handle + kObjectHandleBytes > objects.end)
                break;
            uint32_t pointer = Load32(out + handle + kObjectHandleData);
            uint32_t object = pointer - address;
            if (!pointer || object < objects.end || object >= gapEnd)
                continue;
            uint32_t size = uint32_t(out[handle + kObjectHandleSize]) << 8 | out[handle + kObjectHandleSize + 1];
            live.push_back({ object, std::min(object + size, gapEnd) });
        }
        if (live.empty())
            return;
        std::sort(live.begin(), live.end());
        uint32_t cursor = live.front().first;
        for (const auto& [start, end] : live)
        {
            if (start > cursor)
                memset(out + cursor, 0, start - cursor);
            cursor = std::max(cursor, end);
        }
        if (gapEnd > cursor)
            memset(out + cursor, 0, gapEnd - cursor);
    }

    void ApplyMask(const Mask& mask, uint8_t* out, uint32_t bytes, uint32_t address)
    {
        // PERF: rescans 4 MB for array headers every tick (~1 ms); the
        // layout only changes when a level loads.
        std::vector<DataArray> arrays = FindArrays(out, bytes);
        for (size_t i = 0; i < arrays.size(); i++)
        {
            const DataArray& array = arrays[i];
            if (array.name == "object" && mask.freeObjectMemory)
                ClearFreeObjectMemory(arrays, array, out, bytes, address);
            if (array.name == "object")
            {
                // Object fields first: the handles live in this array.
                int32_t count = int32_t(Load32(out + array.start + kArrayMaxCount));
                for (int32_t slot = 0; slot < count && !mask.objectFields.empty(); slot++)
                {
                    uint32_t handle = array.start + kArrayHeaderBytes + uint32_t(slot) * kObjectHandleBytes;
                    if (handle + kObjectHandleBytes > array.end)
                        break;
                    uint32_t object = Load32(out + handle + kObjectHandleData) - address;
                    if (!Load32(out + handle + kObjectHandleData) || object + 4 > bytes)
                        continue;
                    auto fields = mask.objectFields.find(Load32(out + object));
                    if (fields == mask.objectFields.end())
                        continue;
                    // Bounded by the object's own size (as learn_mask.py
                    // classifies fields): some objects are over 0x4000
                    // bytes (Tsavo has fields to mask at 0x4980).
                    uint32_t size = uint32_t(out[handle + kObjectHandleSize]) << 8 | out[handle + kObjectHandleSize + 1];
                    for (uint32_t field : fields->second)
                        if (field + 4 <= size && object + field + 4 <= bytes)
                            memset(out + object + field, 0, 4);
                }
            }
            if (Contains(mask.arrays, array.name))
                memset(out + array.start, 0, array.end - array.start);
            for (const auto& [name, offset] : mask.afterBlocks)
                if (name == array.name && array.end + offset + kGapBlockBytes <= bytes)
                    memset(out + array.end + offset, 0, kGapBlockBytes);
            if (Contains(mask.after, array.name))
            {
                uint32_t next = bytes;
                for (const DataArray& other : arrays)
                    if (other.start >= array.end && other.start < next)
                        next = other.start;
                memset(out + array.end, 0, next - array.end);
            }
        }
        for (uint32_t word : mask.words)
            if (word + 4 <= bytes)
                memset(out + word, 0, 4);
    }

    // H3_STATE_RECORD: appends the words of state that changed since the
    // last call (all of them the first time).
    void Record(FILE* file, const uint8_t* state, uint32_t bytes, uint32_t tick)
    {
        static std::vector<uint32_t> previous;
        static std::vector<uint32_t> changes;  // index, value pairs
        const uint32_t* words = reinterpret_cast<const uint32_t*>(state);
        uint32_t count = bytes / 4;
        bool full = previous.size() != count;
        if (full)
            previous.assign(count, 0);
        changes.clear();
        // PERF: compares all 1M words per tick (~1 ms); testing only.
        for (uint32_t i = 0; i < count; i++)
            if (full || words[i] != previous[i])
            {
                changes.push_back(i);
                changes.push_back(words[i]);
                previous[i] = words[i];
            }
        uint32_t header[2] = { tick, uint32_t(changes.size() / 2) };
        fwrite(header, sizeof(header), 1, file);
        fwrite(changes.data(), sizeof(uint32_t), changes.size(), file);
        // Complete copies only: the run is usually stopped by a signal,
        // which would drop whatever stdio still buffers (learn_mask.py then
        // saw a half-applied copy as whole objects differing).
        fflush(file);
    }

    // H3_STATE_DUMP / H3_CAPTURE_LEVEL: whether this level time is in the list.
    bool DumpRequested(const char* list, uint32_t levelTime)
    {
        char key[24];
        snprintf(key, sizeof(key), ",%u,", levelTime);
        return strstr((std::string(",") + list + ",").c_str(), key) != nullptr;
    }
}

void h3::debug::OnGameStateCopy(const uint8_t* state, uint32_t bytes, uint32_t address)
{
    static FILE* out = [] {
        const char* path = EnvString("H3_STATE_HASH");
        return path ? fopen(path, "w") : nullptr;
    }();
    if (!out)
        return;
    // Where the state lives: pointers inside it only compare between runs
    // when this matches (it does with H3_RANDOM_SEED).
    static bool logged = [&] {
        fprintf(stderr, "[state] game state at 0x%08X (%u bytes)\n", address, bytes);
        return true;
    }();
    (void)logged;
    static const Mask mask = [] {
        const char* path = EnvString("H3_STATE_MASK");
        return path ? ReadMask(path) : Mask{};
    }();

    uint32_t tick = Load32(state + kTickOffset);
    static std::vector<uint8_t> masked;
    const uint8_t* hashed = state;
    if (!mask.Empty())
    {
        masked.assign(state, state + bytes);
        ApplyMask(mask, masked.data(), bytes, address);
        hashed = masked.data();
    }
    static uint64_t copies = 0;
    uint64_t copy = copies;
    fprintf(out, "%llu %u %016llx\n", (unsigned long long)copies++, tick, (unsigned long long)XXH3_64bits(hashed, bytes));
    fflush(out);

    static FILE* record = [] {
        const char* path = EnvString("H3_STATE_RECORD");
        return path ? fopen(path, "wb") : nullptr;
    }();
    // Level time (top of the file); held: this copy repeats the last tick.
    static uint32_t lastTick = 0;
    static bool inLevel = false;
    static uint32_t levelTime = 0;
    bool held = copy != 0 && tick == lastTick;
    if (copy != 0 && tick < lastTick && tick < kLevelStartTicks)
    {
        inLevel = true;
        levelTime = 0;
    }
    else if (inLevel && !held)
        levelTime++;
    lastTick = tick;
    static const std::vector<std::pair<uint32_t, uint32_t>> recordSpans = [] {
        std::vector<std::pair<uint32_t, uint32_t>> spans;
        const char* spec = EnvString("H3_STATE_RECORD_TICKS");
        while (spec && *spec)
        {
            char* end;
            uint32_t first = uint32_t(strtoul(spec, &end, 10));
            uint32_t last = *end == '-' ? uint32_t(strtoul(end + 1, &end, 10)) : first;
            spans.push_back({ first, last });
            spec = *end == ',' ? end + 1 : "";
        }
        return spans;
    }();
    static const bool windowed = EnvString("H3_STATE_RECORD_TICKS") != nullptr;
    bool inWindow = !windowed;
    if (windowed && inLevel && !held)
        for (const auto& [first, last] : recordSpans)
            inWindow = inWindow || (levelTime >= first && levelTime <= last);
    if (record && inWindow)
        Record(record, state, bytes, windowed ? levelTime : tick);

    static const char* dumps = EnvString("H3_STATE_DUMP");
    if (dumps && inLevel && !held && DumpRequested(dumps, levelTime))
    {
        char name[40];
        snprintf(name, sizeof(name), "state_%u.bin", levelTime);
        if (FILE* file = fopen(name, "wb"))
        {
            fwrite(state, 1, bytes, file);
            fclose(file);
        }
    }

    static const auto snapshots = [] {
        struct { uint32_t first = 1, last = 0, every = 1; } range;
        if (const char* spec = EnvString("H3_SNAPSHOT_LEVEL"))
            if (sscanf(spec, "%u-%u/%u", &range.first, &range.last, &range.every) < 2 || range.every == 0)
                range.every = 1;
        return range;
    }();
    if (inLevel && !held && levelTime >= snapshots.first && levelTime <= snapshots.last &&
        (levelTime - snapshots.first) % snapshots.every == 0)
        h3::gpu::capture::RequestSnapshot(levelTime);

    static const char* captures = EnvString("H3_CAPTURE_LEVEL");
    if (captures && inLevel && !held && DumpRequested(captures, levelTime))
    {
        static const bool deep = EnvFlag("H3_CAPTURE_DEEP");
        fprintf(stderr, "[state] capture requested at level time %u\n", levelTime);
        h3::gpu::capture::RequestCapture(deep);
    }
}
