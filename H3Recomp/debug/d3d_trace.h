// D3D call tracing (debugging; built with -DH3_TRACE_D3D=ON).

#pragma once

// Debug aid (-DH3_TRACE_D3D=ON): counts and samples calls into the statically linked D3D library
// to identify its functions. Hooks are generated into d3d_trace_hooks.cpp.

#include <cstddef>
#include <cstdint>

struct PPCContext;

namespace h3::d3dtrace
{
    extern const uint32_t kFunctions[];
    extern const size_t kFunctionCount;

    // Called by the generated hooks around D3D function `index`.
    void Enter(size_t index, PPCContext& ctx);
    void Leave(size_t index, PPCContext& ctx);
}
