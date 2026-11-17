// Field of view (fov.cpp).

#pragma once

namespace h3::hooks
{
    // Once per presented frame: scales the loaded map's unit FOVs to the
    // configured FOV.
    void UpdateFov();
}
