// H3_SCAN_FLOAT=<frame>:<value>[,<value>...]: at that presented frame, logs
// every guest address (virtual heaps, the image's data, physical memory)
// holding a float within 1e-5 of one of the values. For finding game state
// by a known value (the camera's FOV), then H3_WATCH_WRITES for its writer.
// H3_SCAN_FLOAT_AGAIN=<frame>,...: at those frames, the current values at
// the addresses the scan found (what changed since, e.g. zoomed in).
// H3_POKE_FLOAT=<frame>:<hex address>=<value>,...: writes those floats at
// that frame (and every frame after, to hold them), to see what they do.

#include "debug/float_scan.h"
#include "config/env.h"
#include "memory.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace h3::debug
{
    // Once per presented frame: runs a scan or poke due at this frame.
    void UpdateFloatScan(uint32_t frame)
    {
        static const char* poke = EnvString("H3_POKE_FLOAT");
        if (poke && frame >= uint32_t(strtoul(poke, nullptr, 10)))
            for (const char* p = strchr(poke, ':'); p && *p; p = strchr(p + 1, ','))
            {
                char* end = nullptr;
                uint32_t address = uint32_t(strtoul(p + 1, &end, 16));
                if (end && *end == '=')
                    GuestRef<float>(address) = float(atof(end + 1));
            }

        static std::vector<uint32_t> matches;
        static const char* again = EnvString("H3_SCAN_FLOAT_AGAIN");
        for (const char* p = again; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : nullptr)
            if (uint32_t(strtoul(p, nullptr, 10)) == frame)
                for (uint32_t address : matches)
                    fprintf(stderr, "[scan] frame %u again %08X = %.6f\n", frame, address, float(GuestRef<float>(address)));

        static const char* spec = EnvString("H3_SCAN_FLOAT");
        if (!spec)
            return;
        static uint32_t at = uint32_t(strtoul(spec, nullptr, 10));
        if (frame != at)
            return;
        std::vector<float> values;
        for (const char* p = strchr(spec, ':'); p && *p; p = strchr(p + 1, ','))
            values.push_back(float(atof(p + 1)));
        const std::pair<uint32_t, uint32_t> ranges[] = { { 0x40000000u, 0x60000000u }, { 0x70000000u, 0x7FC00000u },
            { 0x82000000u, 0x84000000u }, { 0xA0000000u, 0xC0000000u } };
        uint32_t found = 0;
        for (auto [begin, end] : ranges)
            for (uint32_t address = begin; address < end && found < 400; address += 4)
            {
                float v = GuestRef<float>(address);
                for (float target : values)
                    if (std::fabs(v - target) < 1e-5f)
                    {
                        fprintf(stderr, "[scan] frame %u %08X = %.6f\n", frame, address, v);
                        matches.push_back(address);
                        found++;
                    }
            }
        fprintf(stderr, "[scan] frame %u: %u matches\n", frame, found);
    }
}
