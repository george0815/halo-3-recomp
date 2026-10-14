// Calling recompiled code from decompiled code (docs/ROADMAP.md 6, stage
// "decompiled in place"): a replacement is a PPC_FUNC that does its work in
// C++ and calls whatever is still recompiled with the guest calling
// convention.
//
//   Call(ctx, base, sub_XXXXXXXX, a, b, ...)  integer arguments in r3-r10;
//                                             returns r3
//   CallIndirect(ctx, base, address, a, ...)  the same through a guest
//                                             function pointer
//   GuestFrame frame(ctx, bytes)              stack space for buffers a
//                                             callee writes through a
//                                             pointer (frame.Address(off))
//
// Callees preserve r14-r31 as the ABI says and the replacement doesn't
// touch them, so the replacement's own callers see them preserved too.
// Volatile registers (r0, r3-r12, cr) come back clobbered, as from the
// original.

#pragma once

#include <ppc_recomp_shared.h>

#include <cstdint>

namespace h3::native
{
    using GuestFunction = PPCFunc*;

    namespace detail
    {
        inline PPCRegister& ArgumentRegister(PPCContext& ctx, int index)
        {
            PPCRegister* registers[] = { &ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10 };
            return *registers[index];
        }

        template <typename... Args>
        void SetArguments(PPCContext& ctx, Args... args)
        {
            static_assert(sizeof...(Args) <= 8, "more than 8 integer arguments go on the stack");
            int index = 0;
            ((ArgumentRegister(ctx, index++).u64 = uint64_t(uint32_t(args))), ...);
        }
    }

    // Calls a recompiled function with 32-bit integer (or guest pointer)
    // arguments; returns its r3.
    template <typename... Args>
    uint32_t Call(PPCContext& ctx, uint8_t* base, GuestFunction function, Args... args)
    {
        detail::SetArguments(ctx, args...);
        function(ctx, base);
        return ctx.r3.u32;
    }

    // The same through a guest function pointer (a vtable or callback entry).
    template <typename... Args>
    uint32_t CallIndirect(PPCContext& ctx, uint8_t* base, uint32_t address, Args... args)
    {
        detail::SetArguments(ctx, args...);
        PPC_CALL_INDIRECT_FUNC(address);
        return ctx.r3.u32;
    }

    // Guest stack space for the replacement's lifetime: r1 moves down by
    // bytes (16-byte aligned, plus the ABI's back chain word at the new
    // r1) and back on destruction.
    class GuestFrame
    {
    public:
        GuestFrame(PPCContext& ctx, uint8_t* base, uint32_t bytes) : m_ctx(ctx), m_saved(ctx.r1.u32)
        {
            constexpr uint32_t kLinkArea = 80;  // what callees may use above r1 (back chain, LR/CR save)
            uint32_t size = (bytes + kLinkArea + 15) & ~15u;
            m_ctx.r1.u64 = m_saved - size;
            PPC_STORE_U32(m_ctx.r1.u32, m_saved);  // back chain
            m_buffers = m_ctx.r1.u32 + kLinkArea;
        }
        ~GuestFrame() { m_ctx.r1.u64 = m_saved; }
        GuestFrame(const GuestFrame&) = delete;
        GuestFrame& operator=(const GuestFrame&) = delete;

        // Guest address of byte offset within the frame's buffer space.
        uint32_t Address(uint32_t offset) const { return m_buffers + offset; }

    private:
        PPCContext& m_ctx;
        uint32_t m_saved;
        uint32_t m_buffers;
    };
}
