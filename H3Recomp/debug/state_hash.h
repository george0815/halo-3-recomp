// Per-tick game state checksums for film replay comparison
// (debug/state_hash.cpp).

#pragma once

#include <cstdint>

namespace h3::debug
{
    // Called with the game state each time the game copies it (once per
    // game tick, hooks/block_copy.cpp): state = host pointer to its bytes,
    // address = its guest address.
    void OnGameStateCopy(const uint8_t* state, uint32_t bytes, uint32_t address);
}
