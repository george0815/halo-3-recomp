#pragma once

#include "gpu/shader/ucode.h"

#include <cstdint>
#include <string>
#include <vector>

namespace h3::gpu::shader
{
    // Translates Xenos shader microcode into Vulkan GLSL 4.50, laid out for
    // SDL_GPU's SPIR-V binding model:
    //
    //   vertex:   set 0 = sampled textures (bindings 0..), then the vertex
    //                     memory storage buffer
    //             set 1 = uniform buffers: 0 float constants, 1 system
    //   fragment: set 2 = sampled textures
    //             set 3 = uniform buffers: 0 float constants, 1 system
    //
    // Vertex fetch reads raw guest memory (uploaded unchanged) from the
    // storage buffer; the system constants say where each vertex fetch
    // constant's data starts, how long it is and how to byte-swap it.
    //
    // System constant block (std140), shared by both stages:
    //   uvec4 bools[2];          bool constants, 256 bits
    //   uvec4 loops[8];          loop constants 0-31
    //   uvec4 vfetch[96];        x = first dword in the storage buffer,
    //                            y = dword count, z = endian mode
    //   vec4  ndc_scale;         xyz: gl_Position.xyz = pos.xyz * scale
    //   vec4  ndc_offset;        xyz:                   + pos.w * offset
    //   uint  ps_param_gen;      register for pixel parameters, >= 64 = none
    //   uint  flags;             bit 0: rectangle list (see kVertexMain),
    //                            bit 1: round depth to 24-bit float,
    //                            bits 2-4: PA_CL_VTE_CNTL XY/Z divided by W,
    //                            W not reciprocal
    //   uint  rect_indices;      rectangle lists: dword offset of the guest
    //                            vertex indices (3 per rectangle, host-endian)
    //                            in the vertex memory buffer
    //   uvec4 tfetch[32];        per texture fetch constant: x = swizzle,
    //                            y = sign modes, z = exp adjust, w = apply
    //                            (textures decoded from memory)
    //   vec4  color_exp_bias;    pixel shaders: color output i *= [i]
    //   float alpha_ref;         alpha test reference (flags bits 5-8:
    //                            function, enable)
    struct TextureBinding
    {
        uint32_t fetchConstant;
        ucode::TextureDimension dimension;
        uint32_t binding;
    };

    struct ShaderInfo
    {
        bool isVertex = false;
        uint32_t registerCount = 64;
        std::vector<uint32_t> vertexFetchConstants;  // used vfetch constant indices
        std::vector<TextureBinding> textures;
        uint32_t storageBufferBinding = 0;  // vertex shaders: after the textures
        uint32_t colorTargetMask = 0;       // pixel shaders: color exports written
        bool writesDepth = false;
        bool usesKill = false;
        uint32_t interpolatorMask = 0;  // vertex shaders: interpolators written
    };

    struct TranslationResult
    {
        bool ok = false;
        std::string glsl;
        std::string error;
        ShaderInfo info;
    };

    // dwords are host-endian (swap guest memory first).
    TranslationResult Translate(bool isVertex, const uint32_t* dwords, uint32_t dwordCount);

    // Microcode disassembly, for debugging the translator.
    std::string Disassemble(bool isVertex, const uint32_t* dwords, uint32_t dwordCount);
}
