// Guest memory search for floats (float_scan.cpp).

#pragma once

#include <cstdint>

namespace h3::debug
{
    // Once per presented frame (H3_SCAN_FLOAT).
    void UpdateFloatScan(uint32_t frame);
}
