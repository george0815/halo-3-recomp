// Centered crosshair ([video] centered_crosshair).
//
// sub_82205690 (MCC FUN_1404e1c80) finishes an observer result (a view):
// it stores the vertical FOV (+0x68) and FOV / 70 degrees (+0x6C), then
// pitches the view up by atan(tan(vfov / 2) * result+0x60), so the aim
// (the view's forward before that) lands that far below the middle of the
// screen: +0x60 is the crosshair's height in -1..1 (0.17 on foot), and the
// HUD draws the crosshair there too. Zero centres both, as MCC's option
// does. Only the camera changes, not the aim or the simulation.

#include "config/env.h"
#include "config/settings.h"
#include "memory.h"

#include <ppc_recomp_shared.h>

#include <cstdlib>

namespace
{
    constexpr uint32_t kResultCrosshairY = 0x60;
}

PPC_FUNC_IMPL(__imp__sub_82205690);
PPC_FUNC(sub_82205690)
{
    // H3_CROSSHAIR_Y=<-1..1> (testing) overrides the height.
    static const char* testY = h3::EnvString("H3_CROSSHAIR_Y");
    static const bool centered = h3::config::Active().video.centeredCrosshair;
    if (testY)
        h3::GuestRef<float>(ctx.r3.u32 + kResultCrosshairY) = float(atof(testY));
    else if (centered)
        h3::GuestRef<float>(ctx.r3.u32 + kResultCrosshairY) = 0.0f;
    __imp__sub_82205690(ctx, base);
}
