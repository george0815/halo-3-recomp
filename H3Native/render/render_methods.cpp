// Render method lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).
//
// Every shader group ('rmsh', 'rmw ', ...) derives from 'rm  ', whose part
// (host::render_method_struct_definition) comes first.

#ifdef H3_HAVE_TAG_LAYOUTS

#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    constexpr uint32_t kRenderMethodGroup = 0x726D2020;  // 'rm  '
    constexpr uint32_t kNone = 0xFFFFFFFF;
}

// render_method_is_blended(r3 render method datum) -> whether its first
// postprocess entry's blend mode isn't opaque (0); false for NONE. The
// original reads the first entry without checking the count.
H3_REPLACE_INT(82163490, render_method_is_blended, {
    uint32_t method = ctx.r3.u32;
    if (method == kNone)
        return 0u;
    const auto* part = h3::native::tags::TagPartBySlot<host::render_method_struct_definition>(method & 0xFFFF, kRenderMethodGroup);
    if (!part)
        return std::nullopt;
    const auto& postprocess = part->postprocess;
    if (postprocess.storedCount <= 0 || postprocess.count <= 0)
        return std::nullopt;
    return postprocess[0].blend_mode != 0 ? 1u : 0u;
})

#endif

#ifdef H3_HAVE_TAG_LAYOUTS
// Vertex shader constants of a render method definition's shared vertex
// shaders ('glvs'): for a vertex type, entry point 0's default compiled
// shader, whose Constants block (+40) is returned.
namespace
{
    constexpr uint32_t kCompiledVertexShaderBytes = 80;
    constexpr uint32_t kCompiledVertexShaderConstants = 40;
    constexpr uint32_t kTagReferenceDatum = 12;

    // The guest address of the vertex type's compiled shader constants for
    // the render method definition that the tag reference at reference (a
    // guest address) names; 0 if the shader's address is 0. nullopt where
    // the host copy can't say: the original checks nothing (NONE datums
    // read slot 0xFFFF, indexes aren't range checked).
    std::optional<uint32_t> VertexShaderConstants(uint32_t reference, uint32_t vertexType)
    {
        uint32_t datum = h3::GuestRef<uint32_t>(reference + kTagReferenceDatum);
        if (datum == kNone)
            return std::nullopt;
        const auto* definition = h3::native::tags::TagBySlotOf<host::render_method_definition_tag>(datum & 0xFFFF);
        if (!definition)
            return std::nullopt;
        uint32_t shared = definition->render_method_definition.shared_vertex_shaders.datum;
        if (shared == kNone)
            return std::nullopt;
        const auto* shaders = h3::native::tags::TagBySlotOf<host::global_vertex_shader_tag>(shared & 0xFFFF);
        if (!shaders)
            return std::nullopt;
        const auto& types = shaders->global_vertex_shader.vertex_types;
        int32_t type = int32_t(vertexType);
        if (type < 0 || type >= types.count)
            return std::nullopt;
        const auto& entryPoints = types[type].entry_point_dependency;
        if (entryPoints.count <= 0)
            return std::nullopt;
        uint32_t index = uint32_t(entryPoints[0].default_compiled_shader_index);
        uint32_t shader = shaders->global_vertex_shader.compiled_shaders.address + index * kCompiledVertexShaderBytes;
        return shader ? shader + kCompiledVertexShaderConstants : 0u;
    }

    // The same for one of the renderers' static states: +0 the guest
    // address of the render method definition reference, +44 the vertex type.
    std::optional<uint32_t> StaticStateVertexShaderConstants(uint32_t state)
    {
        constexpr uint32_t kReference = 0, kVertexType = 44;
        return VertexShaderConstants(h3::GuestRef<uint32_t>(state + kReference), h3::GuestRef<uint32_t>(state + kVertexType));
    }
}

// render_method_state_get_vertex_shader_constants(r3 state: +0 guest
// address of a render method definition reference, +80 vertex type) ->
// guest address of the vertex shader constants (above), 0 if none.
H3_REPLACE_INT(824602B8, render_method_state_get_vertex_shader_constants, {
    constexpr uint32_t kReference = 0, kVertexType = 80;
    return VertexShaderConstants(h3::GuestRef<uint32_t>(ctx.r3.u32 + kReference), h3::GuestRef<uint32_t>(ctx.r3.u32 + kVertexType));
})

// The same for three static render states (each read by one renderer
// near the particle code, 822B5D08 / 822B6760 / 822B7608).
H3_REPLACE_INT(824624E8, render_state_827BEA84_get_vertex_shader_constants, {
    return StaticStateVertexShaderConstants(0x827BEA84);
})
H3_REPLACE_INT(82463F88, render_state_827BF4B4_get_vertex_shader_constants, {
    return StaticStateVertexShaderConstants(0x827BF4B4);
})
H3_REPLACE_INT(824654C0, render_state_827BF724_get_vertex_shader_constants, {
    return StaticStateVertexShaderConstants(0x827BF724);
})
#endif
