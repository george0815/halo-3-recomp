#pragma once

#include <cstdint>

namespace h3::kernel
{
    // Guest address of an xboxkrnl data export (KeTimeStampBundle,
    // VdHSIOCalibrationLock, ...) by ordinal, created on first use. 0 for
    // ordinals that aren't implemented. XenonRecomp only resolves function
    // imports; the module loader patches these into variable import slots.
    uint32_t KernelVariable(uint16_t ordinal);
}
