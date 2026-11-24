// C library functions the recompiled code needs that the Windows C runtime
// lacks (Windows builds only).
//
// roundevenf/roundeven (C23): SIMDe's float rounding (simde-math.h) uses
// __builtin_roundeven*, which becomes a call to these when the target CPU
// has no SSE4.1 round instruction. Round half to even, computed explicitly
// so it doesn't depend on the current rounding mode (the guest sets its
// own).

#include <cmath>
#include <cstdint>

// Round to nearest, ties to even (1.5 -> 2, 2.5 -> 2).
extern "C" float roundevenf(float value)
{
    float rounded = std::round(value);  // half away from zero
    if (std::fabs(rounded - value) == 0.5f && std::fmod(rounded, 2.0f) != 0.0f)
        rounded -= std::copysign(1.0f, value);
    return rounded;
}

// The same for double.
extern "C" double roundeven(double value)
{
    double rounded = std::round(value);
    if (std::fabs(rounded - value) == 0.5 && std::fmod(rounded, 2.0) != 0.0)
        rounded -= std::copysign(1.0, value);
    return rounded;
}
