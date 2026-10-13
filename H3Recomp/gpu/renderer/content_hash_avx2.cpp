// ContentHash's AVX2 variant: xxHash's XXH3 built for AVX2, roughly twice
// the speed of its SSE2 path on large data. This file alone is compiled with
// -mavx2 (H3Recomp/CMakeLists.txt) and only called after a runtime CPU check
// (ContentHash in shaders.cpp), so the executable still runs without AVX2.
// Re-hashing staged vertex data and texture memory was ~10% of the command
// processor thread at 60 Hz.

#include <cstddef>
#include <cstdint>

// Every XXH symbol static to this file (no clash with shaders.cpp's SSE2
// copy), with the AVX2 accumulate loop.
#define XXH_INLINE_ALL
#define XXH_VECTOR XXH_AVX2
#include <xxhash.h>

namespace h3::gpu::renderer::internal
{
    uint64_t ContentHashAvx2(const void* data, size_t size)
    {
        return XXH3_64bits(data, size);
    }
}
