// Environment variable switches. Debugging and tuning options are all set
// with H3_* environment variables (the README lists them). These helpers
// read them; call sites cache the result in a function-level static so the
// environment is only scanned once:
//
//     static const bool log = EnvFlag("H3_LOG_FOO");

#pragma once

#include <cstdint>
#include <cstdlib>

namespace h3
{
    // True if the variable is set (to anything, even empty).
    inline bool EnvFlag(const char* name)
    {
        return std::getenv(name) != nullptr;
    }

    // The variable's text, or nullptr if it isn't set.
    inline const char* EnvString(const char* name)
    {
        return std::getenv(name);
    }

    // The variable as an unsigned number (decimal, or hex with 0x), or
    // fallback if it isn't set.
    inline uint32_t EnvNumber(const char* name, uint32_t fallback = 0)
    {
        const char* value = std::getenv(name);
        return value ? uint32_t(std::strtoul(value, nullptr, 0)) : fallback;
    }
}
