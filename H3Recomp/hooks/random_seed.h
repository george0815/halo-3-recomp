// Fixed random seeds for replay comparison (hooks/random_seed.cpp).

#pragma once

namespace h3::hooks
{
    // Under H3_RANDOM_SEED: sets the local random seed back to its fixed
    // start, so how far loading screens stepped it doesn't carry into the
    // level (called when a level's game time starts). Otherwise nothing.
    void ResetLocalRandomSeed();
}
