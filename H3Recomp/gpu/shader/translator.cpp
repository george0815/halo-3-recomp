// Xenos microcode -> GLSL. Instruction semantics follow the notes in Xenia's
// src/xenia/gpu/ucode.h (BSD license); see ucode.h here for the encodings.
//
// Shape of the output:
// - Temporary registers are a vec4 array (relative addressing needs it), all
//   zeroed first. Vertex shaders get the vertex index in r0.x; pixel shaders
//   get the interpolators in r0, r1, ...
// - Control flow becomes a state machine: one switch case per control flow
//   instruction, `pc` holding the next one. Exec clauses expand inline.
// - Multiplications follow the Direct3D 9 rule that 0 * anything (including
//   infinity and NaN) is +0, which games depend on.

#include "gpu/shader/translator.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <set>

namespace h3::gpu::shader
{
    using namespace ucode;

    namespace
    {
        constexpr uint32_t kMaxRegisters = 64;
        constexpr uint32_t kFloatConstantCount = 256;

        const char kSwizzleChars[] = "xyzw";

        std::string Format(const char* format, ...)
        {
            char buffer[1024];
            va_list args;
            va_start(args, format);
            vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            return buffer;
        }

        // Shared by both stages.
        const char* kPrelude = R"(
const float XE_FLT_MAX = 3.402823466e+38;

// Direct3D 9 ("legacy") multiplication: +-0 times anything is +0.
float xe_mul(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
vec4 xe_mul(vec4 a, vec4 b)
{
    bvec4 zero = bvec4(uvec4(equal(a, vec4(0.0))) | uvec4(equal(b, vec4(0.0))));
    return mix(a * b, vec4(0.0), zero);
}
float xe_dot4(vec4 a, vec4 b) { vec4 p = xe_mul(a, b); return ((p.x + p.y) + p.z) + p.w; }
float xe_dot3(vec4 a, vec4 b) { vec4 p = xe_mul(a, b); return (p.x + p.y) + p.z; }
vec4 xe_max(vec4 a, vec4 b) { return mix(b, a, greaterThanEqual(a, b)); }
vec4 xe_min(vec4 a, vec4 b) { return mix(b, a, lessThan(a, b)); }
float xe_clamp_inf(float t, float replacement)
{
    return isinf(t) ? (t < 0.0 ? -replacement : replacement) : t;
}
int xe_address(float value) { return int(clamp(floor(value + 0.5), -256.0, 255.0)); }
int xe_address_floor(float value) { return int(clamp(floor(value), -256.0, 255.0)); }

// Rounds depth to the Xenos 24-bit float (20e4) depth format, as Xenia's
// Float32To20e4 / Float20e4To32 do. Passes that test against depth written
// by another pass (a depth prepass, then GREATER_EQUAL) need both to land on
// exactly the same value, which they do on the console through this format.
float xe_float24_round(float f)
{
    if (!(f > 0.0))
        return 0.0;
    uint u = floatBitsToUint(f);
    uint f24;
    if (u >= 0x3FFFFFF8u)
    {
        f24 = 0xFFFFFFu;
    }
    else
    {
        if (u < 0x38800000u)
            u = (0x800000u | (u & 0x7FFFFFu)) >> min(113u - (u >> 23), 24u);
        else
            u += 0xC8000000u;
        u += 3u + ((u >> 3) & 1u);
        f24 = (u >> 3) & 0xFFFFFFu;
    }
    if (f24 == 0u)
        return 0.0;
    uint mantissa = f24 & 0xFFFFFu;
    int exponent = int(f24 >> 20);
    if (exponent == 0)
    {
        int shift = 20 - findMSB(mantissa);
        exponent = 1 - shift;
        mantissa = (mantissa << uint(shift)) & 0xFFFFFu;
    }
    return uintBitsToFloat((uint(exponent + 112) << 23) | (mantissa << 3));
}

uint xe_swap(uint value, uint mode)
{
    if (mode == 1u)  // 8-in-16
        return ((value & 0x00FF00FFu) << 8) | ((value >> 8) & 0x00FF00FFu);
    if (mode == 2u)  // 8-in-32
        return (value << 24) | ((value & 0xFF00u) << 8) | ((value >> 8) & 0xFF00u) | (value >> 24);
    if (mode == 3u)  // 16-in-32
        return (value << 16) | (value >> 16);
    return value;
}
)";

        // Vertex shader entry point. Normal draws run the guest shader once
        // for the vertex. Rectangle lists (flags bit 0) are drawn as six host
        // vertices per rectangle, non-indexed: corners 0-2 are the guest's
        // three vertices (guest indices at rect_indices in the vertex memory
        // buffer), and the second triangle uses the fourth corner, made from
        // the other three like Xenia's geometry shader: opposite the
        // right-angle corner, every output = a + b - corner.
        const char* kVertexMain = R"(
void xe_write_vertex(vec4 position, vec4 outputs[16])
{
    for (int i = 0; i < 16; i++)
        xe_out[i] = outputs[i];
    // PA_CL_VTE_CNTL formats (Xenia's CompleteVertexOrTessEvalShaderInMain):
    // W may be 1/W, and XY and Z may already be divided by W.
    if ((xe_sys.flags & 16u) == 0u)
        position.w = 1.0 / position.w;
    if ((xe_sys.flags & 4u) != 0u)
        position.xy *= position.w;
    if ((xe_sys.flags & 8u) != 0u)
        position.z *= position.w;
    gl_Position = vec4(position.xyz * xe_sys.ndc_scale.xyz + position.w * xe_sys.ndc_offset.xyz, position.w);
}

void main()
{
    vec4 position;
    vec4 outputs[16];
    if ((xe_sys.flags & 1u) == 0u)
    {
        xe_vertex_shader(gl_VertexIndex, position, outputs);
        xe_write_vertex(position, outputs);
        return;
    }

    int rect = gl_VertexIndex / 6;
    int corner = gl_VertexIndex % 6;
    uint base = xe_sys.rect_indices + uint(rect) * 3u;
    if (corner < 3)
    {
        xe_vertex_shader(int(xe_memory[base + uint(corner)]), position, outputs);
        xe_write_vertex(position, outputs);
        return;
    }

    vec4 p[3];
    vec4 o[3][16];
    for (int i = 0; i < 3; i++)
        xe_vertex_shader(int(xe_memory[base + uint(i)]), p[i], o[i]);
    // The right angle is opposite the longest edge.
    vec2 s0 = p[0].xy / p[0].w, s1 = p[1].xy / p[1].w, s2 = p[2].xy / p[2].w;
    float d01 = dot(s0 - s1, s0 - s1), d02 = dot(s0 - s2, s0 - s2), d12 = dot(s1 - s2, s1 - s2);
    int r = (d12 >= d01 && d12 >= d02) ? 0 : (d02 >= d01 ? 1 : 2);
    int a = r == 0 ? 1 : 0;
    int b = r == 2 ? 1 : 2;
    if (corner == 3)
    {
        xe_write_vertex(p[a], o[a]);
        return;
    }
    if (corner == 4)
    {
        xe_write_vertex(p[b], o[b]);
        return;
    }
    position = p[a] + p[b] - p[r];
    for (int i = 0; i < 16; i++)
        outputs[i] = o[a][i] + o[b][i] - o[r][i];
    xe_write_vertex(position, outputs);
}
)";

        struct SourceOperand
        {
            std::string base;  // the vec4 expression before swizzling
            bool negate = false;
            bool absolute = false;
            uint32_t swizzle = 0;
        };

        class Translator
        {
        public:
            Translator(bool isVertex, const uint32_t* dwords, uint32_t count)
                : m_vertex(isVertex), m_dwords(dwords), m_count(count)
            {
                m_info.isVertex = isVertex;
            }

            TranslationResult Run()
            {
                TranslationResult result;
                if (m_count < 3)
                {
                    result.error = "shader too small";
                    return result;
                }
                ScanControlFlow();
                ScanFetches();
                EmitBody();
                if (!m_error.empty())
                {
                    result.error = m_error;
                    return result;
                }
                result.glsl = EmitShader();
                result.info = m_info;
                result.ok = true;
                return result;
            }

            std::string Disassemble()
            {
                ScanControlFlow();
                std::string out;
                for (uint32_t i = 0; i < m_cfCount; i++)
                {
                    const CfInstruction& cf = m_cf[i];
                    out += Format("%3u: cf op %2u", i, uint32_t(cf.Opcode()));
                    if (cf.IsExec())
                    {
                        out += Format(" exec addr %u count %u seq %03X bool %u cond %u%s\n", cf.ExecAddress(),
                            cf.ExecCount(), cf.ExecSequence(), cf.BoolAddress(), cf.Condition(),
                            cf.EndsShader() ? " END" : "");
                        for (uint32_t j = 0; j < cf.ExecCount(); j++)
                        {
                            uint32_t slot = cf.ExecAddress() + j;
                            bool fetch = (cf.ExecSequence() >> (2 * j)) & 1;
                            const uint32_t* d = m_dwords + slot * 3;
                            if (fetch)
                            {
                                FetchInstruction f{ { d[0], d[1], d[2] } };
                                out += Format("       %s fetch op %u r%u <- r%u const %u\n",
                                    f.Opcode() == FetchOpcode::VertexFetch ? "vertex" : "texture",
                                    uint32_t(f.Opcode()), f.DstReg(), f.SrcReg(),
                                    f.Opcode() == FetchOpcode::VertexFetch ? f.VertexConstIndex() : f.TextureConstIndex());
                            }
                            else
                            {
                                AluInstruction a{ { d[0], d[1], d[2] } };
                                out += Format("       alu vec %2u -> %s%u mask %X | scl %2u -> %u mask %X\n",
                                    uint32_t(a.VectorOp()), a.IsExport() ? "export " : "r", a.VectorDest(),
                                    a.VectorWriteMask(), uint32_t(a.ScalarOp()), a.ScalarDest(), a.ScalarWriteMask());
                            }
                        }
                    }
                    else
                    {
                        out += Format(" target %u loop %u\n", cf.TargetAddress(), cf.LoopId());
                    }
                }
                return out;
            }

        private:
            bool m_vertex;
            const uint32_t* m_dwords;
            uint32_t m_count;
            ShaderInfo m_info;
            std::string m_error;

            std::vector<CfInstruction> m_cf;
            uint32_t m_cfCount = 0;

            std::string m_body;
            std::set<uint32_t> m_vertexFetchSet;
            bool m_usesPosition = false;
            int m_indent = 0;

            // vfetch_mini reuses the address of the previous vfetch_full.
            FetchInstruction m_previousFullFetch;

            void Fail(const std::string& message)
            {
                if (m_error.empty())
                    m_error = message;
            }

            void Line(const std::string& text)
            {
                m_body.append(size_t(m_indent) * 4, ' ');
                m_body += text;
                m_body += '\n';
            }

            // Control flow comes first; its length is the lowest exec address
            // (in three-dword pairs), as Xenia and freedreno determine it.
            void ScanControlFlow()
            {
                uint32_t pairBound = m_count / 3;
                for (uint32_t pair = 0; pair < pairBound; pair++)
                {
                    CfInstruction cf[2];
                    UnpackCfPair(m_dwords, pair, cf);
                    for (const CfInstruction& instruction : cf)
                        if (instruction.IsExec())
                            pairBound = std::min(pairBound, instruction.ExecAddress());
                }
                m_cf.clear();
                for (uint32_t pair = 0; pair < pairBound; pair++)
                {
                    CfInstruction cf[2];
                    UnpackCfPair(m_dwords, pair, cf);
                    m_cf.push_back(cf[0]);
                    m_cf.push_back(cf[1]);
                }
                m_cfCount = uint32_t(m_cf.size());
            }

            // Texture bindings and vertex fetch constants are needed before the
            // body is emitted (declarations come first), so collect them in a
            // separate pass over all exec clauses.
            void ScanFetches()
            {
                for (const CfInstruction& cf : m_cf)
                {
                    if (!cf.IsExec())
                        continue;
                    for (uint32_t j = 0; j < cf.ExecCount(); j++)
                    {
                        if (!((cf.ExecSequence() >> (2 * j)) & 1))
                            continue;
                        uint32_t slot = cf.ExecAddress() + j;
                        if ((slot + 1) * 3 > m_count)
                            continue;
                        const uint32_t* d = m_dwords + slot * 3;
                        FetchInstruction f{ { d[0], d[1], d[2] } };
                        if (f.Opcode() == FetchOpcode::VertexFetch)
                        {
                            if (!f.VertexIsMini())
                                m_vertexFetchSet.insert(f.VertexConstIndex());
                        }
                        else if (f.Opcode() == FetchOpcode::TextureFetch ||
                            f.Opcode() == FetchOpcode::GetTextureWeights ||
                            f.Opcode() == FetchOpcode::GetTextureComputedLod)
                        {
                            TextureBindingFor(f.TextureConstIndex(), f.TextureDim());
                        }
                    }
                }
                m_info.vertexFetchConstants.assign(m_vertexFetchSet.begin(), m_vertexFetchSet.end());
                m_info.storageBufferBinding = uint32_t(m_info.textures.size());
            }

            uint32_t TextureBindingFor(uint32_t fetchConstant, TextureDimension dimension)
            {
                for (const TextureBinding& binding : m_info.textures)
                    if (binding.fetchConstant == fetchConstant && binding.dimension == dimension)
                        return binding.binding;
                uint32_t binding = uint32_t(m_info.textures.size());
                m_info.textures.push_back({ fetchConstant, dimension, binding });
                return binding;
            }

            // --- Body ----------------------------------------------------

            std::string BoolConstant(uint32_t index)
            {
                return Format("((xe_sys.bools[%u][%u] & %uu) != 0u)", index >> 7, (index >> 5) & 3, 1u << (index & 31));
            }

            void EmitBody()
            {
                m_indent = 2;
                for (uint32_t i = 0; i < m_cfCount && m_error.empty(); i++)
                {
                    const CfInstruction& cf = m_cf[i];
                    m_indent = 2;
                    Line(Format("case %u:", i));
                    m_indent = 3;
                    EmitControlFlow(i, cf);
                }
            }

            void EmitControlFlow(uint32_t index, const CfInstruction& cf)
            {
                uint32_t next = index + 1;
                switch (cf.Opcode())
                {
                case CfOpcode::Nop:
                case CfOpcode::Alloc:
                case CfOpcode::MarkVsFetchDone:
                    Line(Format("pc = %u; break;", next));
                    return;

                case CfOpcode::Exec:
                case CfOpcode::ExecEnd:
                    EmitExec(cf);
                    Line(cf.EndsShader() ? "running = false; break;" : Format("pc = %u; break;", next));
                    return;

                case CfOpcode::CondExec:
                case CfOpcode::CondExecEnd:
                case CfOpcode::CondExecPredClean:
                case CfOpcode::CondExecPredCleanEnd:
                case CfOpcode::CondExecPred:
                case CfOpcode::CondExecPredEnd:
                {
                    bool onPredicate = cf.Opcode() == CfOpcode::CondExecPred || cf.Opcode() == CfOpcode::CondExecPredEnd;
                    std::string condition = onPredicate ? Format("(p0 == %s)", cf.Condition() ? "true" : "false")
                                                        : Format("(%s == %s)", BoolConstant(cf.BoolAddress()).c_str(),
                                                              cf.Condition() ? "true" : "false");
                    Line(Format("if %s", condition.c_str()));
                    Line("{");
                    m_indent++;
                    EmitExec(cf);
                    // Ending execs end the shader only when their condition holds.
                    if (cf.EndsShader())
                        Line("running = false; break;");
                    m_indent--;
                    Line("}");
                    Line(Format("pc = %u; break;", next));
                    return;
                }

                case CfOpcode::LoopStart:
                    Line(Format("loopDepth++; loopSavedAL[loopDepth] = aL; loopIteration[loopDepth] = 0;"));
                    Line(Format("loopConstant[loopDepth] = xe_sys.loops[%u][%u];", cf.LoopId() >> 2, cf.LoopId() & 3));
                    if (!cf.LoopIsRepeat())
                        Line("aL = int((loopConstant[loopDepth] >> 8) & 0xFFu);");
                    Line(Format("if ((loopConstant[loopDepth] & 0xFFu) == 0u) { aL = loopSavedAL[loopDepth]; loopDepth--; pc = %u; break; }",
                        cf.TargetAddress()));
                    Line(Format("pc = %u; break;", next));
                    return;

                case CfOpcode::LoopEnd:
                {
                    Line("loopIteration[loopDepth]++;");
                    Line("aL += int(bitfieldExtract(int(loopConstant[loopDepth]), 16, 8));");
                    std::string continueCondition = "uint(loopIteration[loopDepth]) < (loopConstant[loopDepth] & 0xFFu)";
                    if (cf.LoopIsPredicatedBreak())
                        continueCondition += Format(" && !(p0 == %s)", cf.Condition() ? "true" : "false");
                    Line(Format("if (%s) { pc = %u; break; }", continueCondition.c_str(), cf.TargetAddress()));
                    Line(Format("aL = loopSavedAL[loopDepth]; loopDepth--; pc = %u; break;", next));
                    return;
                }

                case CfOpcode::CondCall:
                case CfOpcode::CondJmp:
                {
                    std::string condition;
                    if (cf.IsUnconditional())
                        condition = "true";
                    else if (cf.IsPredicated())
                        condition = Format("(p0 == %s)", cf.Condition() ? "true" : "false");
                    else
                        condition = Format("(%s == %s)", BoolConstant(cf.BoolAddress()).c_str(), cf.Condition() ? "true" : "false");
                    if (cf.Opcode() == CfOpcode::CondCall)
                        Line(Format("if (%s && callDepth < 3) { callDepth++; callStack[callDepth] = %u; pc = %u; break; }",
                            condition.c_str(), next, cf.TargetAddress()));
                    else
                        Line(Format("if (%s) { pc = %u; break; }", condition.c_str(), cf.TargetAddress()));
                    Line(Format("pc = %u; break;", next));
                    return;
                }

                case CfOpcode::Return:
                    Line(Format("if (callDepth >= 0) { pc = callStack[callDepth]; callDepth--; break; }"));
                    Line(Format("pc = %u; break;", next));
                    return;
                }
                Fail(Format("unknown control flow opcode %u", uint32_t(cf.Opcode())));
            }

            void EmitExec(const CfInstruction& cf)
            {
                for (uint32_t j = 0; j < cf.ExecCount(); j++)
                {
                    uint32_t slot = cf.ExecAddress() + j;
                    if ((slot + 1) * 3 > m_count)
                    {
                        Fail(Format("instruction slot %u out of range", slot));
                        return;
                    }
                    const uint32_t* d = m_dwords + slot * 3;
                    bool fetch = (cf.ExecSequence() >> (2 * j)) & 1;
                    if (fetch)
                        EmitFetch(FetchInstruction{ { d[0], d[1], d[2] } });
                    else
                        EmitAlu(AluInstruction{ { d[0], d[1], d[2] } });
                }
            }

            // --- ALU -----------------------------------------------------

            std::string TempRegister(uint32_t index, bool relative)
            {
                if (relative)
                    return Format("r[clamp(%u + aL, 0, %u)]", index, kMaxRegisters - 1);
                return Format("r[%u]", index & (kMaxRegisters - 1));
            }

            SourceOperand AluSource(const AluInstruction& alu, uint32_t i)
            {
                SourceOperand op;
                uint32_t reg = alu.SrcReg(i);
                op.negate = alu.SrcNegate(i);
                op.swizzle = alu.SrcSwizzle(i);
                if (alu.SrcIsTemp(i))
                {
                    op.base = TempRegister(reg & 0x3F, (reg & 0x40) != 0);
                    op.absolute = (reg & 0x80) != 0;
                }
                else
                {
                    if (alu.SrcConstAddressed(i))
                        op.base = Format("c[clamp(%u + %s, 0, %u)]", reg, alu.ConstAddressRegisterRelative() ? "a0" : "aL",
                            kFloatConstantCount - 1);
                    else
                        op.base = Format("c[%u]", reg);
                    op.absolute = alu.AbsConstants();
                }
                return op;
            }

            static std::string Apply(const SourceOperand& op, const std::string& swizzled)
            {
                std::string value = swizzled;
                if (op.absolute)
                    value = "abs(" + value + ")";
                if (op.negate)
                    value = "-" + value;
                return value;
            }

            static std::string Vector(const SourceOperand& op)
            {
                std::string swizzle = ".";
                for (uint32_t c = 0; c < 4; c++)
                    swizzle += kSwizzleChars[SwizzledComponent(op.swizzle, c)];
                return Apply(op, op.base + swizzle);
            }

            static std::string Component(const SourceOperand& op, uint32_t component)
            {
                return Apply(op, op.base + "." + kSwizzleChars[SwizzledComponent(op.swizzle, component)]);
            }

            std::string VectorOperation(const AluInstruction& alu, const std::string s[3], bool& writesA0,
                bool& writesPredicate, bool& kills)
            {
                const std::string& a = s[0];
                const std::string& b = s[1];
                const std::string& c = s[2];
                switch (alu.VectorOp())
                {
                case VectorOpcode::Add: return a + " + " + b;
                case VectorOpcode::Mul: return "xe_mul(" + a + ", " + b + ")";
                case VectorOpcode::Max: return "xe_max(" + a + ", " + b + ")";
                case VectorOpcode::Min: return "xe_min(" + a + ", " + b + ")";
                case VectorOpcode::Seq: return "vec4(equal(" + a + ", " + b + "))";
                case VectorOpcode::Sgt: return "vec4(greaterThan(" + a + ", " + b + "))";
                case VectorOpcode::Sge: return "vec4(greaterThanEqual(" + a + ", " + b + "))";
                case VectorOpcode::Sne: return "vec4(notEqual(" + a + ", " + b + "))";
                case VectorOpcode::Frc: return a + " - floor(" + a + ")";
                case VectorOpcode::Trunc: return "trunc(" + a + ")";
                case VectorOpcode::Floor: return "floor(" + a + ")";
                case VectorOpcode::Mad: return "xe_mul(" + a + ", " + b + ") + " + c;
                case VectorOpcode::CndEq: return "mix(" + c + ", " + b + ", equal(" + a + ", vec4(0.0)))";
                case VectorOpcode::CndGe: return "mix(" + c + ", " + b + ", greaterThanEqual(" + a + ", vec4(0.0)))";
                case VectorOpcode::CndGt: return "mix(" + c + ", " + b + ", greaterThan(" + a + ", vec4(0.0)))";
                case VectorOpcode::Dp4: return "vec4(xe_dot4(" + a + ", " + b + "))";
                case VectorOpcode::Dp3: return "vec4(xe_dot3(" + a + ", " + b + "))";
                case VectorOpcode::Dp2Add:
                    return "vec4(xe_mul(" + a + ".x, " + b + ".x) + xe_mul(" + a + ".y, " + b + ".y) + " + c + ".x)";
                case VectorOpcode::Cube: return "xe_cube(" + a + ")";
                case VectorOpcode::Max4: return "xe_max4(" + a + ")";
                case VectorOpcode::SetpEqPush:
                case VectorOpcode::SetpNePush:
                case VectorOpcode::SetpGtPush:
                case VectorOpcode::SetpGePush:
                {
                    static const char* kCompare[] = { "==", "!=", ">", ">=" };
                    const char* compare = kCompare[uint32_t(alu.VectorOp()) - uint32_t(VectorOpcode::SetpEqPush)];
                    writesPredicate = true;
                    Line(Format("newP0 = (%s.w == 0.0 && %s.w %s 0.0);", a.c_str(), b.c_str(), compare));
                    return Format("vec4((%s.x == 0.0 && %s.x %s 0.0) ? 0.0 : %s.x + 1.0)", a.c_str(), b.c_str(), compare, a.c_str());
                }
                case VectorOpcode::KillEq:
                case VectorOpcode::KillGt:
                case VectorOpcode::KillGe:
                case VectorOpcode::KillNe:
                {
                    static const char* kCompare[] = { "equal", "greaterThan", "greaterThanEqual", "notEqual" };
                    const char* compare = kCompare[uint32_t(alu.VectorOp()) - uint32_t(VectorOpcode::KillEq)];
                    kills = true;
                    Line(Format("kill = kill || any(%s(%s, %s));", compare, a.c_str(), b.c_str()));
                    return Format("vec4(any(%s(%s, %s)) ? 1.0 : 0.0)", compare, a.c_str(), b.c_str());
                }
                case VectorOpcode::Dst:
                    return "vec4(1.0, xe_mul(" + a + ".y, " + b + ".y), " + a + ".z, " + b + ".w)";
                case VectorOpcode::MaxA:
                    writesA0 = true;
                    Line("newA0 = xe_address(" + a + ".w);");
                    return "xe_max(" + a + ", " + b + ")";
                }
                Fail(Format("unknown vector opcode %u", uint32_t(alu.VectorOp())));
                return "vec4(0.0)";
            }

            std::string ScalarOperation(const AluInstruction& alu, const std::string& a, const std::string& b,
                bool& writesA0, bool& writesPredicate, bool& kills)
            {
                auto setp = [&](const std::string& condition, const std::string& otherwise) {
                    writesPredicate = true;
                    Line("newP0 = " + condition + ";");
                    return "(" + condition + " ? 0.0 : " + otherwise + ")";
                };
                auto kill = [&](const std::string& condition) {
                    kills = true;
                    Line("kill = kill || " + condition + ";");
                    return "(" + condition + " ? 1.0 : 0.0)";
                };
                switch (alu.ScalarOp())
                {
                case ScalarOpcode::Adds: return a + " + " + b;
                case ScalarOpcode::AddsPrev: return a + " + ps";
                case ScalarOpcode::Muls: return "xe_mul(" + a + ", " + b + ")";
                case ScalarOpcode::MulsPrev: return "xe_mul(" + a + ", ps)";
                case ScalarOpcode::MulsPrev2:
                    return "((ps == -XE_FLT_MAX || isinf(ps) || isnan(ps) || isinf(" + b + ") || isnan(" + b + ") || " + b +
                        " <= 0.0) ? -XE_FLT_MAX : xe_mul(" + a + ", ps))";
                case ScalarOpcode::Maxs: return "(" + a + " >= " + b + " ? " + a + " : " + b + ")";
                case ScalarOpcode::Mins: return "(" + a + " < " + b + " ? " + a + " : " + b + ")";
                case ScalarOpcode::Seqs: return "(" + a + " == 0.0 ? 1.0 : 0.0)";
                case ScalarOpcode::Sgts: return "(" + a + " > 0.0 ? 1.0 : 0.0)";
                case ScalarOpcode::Sges: return "(" + a + " >= 0.0 ? 1.0 : 0.0)";
                case ScalarOpcode::Snes: return "(" + a + " != 0.0 ? 1.0 : 0.0)";
                case ScalarOpcode::Frcs: return a + " - floor(" + a + ")";
                case ScalarOpcode::Truncs: return "trunc(" + a + ")";
                case ScalarOpcode::Floors: return "floor(" + a + ")";
                case ScalarOpcode::Exp: return "(" + a + " == 0.0 ? 1.0 : exp2(" + a + "))";
                case ScalarOpcode::Logc:
                    return "xe_clamp_inf(" + a + " == 1.0 ? 0.0 : log2(" + a + "), XE_FLT_MAX)";
                case ScalarOpcode::Log: return "(" + a + " == 1.0 ? 0.0 : log2(" + a + "))";
                case ScalarOpcode::Rcpc: return "xe_clamp_inf(" + a + " == 1.0 ? 1.0 : 1.0 / " + a + ", XE_FLT_MAX)";
                case ScalarOpcode::Rcpf: return "xe_clamp_inf(" + a + " == 1.0 ? 1.0 : 1.0 / " + a + ", 0.0)";
                case ScalarOpcode::Rcp: return "(" + a + " == 1.0 ? 1.0 : 1.0 / " + a + ")";
                case ScalarOpcode::Rsqc:
                    return "xe_clamp_inf(" + a + " == 1.0 ? 1.0 : inversesqrt(" + a + "), XE_FLT_MAX)";
                case ScalarOpcode::Rsqf: return "xe_clamp_inf(" + a + " == 1.0 ? 1.0 : inversesqrt(" + a + "), 0.0)";
                case ScalarOpcode::Rsq: return "(" + a + " == 1.0 ? 1.0 : inversesqrt(" + a + "))";
                case ScalarOpcode::MaxAs:
                    writesA0 = true;
                    Line("newA0 = xe_address(" + a + ");");
                    return "(" + a + " >= " + b + " ? " + a + " : " + b + ")";
                case ScalarOpcode::MaxAsf:
                    writesA0 = true;
                    Line("newA0 = xe_address_floor(" + a + ");");
                    return "(" + a + " >= " + b + " ? " + a + " : " + b + ")";
                case ScalarOpcode::Subs: return a + " - " + b;
                case ScalarOpcode::SubsPrev: return a + " - ps";
                case ScalarOpcode::SetpEq: return setp("(" + a + " == 0.0)", "1.0");
                case ScalarOpcode::SetpNe: return setp("(" + a + " != 0.0)", "1.0");
                case ScalarOpcode::SetpGt: return setp("(" + a + " > 0.0)", "1.0");
                case ScalarOpcode::SetpGe: return setp("(" + a + " >= 0.0)", "1.0");
                case ScalarOpcode::SetpInv:
                    return setp("(" + a + " == 1.0)", "(" + a + " == 0.0 ? 1.0 : " + a + ")");
                case ScalarOpcode::SetpPop: return setp("(" + a + " - 1.0 <= 0.0)", "(" + a + " - 1.0)");
                case ScalarOpcode::SetpClr:
                    writesPredicate = true;
                    Line("newP0 = false;");
                    return "XE_FLT_MAX";
                case ScalarOpcode::SetpRstr: return setp("(" + a + " == 0.0)", a);
                case ScalarOpcode::KillsEq: return kill("(" + a + " == 0.0)");
                case ScalarOpcode::KillsGt: return kill("(" + a + " > 0.0)");
                case ScalarOpcode::KillsGe: return kill("(" + a + " >= 0.0)");
                case ScalarOpcode::KillsNe: return kill("(" + a + " != 0.0)");
                case ScalarOpcode::KillsOne: return kill("(" + a + " == 1.0)");
                case ScalarOpcode::Sqrt: return "sqrt(" + a + ")";
                case ScalarOpcode::Mulsc0:
                case ScalarOpcode::Mulsc1: return "xe_mul(" + a + ", " + b + ")";
                case ScalarOpcode::Addsc0:
                case ScalarOpcode::Addsc1: return a + " + " + b;
                case ScalarOpcode::Subsc0:
                case ScalarOpcode::Subsc1: return a + " - " + b;
                case ScalarOpcode::Sin: return "sin(" + a + ")";
                case ScalarOpcode::Cos: return "cos(" + a + ")";
                case ScalarOpcode::RetainPrev: return "ps";
                }
                Fail(Format("unknown scalar opcode %u", uint32_t(alu.ScalarOp())));
                return "0.0";
            }

            // Where an export register goes; empty if it's dropped.
            std::string ExportTarget(uint32_t index)
            {
                if (m_vertex)
                {
                    if (index < 16)
                    {
                        m_info.interpolatorMask |= 1u << index;
                        return Format("xe_outputs[%u]", index);
                    }
                    if (index == kExportVsPosition)
                    {
                        m_usesPosition = true;
                        return "xe_position";
                    }
                    return "";  // point size / edge flag / kill vertex, memory export
                }
                if (index < 4)
                {
                    m_info.colorTargetMask |= 1u << index;
                    return Format("xe_color%u", index);
                }
                if (index == kExportPsDepth)
                {
                    m_info.writesDepth = true;
                    return "xe_depth";
                }
                return "";
            }

            void EmitAlu(const AluInstruction& alu)
            {
                bool predicated = alu.IsPredicated();
                if (predicated)
                {
                    Line(Format("if (p0 == %s)", alu.PredicateCondition() ? "true" : "false"));
                }
                Line("{");
                m_indent++;

                // Read every operand before anything is written: the vector and
                // scalar operations are co-issued.
                uint32_t vectorOperands = VectorOperandCount(alu.VectorOp());
                std::string vectorSources[3];
                for (uint32_t i = 0; i < vectorOperands; i++)
                {
                    Line(Format("vec4 s%u = %s;", i, Vector(AluSource(alu, i + 1)).c_str()));
                    vectorSources[i] = Format("s%u", i);
                }

                std::string scalarA = "0.0", scalarB = "0.0";
                uint32_t scalarOperands = ScalarOperandCount(alu.ScalarOp());
                if (scalarOperands == 1)
                {
                    SourceOperand op = AluSource(alu, 3);
                    Line("float sa = " + Component(op, 3) + ";");
                    scalarA = "sa";
                    if (ScalarIsTwoComponent(alu.ScalarOp()))
                    {
                        Line("float sb = " + Component(op, vectorOperands == 3 ? 2 : 0) + ";");
                        scalarB = "sb";
                    }
                }
                else if (scalarOperands == 2)
                {
                    // Constant (W) and temporary register (X), both taking the
                    // third source's negation and abs_constants.
                    SourceOperand constant = AluSource(alu, 3);
                    constant.base = alu.SrcConstAddressed(3)
                        ? Format("c[clamp(%u + %s, 0, %u)]", alu.SrcReg(3), alu.ConstAddressRegisterRelative() ? "a0" : "aL",
                              kFloatConstantCount - 1)
                        : Format("c[%u]", alu.SrcReg(3));
                    constant.absolute = alu.AbsConstants();
                    SourceOperand temp = constant;
                    temp.base = TempRegister(alu.ScalarConstOpTempReg(), false);
                    Line("float sa = " + Component(constant, 3) + ";");
                    Line("float sb = " + Component(temp, 0) + ";");
                    scalarA = "sa";
                    scalarB = "sb";
                }

                bool writesA0 = false, writesPredicate = false, kills = false;
                std::string vector = VectorOperation(alu, vectorSources, writesA0, writesPredicate, kills);
                Line("vec4 v = " + vector + ";");
                std::string scalar = ScalarOperation(alu, scalarA, scalarB, writesA0, writesPredicate, kills);
                Line("float s = " + scalar + ";");
                if (alu.ScalarOp() != ScalarOpcode::RetainPrev)
                    Line("ps = s;");
                if (alu.VectorClamp())
                    Line("v = clamp(v, 0.0, 1.0);");
                if (alu.ScalarClamp())
                    Line("s = clamp(s, 0.0, 1.0);");

                // Writes.
                if (alu.IsExport())
                {
                    std::string target = ExportTarget(alu.VectorDest());
                    if (!target.empty())
                    {
                        uint32_t vectorMask = alu.VectorResultMask();
                        uint32_t scalarMask = alu.ScalarResultMask();
                        uint32_t zeroMask = alu.Constant0Mask();
                        uint32_t oneMask = alu.Constant1Mask();
                        for (uint32_t c = 0; c < 4; c++)
                        {
                            const char* value = nullptr;
                            std::string component = Format("v.%c", kSwizzleChars[c]);
                            if (vectorMask & (1u << c))
                                value = component.c_str();
                            else if (scalarMask & (1u << c))
                                value = "s";
                            else if (zeroMask & (1u << c))
                                value = "0.0";
                            else if (oneMask & (1u << c))
                                value = "1.0";
                            if (value)
                                Line(Format("%s.%c = %s;", target.c_str(), kSwizzleChars[c], value));
                        }
                    }
                }
                else
                {
                    uint32_t vectorMask = alu.VectorResultMask();
                    if (vectorMask)
                    {
                        std::string reg = TempRegister(alu.VectorDest(), alu.VectorDestRelative());
                        std::string mask;
                        for (uint32_t c = 0; c < 4; c++)
                            if (vectorMask & (1u << c))
                                mask += kSwizzleChars[c];
                        Line(Format("%s.%s = v.%s;", reg.c_str(), mask.c_str(), mask.c_str()));
                    }
                    uint32_t scalarMask = alu.ScalarResultMask();
                    if (scalarMask)
                    {
                        std::string reg = TempRegister(alu.ScalarDest(), alu.ScalarDestRelative());
                        std::string mask;
                        for (uint32_t c = 0; c < 4; c++)
                            if (scalarMask & (1u << c))
                                mask += kSwizzleChars[c];
                        Line(Format("%s.%s = vec4(s).%s;", reg.c_str(), mask.c_str(), mask.c_str()));
                    }
                }
                if (writesA0)
                    Line("a0 = newA0;");
                if (writesPredicate)
                    Line("p0 = newP0;");
                if (kills)
                {
                    m_info.usesKill = true;
                    if (!m_vertex)
                        Line("if (kill) discard;");
                }

                m_indent--;
                Line("}");
            }

            // --- Fetch ---------------------------------------------------

            static std::string DestinationWrites(const FetchInstruction& f, const std::string& value)
            {
                // 3 bits per destination component: 0-3 = fetched component,
                // 4 = 0, 5 = 1, 6/7 = keep.
                std::string out;
                for (uint32_t c = 0; c < 4; c++)
                {
                    uint32_t select = (f.DstSwizzle() >> (3 * c)) & 7;
                    if (select >= 6)
                        continue;
                    std::string source = select < 4 ? value + "." + kSwizzleChars[select] : (select == 4 ? "0.0" : "1.0");
                    if (!out.empty())
                        out += ' ';
                    out += Format("dst.%c = %s;", kSwizzleChars[c], source.c_str());
                }
                return out;
            }

            void EmitFetch(const FetchInstruction& f)
            {
                if (f.IsPredicated())
                    Line(Format("if (p0 == %s)", f.PredicateCondition() ? "true" : "false"));
                Line("{");
                m_indent++;
                if (f.Opcode() == FetchOpcode::VertexFetch)
                    EmitVertexFetch(f);
                else
                    EmitTextureFetch(f);
                m_indent--;
                Line("}");
            }

            void EmitVertexFetch(const FetchInstruction& f)
            {
                if (!f.VertexIsMini())
                    m_previousFullFetch = f;
                const FetchInstruction& full = m_previousFullFetch;
                if (!m_vertex)
                {
                    Fail("vertex fetch in a pixel shader");
                    return;
                }

                uint32_t constant = full.VertexConstIndex();
                std::string index = Format("%s.%c", TempRegister(full.SrcReg(), full.SrcRelative()).c_str(),
                    kSwizzleChars[full.VertexSrcSwizzle()]);
                Line(Format("int index = int(%s(%s));", full.VertexIndexRounded() ? "xe_round_index" : "floor", index.c_str()));
                Line(Format("int address = index * %u + %d;", full.VertexStride(), f.VertexOffset()));

                // Component values as written, unconverted.
                std::string raw;
                uint32_t components = 0;
                bool packed = false;
                int widths[4] = {}, offsets[4] = {};
                switch (f.VertexDataFormat())
                {
                case VertexFormat::F8_8_8_8:
                    packed = true, components = 4;
                    widths[0] = widths[1] = widths[2] = widths[3] = 8;
                    offsets[1] = 8, offsets[2] = 16, offsets[3] = 24;
                    break;
                case VertexFormat::F2_10_10_10:
                    packed = true, components = 4;
                    widths[0] = widths[1] = widths[2] = 10, widths[3] = 2;
                    offsets[1] = 10, offsets[2] = 20, offsets[3] = 30;
                    break;
                case VertexFormat::F10_11_11:
                    packed = true, components = 3;
                    widths[0] = widths[1] = 11, widths[2] = 10;
                    offsets[1] = 11, offsets[2] = 22;
                    break;
                case VertexFormat::F11_11_10:
                    packed = true, components = 3;
                    widths[0] = 10, widths[1] = widths[2] = 11;
                    offsets[1] = 10, offsets[2] = 21;
                    break;
                case VertexFormat::F16_16:
                    packed = true, components = 2;
                    widths[0] = widths[1] = 16;
                    offsets[1] = 16;
                    break;
                case VertexFormat::F16_16_16_16:
                    packed = true, components = 4;
                    widths[0] = widths[1] = widths[2] = widths[3] = 16;
                    offsets[1] = offsets[3] = 16;
                    break;
                case VertexFormat::F16_16_Float:
                    Line(Format("vec4 fetched = vec4(unpackHalf2x16(xe_vfetch(%u, address)), 0.0, 1.0);", constant));
                    components = 2;
                    break;
                case VertexFormat::F16_16_16_16_Float:
                    Line(Format("vec4 fetched = vec4(unpackHalf2x16(xe_vfetch(%u, address)), unpackHalf2x16(xe_vfetch(%u, address + 1)));",
                        constant, constant));
                    components = 4;
                    break;
                case VertexFormat::F32:
                case VertexFormat::F32_32:
                case VertexFormat::F32_32_32_32:
                case VertexFormat::F32_Float:
                case VertexFormat::F32_32_Float:
                case VertexFormat::F32_32_32_Float:
                case VertexFormat::F32_32_32_32_Float:
                {
                    VertexFormat format = f.VertexDataFormat();
                    components = (format == VertexFormat::F32 || format == VertexFormat::F32_Float) ? 1
                        : (format == VertexFormat::F32_32 || format == VertexFormat::F32_32_Float) ? 2
                        : format == VertexFormat::F32_32_32_Float ? 3 : 4;
                    bool isFloat = format == VertexFormat::F32_Float || format == VertexFormat::F32_32_Float ||
                        format == VertexFormat::F32_32_32_Float || format == VertexFormat::F32_32_32_32_Float;
                    std::string words[4] = { "0u", "0u", "0u", "0u" };
                    for (uint32_t c = 0; c < components; c++)
                        words[c] = Format("xe_vfetch(%u, address + %u)", constant, c);
                    if (isFloat)
                    {
                        Line(Format("vec4 fetched = vec4(uintBitsToFloat(%s), uintBitsToFloat(%s), uintBitsToFloat(%s), %s);",
                            words[0].c_str(), words[1].c_str(), words[2].c_str(),
                            components == 4 ? ("uintBitsToFloat(" + words[3] + ")").c_str() : "1.0"));
                    }
                    else
                    {
                        // 32-bit integers, optionally normalized.
                        const char* convert = f.VertexIsSigned() ? "vec4(ivec4(" : "vec4(uvec4(";
                        Line(Format("vec4 fetched = %s%s, %s, %s, %s));", convert, words[0].c_str(), words[1].c_str(),
                            words[2].c_str(), words[3].c_str()));
                        if (!f.VertexIsInteger())
                        {
                            if (!f.VertexIsSigned())
                                Line("fetched *= 1.0 / 4294967295.0;");
                            else if (f.VertexSignedRfZeroClamp())
                                Line("fetched *= 1.0 / 2147483647.0;");
                            else
                                Line("fetched = fetched * (1.0 / 2147483647.5) + (0.5 / 2147483647.5);");
                        }
                        if (components < 4)
                            Line(Format("fetched.w = 1.0;"));
                    }
                    break;
                }
                default:
                    Fail(Format("unsupported vertex format %u", uint32_t(f.VertexDataFormat())));
                    return;
                }

                if (packed)
                {
                    // Words in the order the components use them.
                    uint32_t wordCount = (f.VertexDataFormat() == VertexFormat::F16_16_16_16) ? 2 : 1;
                    Line(Format("uint w0 = xe_vfetch(%u, address);", constant));
                    if (wordCount == 2)
                        Line(Format("uint w1 = xe_vfetch(%u, address + 1);", constant));
                    std::string values[4];
                    for (uint32_t c = 0; c < 4; c++)
                    {
                        if (c >= components)
                        {
                            values[c] = c == 3 ? "1.0" : "0.0";
                            continue;
                        }
                        const char* word = (wordCount == 2 && c >= 2) ? "w1" : "w0";
                        int width = widths[c];
                        if (f.VertexIsSigned())
                        {
                            std::string extracted = Format("float(bitfieldExtract(int(%s), %d, %d))", word, offsets[c], width);
                            if (f.VertexIsInteger())
                                values[c] = extracted;
                            else
                            {
                                float maxValue = float((1u << (width - 1)) - 1);
                                if (f.VertexSignedRfZeroClamp())
                                    values[c] = Format("max(%s * %.9g, -1.0)", extracted.c_str(), 1.0f / maxValue);
                                else
                                    values[c] = Format("(%s * 2.0 + 1.0) * %.9g", extracted.c_str(), 1.0 / (double((1u << width) - 1)));
                            }
                        }
                        else
                        {
                            std::string extracted = Format("float(bitfieldExtract(%s, %d, %d))", word, offsets[c], width);
                            values[c] = f.VertexIsInteger() ? extracted
                                                            : Format("%s * %.9g", extracted.c_str(), 1.0 / double((1u << width) - 1));
                        }
                    }
                    Line(Format("vec4 fetched = vec4(%s, %s, %s, %s);", values[0].c_str(), values[1].c_str(),
                        values[2].c_str(), values[3].c_str()));
                }
                else if (components < 4 && (f.VertexDataFormat() == VertexFormat::F32_Float ||
                                               f.VertexDataFormat() == VertexFormat::F32_32_Float ||
                                               f.VertexDataFormat() == VertexFormat::F32_32_32_Float))
                {
                    // Unused components of float formats read as 0 (w as 1).
                    for (uint32_t c = components; c < 3; c++)
                        Line(Format("fetched.%c = 0.0;", kSwizzleChars[c]));
                }

                if (f.VertexExpAdjust() != 0)
                    Line(Format("fetched *= %.9g;", double(std::ldexp(1.0, f.VertexExpAdjust()))));

                std::string dst = TempRegister(f.DstReg(), f.DstRelative());
                std::string writes = DestinationWrites(f, "fetched");
                if (!writes.empty())
                    Line(Format("vec4 dst = %s; %s %s = dst;", dst.c_str(), writes.c_str(), dst.c_str()));
            }

            void EmitTextureFetch(const FetchInstruction& f)
            {
                std::string dst = TempRegister(f.DstReg(), f.DstRelative());
                std::string src = TempRegister(f.SrcReg(), f.SrcRelative());
                uint32_t srcSwizzle = f.TextureSrcSwizzle();
                auto coordinate = [&](uint32_t c) {
                    return Format("%s.%c", src.c_str(), kSwizzleChars[(srcSwizzle >> (2 * c)) & 3]);
                };

                switch (f.Opcode())
                {
                case FetchOpcode::TextureFetch:
                {
                    uint32_t binding = TextureBindingFor(f.TextureConstIndex(), f.TextureDim());
                    std::string sampler = Format("xe_texture%u", binding);
                    std::string coords;
                    switch (f.TextureDim())
                    {
                    case TextureDimension::D1:
                        coords = Format("vec2(%s, 0.5)", coordinate(0).c_str());
                        break;
                    case TextureDimension::D2:
                        coords = Format("vec2(%s, %s)", coordinate(0).c_str(), coordinate(1).c_str());
                        break;
                    case TextureDimension::D3OrStacked:
                        coords = Format("vec3(%s, %s, %s)", coordinate(0).c_str(), coordinate(1).c_str(), coordinate(2).c_str());
                        break;
                    case TextureDimension::Cube:
                        // Xenos cube coordinates: SC/TC in [1, 2) and the face
                        // ID; turn them back into a direction.
                        coords = Format("xe_cube_direction(vec3(%s, %s, %s))", coordinate(0).c_str(), coordinate(1).c_str(),
                            coordinate(2).c_str());
                        break;
                    }
                    // Fetch offsets (half-texel steps), as multi-tap filters
                    // use them: in texels before normalizing.
                    if (f.TextureDim() != TextureDimension::Cube)
                    {
                        bool volume = f.TextureDim() == TextureDimension::D3OrStacked;
                        const char* sizeType = volume ? "vec3" : "vec2";
                        float ox = f.TextureOffset(0), oy = f.TextureDim() == TextureDimension::D1 ? 0.0f : f.TextureOffset(1),
                              oz = volume ? f.TextureOffset(2) : 0.0f;
                        bool offset = ox != 0.0f || oy != 0.0f || oz != 0.0f;
                        std::string offsetVector = volume ? Format("vec3(%.9g, %.9g, %.9g)", ox, oy, oz) : Format("vec2(%.9g, %.9g)", ox, oy);
                        if (f.TextureUnnormalized())
                        {
                            if (offset)
                                coords = Format("(%s + %s)", coords.c_str(), offsetVector.c_str());
                            coords = Format("(%s / %s(textureSize(%s, 0)))", coords.c_str(), sizeType, sampler.c_str());
                        }
                        else if (offset)
                            coords = Format("(%s + %s / %s(textureSize(%s, 0)))", coords.c_str(), offsetVector.c_str(), sizeType,
                                sampler.c_str());
                    }
                    std::string sample;
                    float bias = f.TextureLodBias();
                    if (m_vertex)
                        sample = Format("textureLod(%s, %s, 0.0)", sampler.c_str(), coords.c_str());
                    else if (bias != 0.0f)
                        sample = Format("texture(%s, %s, %.9g)", sampler.c_str(), coords.c_str(), bias);
                    else
                        sample = Format("texture(%s, %s)", sampler.c_str(), coords.c_str());
                    sample = Format("xe_texture_result(%s, %uu)", sample.c_str(), f.TextureConstIndex());
                    std::string writes = DestinationWrites(f, "fetched");
                    if (!writes.empty())
                        Line(Format("vec4 fetched = %s; vec4 dst = %s; %s %s = dst;", sample.c_str(), dst.c_str(),
                            writes.c_str(), dst.c_str()));
                    return;
                }
                case FetchOpcode::GetTextureWeights:
                {
                    // Bilinear weights: the fractional texel position.
                    uint32_t binding = TextureBindingFor(f.TextureConstIndex(), f.TextureDim());
                    std::string size = "vec2(textureSize(xe_texture%u, 0).xy)";
                    Line(Format(("vec2 texel = vec2(%s, %s) * " + size + " - 0.5;").c_str(), coordinate(0).c_str(),
                        coordinate(1).c_str(), binding));
                    std::string writes = DestinationWrites(f, "fetched");
                    if (!writes.empty())
                        Line(Format("vec4 fetched = vec4(fract(texel), 0.0, 0.0); vec4 dst = %s; %s %s = dst;", dst.c_str(),
                            writes.c_str(), dst.c_str()));
                    return;
                }
                case FetchOpcode::GetTextureComputedLod:
                {
                    std::string writes = DestinationWrites(f, "fetched");
                    if (!writes.empty() && !m_vertex)
                    {
                        uint32_t binding = TextureBindingFor(f.TextureConstIndex(), f.TextureDim());
                        Line(Format("vec4 fetched = vec4(textureQueryLod(xe_texture%u, vec2(%s, %s)).y); vec4 dst = %s; %s %s = dst;",
                            binding, coordinate(0).c_str(), coordinate(1).c_str(), dst.c_str(), writes.c_str(), dst.c_str()));
                    }
                    return;
                }
                case FetchOpcode::GetTextureGradients:
                {
                    std::string writes = DestinationWrites(f, "fetched");
                    if (!writes.empty() && !m_vertex)
                        Line(Format("vec2 g = vec2(%s, %s); vec4 fetched = vec4(dFdx(g.x), dFdy(g.x), dFdx(g.y), dFdy(g.y)); "
                                    "vec4 dst = %s; %s %s = dst;",
                            coordinate(0).c_str(), coordinate(1).c_str(), dst.c_str(), writes.c_str(), dst.c_str()));
                    return;
                }
                case FetchOpcode::SetTextureLod:
                case FetchOpcode::SetTextureGradientsHorz:
                case FetchOpcode::SetTextureGradientsVert:
                case FetchOpcode::GetTextureBorderColorFrac:
                    // Register LOD / gradients aren't used by the sampling above.
                    return;
                default:
                    Fail(Format("unknown fetch opcode %u", uint32_t(f.Opcode())));
                    return;
                }
            }

            // --- Assembly ------------------------------------------------

            std::string SamplerType(TextureDimension dimension)
            {
                switch (dimension)
                {
                case TextureDimension::D1: return "sampler2D";  // SDL_GPU has no 1D textures
                case TextureDimension::D2: return "sampler2D";
                case TextureDimension::D3OrStacked: return "sampler3D";
                case TextureDimension::Cube: return "samplerCube";
                }
                return "sampler2D";
            }

            std::string EmitShader()
            {
                uint32_t textureSet = m_vertex ? 0 : 2;
                uint32_t uniformSet = m_vertex ? 1 : 3;

                std::string out = "#version 450\n";
                out += Format("layout(std140, set = %u, binding = 0) uniform XeFloatConstants { vec4 c[%u]; };\n", uniformSet,
                    kFloatConstantCount);
                out += Format("layout(std140, set = %u, binding = 1) uniform XeSystem\n{\n"
                              "    uvec4 bools[2];\n    uvec4 loops[8];\n    uvec4 vfetch[96];\n"
                              "    vec4 ndc_scale;\n    vec4 ndc_offset;\n    uint ps_param_gen;\n    uint flags;\n    uint rect_indices;\n"
                              "    uvec4 tfetch[32];\n    vec4 color_exp_bias;\n    float alpha_ref;\n} xe_sys;\n",
                    uniformSet);
                for (const TextureBinding& texture : m_info.textures)
                    out += Format("layout(set = %u, binding = %u) uniform %s xe_texture%u;\n", textureSet, texture.binding,
                        SamplerType(texture.dimension).c_str(), texture.binding);
                if (m_vertex)
                {
                    out += Format("layout(std430, set = 0, binding = %u) readonly buffer XeVertexMemory { uint xe_memory[]; };\n",
                        m_info.storageBufferBinding);
                    out += "layout(location = 0) out vec4 xe_out[16];\n";
                }
                else
                {
                    out += "layout(location = 0) in vec4 xe_in[16];\n";
                    for (uint32_t i = 0; i < 4; i++)
                        if (m_info.colorTargetMask & (1u << i))
                            out += Format("layout(location = %u) out vec4 xe_color%u_out;\n", i, i);
                }
                out += kPrelude;

                if (m_vertex)
                {
                    out += R"(
// Dword `offset` of vertex fetch constant `constant`'s data, byte-swapped;
// 0 past the end, like the hardware.
uint xe_vfetch(uint constant, int offset)
{
    uvec4 info = xe_sys.vfetch[constant];
    if (offset < 0 || uint(offset) >= info.y)
        return 0u;
    return xe_swap(xe_memory[info.x + uint(offset)], info.z);
}
float xe_round_index(float value) { return floor(value + 0.5); }
)";
                }
                out += R"(
vec4 xe_cube(vec4 v)
{
    // v is src0 = source.zzxy: x = v.z, y = v.w, z = v.x.
    float x = v.z, y = v.w, z = v.x;
    float tc, sc, ma, id;
    if (abs(z) >= abs(x) && abs(z) >= abs(y)) { tc = -y; sc = z < 0.0 ? -x : x; ma = 2.0 * z; id = z < 0.0 ? 5.0 : 4.0; }
    else if (abs(y) >= abs(x)) { tc = y < 0.0 ? -z : z; sc = x; ma = 2.0 * y; id = y < 0.0 ? 3.0 : 2.0; }
    else { tc = -y; sc = x < 0.0 ? z : -z; ma = 2.0 * x; id = x < 0.0 ? 1.0 : 0.0; }
    return vec4(tc, sc, ma, id);
}
vec4 xe_max4(vec4 v)
{
    if (v.x > v.y && v.x > v.z && v.x > v.w) return vec4(v.x);
    if (v.y > v.z && v.y > v.w) return vec4(v.y);
    if (v.z > v.w) return vec4(v.z);
    return vec4(v.w);
}
// The texture fetch constant's view of a sample (xe_sys.tfetch, set for
// textures decoded from memory): per-component sign mode (signed, biased,
// gamma), then the swizzle, then the exponent adjustment.
vec4 xe_texture_result(vec4 v, uint fetchConstant)
{
    uvec4 t = xe_sys.tfetch[fetchConstant];
    if (t.w == 0u)
        return v;
    for (int i = 0; i < 4; i++)
    {
        uint sign = (t.y >> (2 * i)) & 3u;
        if (sign == 1u)
        {
            float s = floor(v[i] * 255.0 + 0.5);
            v[i] = max((s >= 128.0 ? s - 256.0 : s) / 127.0, -1.0);
        }
        else if (sign == 2u)
            v[i] = v[i] * 2.0 - 1.0;
        else if (sign == 3u)
            v[i] = v[i] * v[i];  // approximates the piecewise linear gamma (Halo's own decode squares)
    }
    vec4 r;
    for (int i = 0; i < 4; i++)
    {
        uint s = (t.x >> (3 * i)) & 7u;
        r[i] = s < 4u ? v[s] : (s == 5u ? 1.0 : 0.0);
    }
    return r * exp2(float(int(t.z)));
}

// Xenos compare functions: never, <, ==, <=, >, !=, >=, always.
bool xe_compare(uint func, float a, float b)
{
    switch (func)
    {
    case 0u: return false;
    case 1u: return a < b;
    case 2u: return a == b;
    case 3u: return a <= b;
    case 4u: return a > b;
    case 5u: return a != b;
    case 6u: return a >= b;
    default: return true;
    }
}

vec3 xe_cube_direction(vec3 stf)
{
    // Inverse of cube + "mad r.xy, r, 1/|ma|, 1.5": face coordinates in
    // [1, 2) back to [-1, 1], then to a direction for the face.
    vec2 st = stf.xy * 2.0 - 3.0;
    int face = int(stf.z + 0.5);
    if (face == 0) return vec3(1.0, -st.y, -st.x);
    if (face == 1) return vec3(-1.0, -st.y, st.x);
    if (face == 2) return vec3(st.x, 1.0, st.y);
    if (face == 3) return vec3(st.x, -1.0, -st.y);
    if (face == 4) return vec3(st.x, -st.y, 1.0);
    return vec3(-st.x, -st.y, -1.0);
}
)";

                if (m_vertex)
                    out += "\nvoid xe_vertex_shader(int xe_vertex_index, out vec4 xe_position, out vec4 xe_outputs[16])\n{\n";
                else
                    out += "\nvoid main()\n{\n";
                out += "    vec4 r[64];\n    for (int i = 0; i < 64; i++) r[i] = vec4(0.0);\n";
                out += "    int a0 = 0;\n    int aL = 0;\n    bool p0 = false;\n    float ps = 0.0;\n";
                out += "    int newA0 = 0;\n    bool newP0 = false;\n    bool kill = false;\n";
                out += "    int loopDepth = -1;\n    int loopSavedAL[4];\n    int loopIteration[4];\n    uint loopConstant[4];\n";
                out += "    int callDepth = -1;\n    int callStack[4];\n";
                if (m_vertex)
                {
                    out += "    xe_position = vec4(0.0, 0.0, 0.0, 1.0);\n";
                    out += "    for (int i = 0; i < 16; i++) xe_outputs[i] = vec4(0.0);\n";
                    out += "    r[0].x = float(xe_vertex_index);\n";
                }
                else
                {
                    out += "    for (int i = 0; i < 16; i++) r[i] = xe_in[i];\n";
                    out += "    if (xe_sys.ps_param_gen < 64u)\n"
                           "        r[xe_sys.ps_param_gen] = vec4(gl_FragCoord.xy, gl_FrontFacing ? 0.0 : -0.0, 0.0);\n";
                    for (uint32_t i = 0; i < 4; i++)
                        if (m_info.colorTargetMask & (1u << i))
                            out += Format("    vec4 xe_color%u = vec4(0.0);\n", i);
                    if (m_info.writesDepth)
                        out += "    vec4 xe_depth = vec4(gl_FragCoord.z);\n";
                }
                out += "    int pc = 0;\n    bool running = true;\n";
                out += "    while (running)\n    {\n        switch (pc)\n        {\n";
                out += m_body;
                out += "        default:\n            running = false;\n            break;\n";
                out += "        }\n    }\n";
                if (m_vertex)
                {
                    out += "}\n";
                    out += kVertexMain;
                    return out;
                }
                // Alpha test (RB_COLORCONTROL): flags bits 5-7 compare
                // function, bit 8 enable; color 0's alpha against alpha_ref.
                if (m_info.colorTargetMask & 1)
                    out += "    if ((xe_sys.flags & 0x100u) != 0u && !xe_compare((xe_sys.flags >> 5) & 7u, xe_color0.a, xe_sys.alpha_ref))\n"
                           "        discard;\n";
                for (uint32_t i = 0; i < 4; i++)
                    if (m_info.colorTargetMask & (1u << i))
                        out += Format("    xe_color%u_out = xe_color%u * xe_sys.color_exp_bias[%u];\n", i, i, i);
                out += m_info.writesDepth ? "    float xe_frag_depth = xe_depth.x;\n" : "    float xe_frag_depth = gl_FragCoord.z;\n";
                out += "    if ((xe_sys.flags & 2u) != 0u)\n        xe_frag_depth = xe_float24_round(xe_frag_depth);\n";
                out += "    gl_FragDepth = xe_frag_depth;\n";
                out += "}\n";
                return out;
            }
        };
    }

    TranslationResult Translate(bool isVertex, const uint32_t* dwords, uint32_t dwordCount)
    {
        return Translator(isVertex, dwords, dwordCount).Run();
    }

    std::string Disassemble(bool isVertex, const uint32_t* dwords, uint32_t dwordCount)
    {
        return Translator(isVertex, dwords, dwordCount).Disassemble();
    }
}
