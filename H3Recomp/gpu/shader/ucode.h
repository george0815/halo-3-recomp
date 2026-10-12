#pragma once

// Xenos shader microcode decoding. Field layouts follow Xenia's
// src/xenia/gpu/ucode.h (BSD license), which documents the instruction set in
// detail; the semantics notes in translator.cpp come from the same place.
//
// A shader is a sequence of host-endian dwords. Control flow instructions
// (48 bits, packed two per three dwords) come first; ALU and fetch
// instructions (96 bits, three dwords each) follow, addressed in units of
// three dwords from the start of the shader.

#include <cstdint>

namespace h3::gpu::ucode
{
    constexpr uint32_t Bits(uint32_t value, uint32_t shift, uint32_t count)
    {
        return (value >> shift) & ((1u << count) - 1);
    }

    constexpr int32_t SignedBits(uint32_t value, uint32_t shift, uint32_t count)
    {
        return int32_t(value << (32 - shift - count)) >> (32 - count);
    }

    // --- Control flow ----------------------------------------------------

    enum class CfOpcode : uint32_t
    {
        Nop = 0,
        Exec = 1,
        ExecEnd = 2,
        CondExec = 3,           // on a bool constant
        CondExecEnd = 4,
        CondExecPred = 5,       // on the predicate
        CondExecPredEnd = 6,
        LoopStart = 7,
        LoopEnd = 8,
        CondCall = 9,
        Return = 10,
        CondJmp = 11,
        Alloc = 12,
        CondExecPredClean = 13,  // on a bool constant (despite the name)
        CondExecPredCleanEnd = 14,
        MarkVsFetchDone = 15,
    };

    // One 48-bit control flow instruction: word0 is 32 bits, word1 16 bits.
    struct CfInstruction
    {
        uint32_t word0 = 0, word1 = 0;

        CfOpcode Opcode() const { return CfOpcode(Bits(word1, 12, 4)); }
        bool AddressRelative() const { return Bits(word1, 11, 1) == 0; }  // aL rather than a0

        // Exec family.
        uint32_t ExecAddress() const { return Bits(word0, 0, 12); }
        uint32_t ExecCount() const { return Bits(word0, 12, 3); }
        uint32_t ExecSequence() const { return Bits(word0, 16, 12); }  // 2 bits per instruction
        uint32_t BoolAddress() const { return Bits(word1, 2, 8); }  // CondExec / CondCall / CondJmp
        bool Condition() const { return Bits(word1, 10, 1) != 0; }

        // Loops, calls and jumps.
        uint32_t TargetAddress() const { return Bits(word0, 0, 13); }
        bool LoopIsRepeat() const { return Bits(word0, 13, 1) != 0; }
        uint32_t LoopId() const { return Bits(word0, 16, 5); }
        bool LoopIsPredicatedBreak() const { return Bits(word0, 21, 1) != 0; }
        bool IsUnconditional() const { return Bits(word0, 13, 1) != 0; }  // CondCall / CondJmp
        bool IsPredicated() const { return Bits(word0, 14, 1) != 0; }      // CondCall / CondJmp

        // Alloc.
        uint32_t AllocSize() const { return Bits(word0, 0, 3); }
        uint32_t AllocType() const { return Bits(word1, 9, 2); }

        bool IsExec() const
        {
            switch (Opcode())
            {
            case CfOpcode::Exec:
            case CfOpcode::ExecEnd:
            case CfOpcode::CondExec:
            case CfOpcode::CondExecEnd:
            case CfOpcode::CondExecPred:
            case CfOpcode::CondExecPredEnd:
            case CfOpcode::CondExecPredClean:
            case CfOpcode::CondExecPredCleanEnd:
                return true;
            default:
                return false;
            }
        }

        bool EndsShader() const
        {
            CfOpcode op = Opcode();
            return op == CfOpcode::ExecEnd || op == CfOpcode::CondExecEnd || op == CfOpcode::CondExecPredEnd ||
                op == CfOpcode::CondExecPredCleanEnd;
        }
    };

    // Unpacks control flow instructions 2 * pair and 2 * pair + 1.
    inline void UnpackCfPair(const uint32_t* dwords, uint32_t pair, CfInstruction out[2])
    {
        const uint32_t* d = dwords + pair * 3;
        out[0].word0 = d[0];
        out[0].word1 = d[1] & 0xFFFF;
        out[1].word0 = (d[1] >> 16) | (d[2] << 16);
        out[1].word1 = d[2] >> 16;
    }

    // --- Fetch -----------------------------------------------------------

    enum class FetchOpcode : uint32_t
    {
        VertexFetch = 0,
        TextureFetch = 1,
        GetTextureBorderColorFrac = 16,
        GetTextureComputedLod = 17,
        GetTextureGradients = 18,
        GetTextureWeights = 19,
        SetTextureLod = 24,
        SetTextureGradientsHorz = 25,
        SetTextureGradientsVert = 26,
    };

    enum class TextureDimension : uint32_t
    {
        D1 = 0,
        D2 = 1,
        D3OrStacked = 2,
        Cube = 3,
    };

    // Vertex formats (xenos::VertexFormat).
    enum class VertexFormat : uint32_t
    {
        Undefined = 0,
        F8_8_8_8 = 6,
        F2_10_10_10 = 7,
        F10_11_11 = 16,
        F11_11_10 = 17,
        F16_16 = 25,
        F16_16_16_16 = 26,
        F16_16_Float = 31,
        F16_16_16_16_Float = 32,
        F32 = 33,
        F32_32 = 34,
        F32_32_32_32 = 35,
        F32_Float = 36,
        F32_32_Float = 37,
        F32_32_32_32_Float = 38,
        F32_32_32_Float = 57,
    };

    // Texture fetch filter fields (instruction overrides and fetch constant):
    // point, linear, base map (mip filter only), or "use the fetch constant's"
    // (instruction only).
    constexpr uint32_t kTextureFilterPoint = 0;
    constexpr uint32_t kTextureFilterLinear = 1;
    constexpr uint32_t kTextureFilterBaseMap = 2;
    constexpr uint32_t kTextureFilterUseFetchConstant = 3;

    struct FetchInstruction
    {
        uint32_t d[3] = {};

        // Common.
        FetchOpcode Opcode() const { return FetchOpcode(Bits(d[0], 0, 5)); }
        uint32_t SrcReg() const { return Bits(d[0], 5, 6); }
        bool SrcRelative() const { return Bits(d[0], 11, 1) != 0; }
        uint32_t DstReg() const { return Bits(d[0], 12, 6); }
        bool DstRelative() const { return Bits(d[0], 18, 1) != 0; }
        uint32_t DstSwizzle() const { return Bits(d[1], 0, 12); }  // 3 bits per component, absolute
        bool IsPredicated() const { return Bits(d[1], 31, 1) != 0; }
        bool PredicateCondition() const { return Bits(d[2], 31, 1) != 0; }

        // Vertex fetch.
        uint32_t VertexConstIndex() const { return Bits(d[0], 20, 5) * 3 + Bits(d[0], 25, 2); }
        uint32_t VertexSrcSwizzle() const { return Bits(d[0], 30, 2); }
        bool VertexIsSigned() const { return Bits(d[1], 12, 1) != 0; }
        bool VertexIsInteger() const { return Bits(d[1], 13, 1) != 0; }  // num_format_all = 1
        bool VertexSignedRfZeroClamp() const { return Bits(d[1], 14, 1) == 0; }
        bool VertexIndexRounded() const { return Bits(d[1], 15, 1) != 0; }
        VertexFormat VertexDataFormat() const { return VertexFormat(Bits(d[1], 16, 6)); }
        int32_t VertexExpAdjust() const { return SignedBits(d[1], 24, 6); }
        bool VertexIsMini() const { return Bits(d[1], 30, 1) != 0; }
        uint32_t VertexStride() const { return Bits(d[2], 0, 8); }      // dwords
        int32_t VertexOffset() const { return SignedBits(d[2], 8, 23); }  // dwords

        // Texture fetch.
        uint32_t TextureConstIndex() const { return Bits(d[0], 20, 5); }
        bool TextureUnnormalized() const { return Bits(d[0], 25, 1) != 0; }
        uint32_t TextureSrcSwizzle() const { return Bits(d[0], 26, 6); }  // 2 bits per component, absolute
        // Filter overrides (TextureFilter values below; kTextureFilterUseFetchConstant
        // defers to the fetch constant's filter).
        uint32_t TextureMagFilter() const { return Bits(d[1], 12, 2); }
        uint32_t TextureMinFilter() const { return Bits(d[1], 14, 2); }
        uint32_t TextureMipFilter() const { return Bits(d[1], 16, 2); }
        bool TextureUseRegisterLod() const { return Bits(d[1], 29, 1) != 0; }
        bool TextureUseComputedLod() const { return Bits(d[1], 28, 1) != 0; }
        bool TextureUseRegisterGradients() const { return Bits(d[2], 0, 1) != 0; }
        float TextureLodBias() const { return float(SignedBits(d[2], 2, 7)) / 16.0f; }
        TextureDimension TextureDim() const { return TextureDimension(Bits(d[2], 14, 2)); }
        float TextureOffset(uint32_t axis) const { return float(SignedBits(d[2], 16 + axis * 5, 5)) * 0.5f; }
    };

    // --- ALU -------------------------------------------------------------

    enum class VectorOpcode : uint32_t
    {
        Add = 0, Mul = 1, Max = 2, Min = 3, Seq = 4, Sgt = 5, Sge = 6, Sne = 7,
        Frc = 8, Trunc = 9, Floor = 10, Mad = 11, CndEq = 12, CndGe = 13, CndGt = 14,
        Dp4 = 15, Dp3 = 16, Dp2Add = 17, Cube = 18, Max4 = 19,
        SetpEqPush = 20, SetpNePush = 21, SetpGtPush = 22, SetpGePush = 23,
        KillEq = 24, KillGt = 25, KillGe = 26, KillNe = 27, Dst = 28, MaxA = 29,
    };

    enum class ScalarOpcode : uint32_t
    {
        Adds = 0, AddsPrev = 1, Muls = 2, MulsPrev = 3, MulsPrev2 = 4, Maxs = 5, Mins = 6,
        Seqs = 7, Sgts = 8, Sges = 9, Snes = 10, Frcs = 11, Truncs = 12, Floors = 13,
        Exp = 14, Logc = 15, Log = 16, Rcpc = 17, Rcpf = 18, Rcp = 19, Rsqc = 20, Rsqf = 21, Rsq = 22,
        MaxAs = 23, MaxAsf = 24, Subs = 25, SubsPrev = 26,
        SetpEq = 27, SetpNe = 28, SetpGt = 29, SetpGe = 30, SetpInv = 31, SetpPop = 32, SetpClr = 33, SetpRstr = 34,
        KillsEq = 35, KillsGt = 36, KillsGe = 37, KillsNe = 38, KillsOne = 39, Sqrt = 40,
        Mulsc0 = 42, Mulsc1 = 43, Addsc0 = 44, Addsc1 = 45, Subsc0 = 46, Subsc1 = 47,
        Sin = 48, Cos = 49, RetainPrev = 50,
    };

    // Number of vector operands (Xenia's kAluVectorOpcodeInfos).
    inline uint32_t VectorOperandCount(VectorOpcode op)
    {
        switch (op)
        {
        case VectorOpcode::Frc:
        case VectorOpcode::Trunc:
        case VectorOpcode::Floor:
        case VectorOpcode::Max4:
            return 1;
        case VectorOpcode::Mad:
        case VectorOpcode::CndEq:
        case VectorOpcode::CndGe:
        case VectorOpcode::CndGt:
        case VectorOpcode::Dp2Add:
            return 3;
        default:
            return 2;
        }
    }

    // Scalar operand shape: 0 = none, 1 = one source (W, or W and X/Z when
    // two-component), 2 = constant W and temporary X (mulsc/addsc/subsc).
    inline uint32_t ScalarOperandCount(ScalarOpcode op)
    {
        switch (op)
        {
        case ScalarOpcode::SetpClr:
        case ScalarOpcode::RetainPrev:
            return 0;
        case ScalarOpcode::Mulsc0:
        case ScalarOpcode::Mulsc1:
        case ScalarOpcode::Addsc0:
        case ScalarOpcode::Addsc1:
        case ScalarOpcode::Subsc0:
        case ScalarOpcode::Subsc1:
            return 2;
        default:
            return uint32_t(op) <= uint32_t(ScalarOpcode::RetainPrev) && uint32_t(op) != 41 ? 1 : 0;
        }
    }

    inline bool ScalarIsTwoComponent(ScalarOpcode op)
    {
        switch (op)
        {
        case ScalarOpcode::Adds:
        case ScalarOpcode::Muls:
        case ScalarOpcode::MulsPrev2:
        case ScalarOpcode::Maxs:
        case ScalarOpcode::Mins:
        case ScalarOpcode::MaxAs:
        case ScalarOpcode::MaxAsf:
        case ScalarOpcode::Subs:
            return true;
        default:
            return false;
        }
    }

    // Export registers (ALU destination when exporting).
    constexpr uint32_t kExportVsInterpolator0 = 0;  // .. 15
    constexpr uint32_t kExportVsPosition = 62;
    constexpr uint32_t kExportVsPointSize = 63;
    constexpr uint32_t kExportPsColor0 = 0;  // .. 3
    constexpr uint32_t kExportPsDepth = 61;
    constexpr uint32_t kExportAddress = 32;
    constexpr uint32_t kExportData0 = 33;  // .. 37
    constexpr uint32_t kMemExportElements = 5;  // eM0-eM4

    struct AluInstruction
    {
        uint32_t d[3] = {};

        // Word 0.
        uint32_t VectorDest() const { return Bits(d[0], 0, 6); }
        bool VectorDestRelative() const { return Bits(d[0], 6, 1) != 0; }
        bool AbsConstants() const { return Bits(d[0], 7, 1) != 0; }
        uint32_t ScalarDest() const { return Bits(d[0], 8, 6); }
        bool ScalarDestRelative() const { return Bits(d[0], 14, 1) != 0; }
        bool IsExport() const { return Bits(d[0], 15, 1) != 0; }
        uint32_t VectorWriteMask() const { return Bits(d[0], 16, 4); }
        uint32_t ScalarWriteMask() const { return Bits(d[0], 20, 4); }
        bool VectorClamp() const { return Bits(d[0], 24, 1) != 0; }
        bool ScalarClamp() const { return Bits(d[0], 25, 1) != 0; }
        ScalarOpcode ScalarOp() const { return ScalarOpcode(Bits(d[0], 26, 6)); }

        // Word 1.
        uint32_t SrcSwizzle(uint32_t i) const { return Bits(d[1], 16 - (i - 1) * 8 + 0, 8); }  // i = 1..3
        bool SrcNegate(uint32_t i) const { return Bits(d[1], 26 - (i - 1), 1) != 0; }
        bool PredicateCondition() const { return Bits(d[1], 27, 1) != 0; }
        bool IsPredicated() const { return Bits(d[1], 28, 1) != 0; }
        bool ConstAddressRegisterRelative() const { return Bits(d[1], 29, 1) != 0; }  // a0 rather than aL
        bool Const1RelAbs() const { return Bits(d[1], 30, 1) != 0; }
        bool Const0RelAbs() const { return Bits(d[1], 31, 1) != 0; }

        // Word 2.
        uint32_t SrcReg(uint32_t i) const { return Bits(d[2], 16 - (i - 1) * 8, 8); }  // i = 1..3
        VectorOpcode VectorOp() const { return VectorOpcode(Bits(d[2], 24, 5)); }
        bool SrcIsTemp(uint32_t i) const { return Bits(d[2], 31 - (i - 1), 1) != 0; }

        // Whether constant operand i uses relative addressing (see Xenia's
        // src_const_is_addressed).
        bool SrcConstAddressed(uint32_t i) const
        {
            if (i == 1)
                return Const0RelAbs();
            if (i == 2)
                return SrcIsTemp(1) ? Const0RelAbs() : Const1RelAbs();
            return (SrcIsTemp(1) && SrcIsTemp(2)) ? Const0RelAbs() : Const1RelAbs();
        }

        // Temporary register of mulsc/addsc/subsc.
        uint32_t ScalarConstOpTempReg() const
        {
            return (uint32_t(ScalarOp()) & 1) | (Bits(d[2], 29, 1) << 1) | (SrcSwizzle(3) & 0x3C);
        }

        // Export write masks (Xenia's GetVectorOpResultWriteMask etc.).
        uint32_t VectorResultMask() const
        {
            return IsExport() ? VectorWriteMask() & ~ScalarWriteMask() : VectorWriteMask();
        }
        uint32_t ScalarResultMask() const
        {
            return IsExport() ? ScalarWriteMask() & ~VectorWriteMask() : ScalarWriteMask();
        }
        uint32_t Constant0Mask() const
        {
            return IsExport() && ScalarDestRelative() ? 0xF & ~(VectorWriteMask() | ScalarWriteMask()) : 0;
        }
        uint32_t Constant1Mask() const { return IsExport() ? VectorWriteMask() & ScalarWriteMask() : 0; }
    };

    // Component of a (relative) ALU source swizzle.
    constexpr uint32_t SwizzledComponent(uint32_t swizzle, uint32_t component)
    {
        return ((swizzle >> (2 * component)) + component) & 3;
    }
}
