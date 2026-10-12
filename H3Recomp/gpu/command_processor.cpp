// The command processor (see command_processor.h): reads PM4 packets from
// the ring buffer on its own thread and executes them. Packet types:
//   0  write consecutive registers      1  write two registers
//   2  no-op                            3  an opcode (ExecuteType3)
// Also delivers the GPU's interrupts (command processor and 60 Hz vblank)
// to the game's graphics interrupt callback.

#include "command_processor.h"
#include "config/env.h"
#include "kernel/objects.h"

#include <ppc_recomp_shared.h>

#include "../kernel/thread.h"
#include "../memory.h"
#include "../video/window.h"
#include "frame_capture.h"
#include "renderer.h"
#include "registers.h"
#include "xenos.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace h3::gpu
{
    namespace
    {
        // Register indices, from Xenia's register_table.inc.
        constexpr uint32_t kRegCpRbRptr = 0x01C4;
        constexpr uint32_t kRegCpRbWptr = 0x01C5;
        constexpr uint32_t kRegCoherStatusHost = 0x0A2F;
        constexpr uint32_t kRegVgtEventInitiator = 0x21F9;
        constexpr uint32_t kRegScratchUmsk = 0x01DC;
        constexpr uint32_t kRegScratchAddr = 0x01DD;
        constexpr uint32_t kRegScratch0 = 0x0578;
        constexpr uint32_t kRegScratch7 = 0x057F;
        constexpr uint32_t kRegRbEdramTiming = 0x0F00;
        constexpr uint32_t kRegRbBcSpares = 0x0F0C;
        constexpr uint32_t kRegVblankStatus = 0x1951;  // bit 0: vblank pending
        // Display controller gamma ramp registers (Xenia's register table).
        constexpr uint32_t kRegDcLutRwMode = 0x1921;     // 0: 256-entry table, 1: piecewise linear
        constexpr uint32_t kRegDcLutRwIndex = 0x1922;    // entry the next writes go to
        constexpr uint32_t kRegDcLutSeqColor = 0x1923;   // table mode data
        constexpr uint32_t kRegDcLutPwlData = 0x1924;    // PWL data: base : 16, delta : 16, one write per channel
        constexpr uint32_t kRegD1ModeViewportSize = 0x1961;
        constexpr uint32_t kRegisterCount = 0x8000;  // size of the register file (dwords)
        // Register indices in REG_RMW / COND_WRITE are 13 bits.
        constexpr uint32_t kPacketRegisterMask = 0x1FFF;

        // WAIT_REG_MEM / COND_WRITE wait info bits.
        constexpr uint32_t kWaitMemory = 0x10;   // poll memory rather than a register
        constexpr uint32_t kWriteMemory = 0x100;  // COND_WRITE: write memory rather than a register
        // Bit 31 of COHER_STATUS_HOST: a coherency request is pending.
        constexpr uint32_t kCoherRequest = 0x80000000u;
        // VGT_EVENT_INITIATOR's event type field.
        constexpr uint32_t kEventTypeMask = 0x3F;

        // The 360's CPU has 6 hardware threads; interrupts target any subset.
        constexpr uint32_t kCpuCount = 6;
        // Vblank interrupts are delivered on this hardware thread (as Xenia).
        constexpr uint32_t kVblankCpu = 2;
        constexpr int kVblankHz = 60;
        // Stack for the guest threads that run interrupt callbacks.
        constexpr uint32_t kInterruptStackSize = 64 * 1024;
        // How long the ring buffer thread sleeps when there are no packets.
        constexpr auto kIdleSleep = std::chrono::microseconds(100);
        // A WAIT_REG_MEM still waiting after this long gets logged once.
        constexpr auto kStuckWaitReport = std::chrono::seconds(1);
        // H3_LOG_WAITS reports every this many swaps, waits over this long.
        constexpr uint32_t kWaitReportInterval = 150;
        constexpr double kWaitReportMinSeconds = 0.01;

        // Type-3 opcodes that matter to the CPU.
        enum : uint32_t
        {
            kOpRegRmw = 0x21,
            kOpIndirectBufferPfd = 0x37,
            kOpWaitRegMem = 0x3C,
            kOpMemWrite = 0x3D,
            kOpIndirectBuffer = 0x3F,
            kOpCondWrite = 0x45,
            kOpInterrupt = 0x54,
            kOpEventWriteShd = 0x58,
            kOpEventWriteExt = 0x5A,  // screen extents (0x59 is EVENT_WRITE_CFL)
            kOpEventWriteZpd = 0x5B,
            kOpSetBinMask = 0x50,
            kOpSetBinSelect = 0x51,
            kOpSetBinMaskLo = 0x60,
            kOpSetBinMaskHi = 0x61,
            kOpSetBinSelectLo = 0x62,
            kOpSetBinSelectHi = 0x63,
            kOpDrawIndx = 0x22,
            kOpImLoad = 0x27,
            kOpImLoadImmediate = 0x2B,
            kOpSetConstant = 0x2D,
            kOpLoadAluConstant = 0x2F,
            kOpDrawIndx2 = 0x36,
            kOpSetConstant2 = 0x55,
            kOpSetShaderConstants = 0x56,
        };

        // SET_CONSTANT / LOAD_ALU_CONSTANT type field: register block.
        constexpr uint32_t kConstantBases[] = { kRegShaderConstantAlu, kRegShaderConstantFetch, kRegShaderConstantBool,
            kRegShaderConstantLoop, kRegContextRegisterBase };

        // Predicated tiling. D3D replays a frame's commands once per EDRAM
        // tile ("bin") and marks per-tile packets as predicated (type-3
        // header bit 0); those only run when bin select & bin mask is
        // nonzero. Skipping this runs every tile's packets on every pass,
        // including D3D's GPU-to-CPU callbacks, which then re-trigger the
        // replay forever. Command processor thread only. Reference: Xenia's
        // ExecutePacketType3.
        uint64_t g_binMask = ~0ull;
        uint64_t g_binSelect = ~0ull;

        // Interrupt sources passed to the graphics interrupt callback.
        constexpr uint32_t kInterruptVblank = 0;
        constexpr uint32_t kInterruptCommandProcessor = 1;

        std::atomic<uint32_t> g_ringBase{ 0 };   // guest address
        std::atomic<uint32_t> g_ringDwords{ 0 };
        std::atomic<uint32_t> g_writeback{ 0 };  // guest address, 0 if disabled
        std::atomic<uint32_t> g_swapCounter{ 0 };
        std::atomic<uint32_t> g_interruptCallback{ 0 };
        std::atomic<uint32_t> g_interruptData{ 0 };


        const bool g_log = EnvFlag("H3_LOG_GPU");

        // H3_LOG_WAITS: time WAIT_REG_MEM spends waiting, per address
        // (memory addresses have bit 31 set), reported every 150 swaps.
        const bool g_logWaits = EnvFlag("H3_LOG_WAITS");
        struct WaitStat
        {
            double seconds = 0;
            uint32_t count = 0;
        };
        std::map<uint32_t, WaitStat> g_waitStats;

        // Prints and resets the H3_LOG_WAITS totals.
        void ReportWaitStats()
        {
            fprintf(stderr, "[gpu waits]");
            for (const auto& [address, stat] : g_waitStats)
                if (stat.seconds > kWaitReportMinSeconds)
                    fprintf(stderr, " %s %08X %.3f s/%u", (address >> 31) ? "mem" : "reg", address & 0x7FFFFFFF, stat.seconds, stat.count);
            fprintf(stderr, "\n");
            g_waitStats.clear();
        }

        // GPU memory writes: the low two bits of the address are the endian
        // mode (xenos.h). As in Xenia, the swapped value is stored in host
        // byte order, so the common 8-in-32 mode leaves a big-endian word for
        // the CPU.
        void WriteMemory(uint32_t address, uint32_t value)
        {
            uint32_t swapped = SwapEndian32(value, address);
            memcpy(GuestPtr<uint8_t>(PhysicalToGuest(address & ~3u)), &swapped, 4);
        }

        // GPU memory read, swapped the same way as WriteMemory.
        uint32_t ReadMemory(uint32_t address)
        {
            uint32_t value;
            memcpy(&value, GuestPtr<uint8_t>(PhysicalToGuest(address & ~3u)), 4);
            return SwapEndian32(value, address);
        }

        // Reads packets from the ring buffer (wrapping) or an indirect
        // buffer (linear), up to `end`.
        struct Reader
        {
            uint32_t base;
            uint32_t size;  // dwords; the ring wraps at this
            uint32_t index;
            uint32_t end;
            bool ring;

            uint32_t Remaining() const { return ring ? (end + size - index) % size : end - index; }

            // The next dword (wrapping in the ring).
            uint32_t Read()
            {
                uint32_t value = GuestRef<uint32_t>(base + index * 4);
                index++;
                if (ring && index == size)
                    index = 0;
                return value;
            }

            // Skips up to count dwords.
            void Skip(uint32_t count)
            {
                count = std::min(count, Remaining());
                index = ring ? (index + count) % size : index + count;
            }
        };

        // WAIT_REG_MEM / COND_WRITE compare functions.
        bool Compare(uint32_t function, uint32_t value, uint32_t ref)
        {
            switch (function & 7)
            {
            case 1: return value < ref;
            case 2: return value <= ref;
            case 3: return value == ref;
            case 4: return value != ref;
            case 5: return value >= ref;
            case 6: return value > ref;
            case 7: return true;
            default: return false;
            }
        }

        // The CPU requests cache coherency by setting bit 31 of
        // COHER_STATUS_HOST and waits for it to clear. Memory is always
        // coherent here.
        void MakeCoherent()
        {
            uint32_t status = ReadRegister(kRegCoherStatusHost);
            if (status & kCoherRequest)
                WriteRegister(kRegCoherStatusHost, status & ~kCoherRequest);
        }

        // The value a WAIT_REG_MEM / COND_WRITE compares: memory or a register.
        uint32_t PollValue(uint32_t waitInfo, uint32_t address)
        {
            if (waitInfo & kWaitMemory)
                return ReadMemory(address);
            if (address == kRegCoherStatusHost)
                MakeCoherent();
            return ReadRegister(address);
        }

        // A guest thread's context for running the interrupt callback: the
        // command processor and the vblank thread each have one.
        struct InterruptContext
        {
            PPCContext ctx{};
            uint64_t stack = 0;
        };

        // Makes the calling host thread a guest thread able to run callbacks.
        void InitInterruptContext(InterruptContext& context)
        {
            kernel::AdoptHostThread(context.ctx, kInterruptStackSize);
            context.stack = context.ctx.r1.u64;
        }

        // Runs the game's graphics interrupt callback on the calling thread.
        void DeliverInterrupt(InterruptContext& context, uint32_t source, uint32_t cpu)
        {
            uint32_t callback = g_interruptCallback.load();
            if (callback == 0)
                return;
            // The callback ignores a vblank unless the status register says
            // one is pending.
            if (source == kInterruptVblank)
                WriteRegister(kRegVblankStatus, ReadRegister(kRegVblankStatus) | 1);
            // The callback acknowledges the interrupt for the CPU it runs on
            // (KPCR current processor), so run it "on" the target CPU.
            *GuestPtr<uint8_t>(kernel::CurrentThread()->pcr + kernel::kPcrCurrentCpu) = uint8_t(cpu);
            PPCContext& ctx = context.ctx;
            ctx.r1.u64 = context.stack;
            ctx.r3.u64 = source;
            ctx.r4.u64 = g_interruptData.load();
            // Like a real interrupt, never in the middle of guest code running
            // at raised IRQL.
            kernel::EnterInterrupt();
            (PPC_LOOKUP_FUNC(g_base, callback))(ctx, g_base);
            kernel::LeaveInterrupt();
        }

        // The command processor's own guest context (see CommandProcessorThread).
        InterruptContext* g_cpInterruptContext = nullptr;

        // H3_LOG_GPU: logs the first time each type-3 opcode is seen.
        void LogOpcode(uint32_t opcode, const char* what)
        {
            static std::mutex mutex;
            static bool seen[128];
            if (!g_log)
                return;
            std::lock_guard lock(mutex);
            if (!seen[opcode & 0x7F])
            {
                seen[opcode & 0x7F] = true;
                fprintf(stderr, "[gpu] first type-3 opcode 0x%02X (%s)\n", opcode, what);
            }
        }

        void Execute(Reader& reader);

        // Register writes from the command stream. Scratch registers whose
        // bit is set in SCRATCH_UMSK are also written back to memory at
        // SCRATCH_ADDR, which is how the CPU sees GPU progress markers.
        // The gamma ramp being written (DC_LUT_* registers) and the last
        // complete one.
        std::mutex g_gammaMutex;
        GammaRamp g_gammaRamp;
        uint32_t g_lutIndex = 0, g_lutComponent = 0;

        // Tracks writes to the gamma ramp registers.
        void WriteGammaRegister(uint32_t index, uint32_t value)
        {
            switch (index)
            {
            case kRegDcLutRwIndex:
                g_lutIndex = value;
                g_lutComponent = 0;
                break;
            case kRegDcLutPwlData:
                if (ReadRegister(kRegDcLutRwMode) == 1 && g_lutIndex < GammaRamp::kSegments)
                {
                    std::lock_guard lock(g_gammaMutex);
                    g_gammaRamp.base[g_lutIndex][g_lutComponent] = uint16_t(value);
                    g_gammaRamp.delta[g_lutIndex][g_lutComponent] = uint16_t(value >> 16);
                    g_gammaRamp.valid = true;
                    // Red, green, blue, then the next entry.
                    if (++g_lutComponent == 3)
                    {
                        g_lutComponent = 0;
                        g_lutIndex++;
                    }
                }
                break;
            case kRegDcLutSeqColor:
                static bool logged = false;
                if (!logged)
                {
                    logged = true;
                    fprintf(stderr, "[gpu] 256-entry gamma table written; not applied (only the piecewise-linear ramp is)\n");
                }
                break;
            default:
                break;
            }
        }

        void CpWriteRegister(uint32_t index, uint32_t value)
        {
            if (index >= kRegDcLutRwIndex && index <= kRegDcLutPwlData)
                WriteGammaRegister(index, value);
            // H3_LOG_REG_WRITES=<first>-<last> (register indices, hex): log
            // command-stream writes to that range (the first 4096).
            static const auto logRange = [] {
                std::pair<uint32_t, uint32_t> range{ 1, 0 };
                if (const char* spec = EnvString("H3_LOG_REG_WRITES"))
                {
                    char* end;
                    range.first = uint32_t(strtoul(spec, &end, 16));
                    range.second = *end == '-' ? uint32_t(strtoul(end + 1, nullptr, 16)) : range.first;
                }
                return range;
            }();
            constexpr uint32_t kMaxLoggedWrites = 4096;
            static uint32_t logged = 0;
            if (index >= logRange.first && index <= logRange.second && logged < kMaxLoggedWrites)
            {
                logged++;
                fprintf(stderr, "[gpu] reg %04X = %08X\n", index, value);
            }
            WriteRegister(index, value);
            if (index >= kRegScratch0 && index <= kRegScratch7)
            {
                uint32_t scratch = index - kRegScratch0;
                if (ReadRegister(kRegScratchUmsk) & (1u << scratch))
                    GuestRef<uint32_t>(PhysicalToGuest(ReadRegister(kRegScratchAddr) + scratch * 4)) = value;
            }
        }

        // Executes one type-3 packet (header: opcode @8, payload dwords - 1
        // @16, predicated @0). Whatever part of the payload a case doesn't
        // read is skipped at the end.
        void ExecuteType3(Reader& reader, uint32_t header)
        {
            uint32_t opcode = (header >> 8) & 0x7F;
            uint32_t count = ((header >> 16) & 0x3FFF) + 1;
            uint32_t start = reader.Remaining();
            auto consumed = [&] { return start - reader.Remaining(); };

            if ((header & 1) && (g_binSelect & g_binMask) == 0)
            {
                capture::OnPredicatedSkip(opcode);
                reader.Skip(count);
                return;
            }

            auto setLow = [](uint64_t& value, uint32_t low) { value = (value & 0xFFFFFFFF00000000ull) | low; };
            auto setHigh = [](uint64_t& value, uint32_t high) { value = (value & 0xFFFFFFFFull) | (uint64_t(high) << 32); };

            switch (opcode)
            {
            case kOpSetBinMask:
            case kOpSetBinSelect:
            {
                uint64_t high = reader.Read();
                uint64_t low = reader.Read();
                (opcode == kOpSetBinMask ? g_binMask : g_binSelect) = (high << 32) | low;
                capture::OnBinState(g_binMask, g_binSelect);
                break;
            }
            case kOpSetBinMaskLo:
                setLow(g_binMask, reader.Read());
                capture::OnBinState(g_binMask, g_binSelect);
                break;
            case kOpSetBinMaskHi:
                setHigh(g_binMask, reader.Read());
                capture::OnBinState(g_binMask, g_binSelect);
                break;
            case kOpSetBinSelectLo:
                setLow(g_binSelect, reader.Read());
                capture::OnBinState(g_binMask, g_binSelect);
                break;
            case kOpSetBinSelectHi:
                setHigh(g_binSelect, reader.Read());
                capture::OnBinState(g_binMask, g_binSelect);
                break;
            case kOpSetConstant:
            {
                // Type in bits 16-23 picks the block; the rest are the values.
                uint32_t offsetType = reader.Read();
                uint32_t type = (offsetType >> 16) & 0xFF;
                if (type >= std::size(kConstantBases))
                    break;
                uint32_t index = kConstantBases[type] + (offsetType & 0x7FF);
                for (uint32_t i = 0; i + 1 < count; i++)
                    CpWriteRegister(index + i, reader.Read());
                break;
            }
            case kOpSetConstant2:
            case kOpSetShaderConstants:
            {
                uint32_t index = reader.Read() & 0xFFFF;
                for (uint32_t i = 0; i + 1 < count; i++)
                    CpWriteRegister(index + i, reader.Read());
                break;
            }
            case kOpLoadAluConstant:
            {
                // The same blocks, loaded from (physical) memory.
                uint32_t address = PhysicalToGuest(reader.Read() & 0x3FFFFFFF);
                uint32_t offsetType = reader.Read();
                uint32_t dwords = reader.Read() & 0xFFF;
                uint32_t type = (offsetType >> 16) & 0xFF;
                if (type >= std::size(kConstantBases))
                    break;
                uint32_t index = kConstantBases[type] + (offsetType & 0x7FF);
                for (uint32_t i = 0; i < dwords; i++)
                    CpWriteRegister(index + i, GuestRef<uint32_t>(address + i * 4));
                break;
            }
            case kOpImLoad:
            {
                // Shader microcode by pointer: type in the low 2 bits.
                uint32_t addressType = reader.Read();
                uint32_t dwords = reader.Read() & 0xFFFF;
                uint32_t address = PhysicalToGuest(addressType & ~3u);
                capture::OnShaderLoad(addressType & 3, address, dwords);
                std::vector<uint32_t> code(dwords);
                for (uint32_t i = 0; i < dwords; i++)
                    code[i] = GuestRef<uint32_t>(address + i * 4);
                renderer::OnShaderLoad(addressType & 3, code.data(), dwords);
                break;
            }
            case kOpImLoadImmediate:
            {
                // Shader microcode embedded in the packet.
                uint32_t type = reader.Read();
                uint32_t dwords = std::min(reader.Read() & 0xFFFF, count - 2);
                uint32_t address = reader.ring ? 0 : reader.base + reader.index * 4;
                std::vector<uint32_t> code(dwords);
                for (uint32_t i = 0; i < dwords; i++)
                    code[i] = reader.Read();
                capture::OnShaderLoad(type & 3, address, dwords);
                renderer::OnShaderLoad(type & 3, code.data(), dwords);
                break;
            }
            case kOpDrawIndx:
            case kOpDrawIndx2:
            {
                // DRAW_INDX starts with a visibility query token.
                if (opcode == kOpDrawIndx)
                    reader.Read();
                uint32_t initiator = reader.Read();
                WriteRegister(kRegVgtDrawInitiator, initiator);
                uint32_t indexBase = 0, dmaSize = 0;
                if (((initiator >> 6) & 3) == 0 && consumed() + 2 <= count)  // DMA (indexed)
                {
                    indexBase = PhysicalToGuest(reader.Read());
                    dmaSize = reader.Read();
                    WriteRegister(kRegVgtDmaBase, indexBase);
                    WriteRegister(kRegVgtDmaSize, dmaSize);
                }
                capture::OnDraw(initiator, indexBase, dmaSize & 0xFFFFFF);
                if ((ReadRegister(kRegRbModeControl) & 7) == kEdramModeCopy)
                    renderer::OnResolve();
                else
                    renderer::OnDraw(initiator, indexBase, dmaSize);
                break;
            }
            case kOpIndirectBuffer:
            case kOpIndirectBufferPfd:
            {
                LogOpcode(opcode, "indirect buffer");
                uint32_t address = reader.Read();
                uint32_t length = reader.Read() & 0xFFFFF;
                Reader indirect{ PhysicalToGuest(address & ~3u), length, 0, length, false };
                Execute(indirect);
                break;
            }
            case kOpWaitRegMem:
            {
                LogOpcode(opcode, "wait reg/mem");
                uint32_t waitInfo = reader.Read();
                uint32_t address = reader.Read();
                uint32_t ref = reader.Read();
                uint32_t mask = reader.Read();
                reader.Read();  // wait interval
                static const bool logDetail = EnvFlag("H3_LOG_WAIT_DETAIL");
                if (logDetail)
                    fprintf(stderr, "[gpu] wait %s %08X func %u ref %08X mask %08X now %08X\n", (waitInfo & kWaitMemory) ? "mem" : "reg", address,
                        waitInfo & 7, ref, mask, PollValue(waitInfo, address));
                auto start = std::chrono::steady_clock::now();
                bool reported = false;
                while (!Compare(waitInfo, PollValue(waitInfo, address) & mask, ref))
                {
                    if (!reported && std::chrono::steady_clock::now() - start > kStuckWaitReport)
                    {
                        fprintf(stderr, "[gpu] stuck waiting on %s 0x%08X: (0x%08X & 0x%08X) func %u ref 0x%08X\n",
                            (waitInfo & kWaitMemory) ? "memory" : "register", address, PollValue(waitInfo, address), mask,
                            waitInfo & 7, ref);
                        // What every guest thread waits on: the value comes
                        // from the CPU (often D3D's interrupt callback
                        // worker), so a stuck GPU is usually a stuck thread.
                        kernel::DumpThreadWaits();
                        reported = true;
                    }
                    // PERF: spins (yielding) until the CPU writes the value;
                    // a wake-up from the guest's register/memory writes isn't
                    // possible with plain memory, but a short sleep after a
                    // while would free the core.
                    std::this_thread::yield();
                }
                if (g_logWaits)
                {
                    WaitStat& stat = g_waitStats[address | ((waitInfo & kWaitMemory) ? 0x80000000u : 0)];
                    stat.seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                    stat.count++;
                }
                break;
            }
            case kOpRegRmw:
            {
                LogOpcode(opcode, "register read-modify-write");
                uint32_t info = reader.Read();
                uint32_t andMask = reader.Read();
                uint32_t orMask = reader.Read();
                // Bits 31/30: the AND/OR operands are registers, not values.
                uint32_t value = ReadRegister(info & kPacketRegisterMask);
                value &= (info >> 31) ? ReadRegister(andMask & kPacketRegisterMask) : andMask;
                value |= ((info >> 30) & 1) ? ReadRegister(orMask & kPacketRegisterMask) : orMask;
                CpWriteRegister(info & kPacketRegisterMask, value);
                break;
            }
            case kOpCondWrite:
            {
                LogOpcode(opcode, "conditional write");
                uint32_t waitInfo = reader.Read();
                uint32_t address = reader.Read();
                uint32_t ref = reader.Read();
                uint32_t mask = reader.Read();
                uint32_t writeAddress = reader.Read();
                uint32_t data = reader.Read();
                if (Compare(waitInfo, PollValue(waitInfo, address) & mask, ref))
                {
                    if (waitInfo & kWriteMemory)
                        WriteMemory(writeAddress, data);
                    else
                        CpWriteRegister(writeAddress, data);
                }
                break;
            }
            case kOpMemWrite:
            {
                LogOpcode(opcode, "memory write");
                uint32_t address = reader.Read();
                for (uint32_t i = 1; i < count; i++, address += 4)
                    WriteMemory(address, reader.Read());
                break;
            }
            case kOpEventWriteShd:
            {
                // Fences: the value, or the swap counter if bit 31 is set.
                LogOpcode(opcode, "event write (shader done)");
                uint32_t initiator = reader.Read();
                uint32_t address = reader.Read();
                uint32_t value = reader.Read();
                WriteRegister(kRegVgtEventInitiator, initiator & kEventTypeMask);
                WriteMemory(address, (initiator >> 31) ? g_swapCounter.load() : value);
                break;
            }
            case kOpEventWriteZpd:
            {
                // Occlusion query snapshot: D3D subtracts the report at a
                // query's start from the one at its end (Total, ZFail, ZPass,
                // StencilFail, each split in A and B lanes, little-endian).
                // Predicated tiling uses these to decide which tiles draw an
                // object, so as in Xenia's fake mode, every interval reports
                // samples passing: a running counter that advances per event.
                LogOpcode(opcode, "event write (z pass)");
                uint32_t initiator = reader.Read();
                WriteRegister(kRegVgtEventInitiator, initiator & kEventTypeMask);
                uint32_t address = ReadRegister(kRegRbSampleCountAddr);
                capture::OnZpd(initiator, address);
                static const bool logZpd = EnvFlag("H3_LOG_ZPD");
                if (logZpd)
                {
                    static uint32_t logged = 0;
                    if (logged++ < 400)
                        fprintf(stderr, "[gpu] zpd swap %u initiator %08X address %08X\n", g_swapCounter.load(), initiator, address);
                }
                if (address == 0)
                    break;
                // The renderer counts real samples (gpu/renderer/queries.cpp).
                if (renderer::OnOcclusionReport(address))
                    break;
                // Rendering off: samples "passing" per event; any positive
                // amount works.
                constexpr uint32_t kFakeSamplesPerEvent = 1000;
                static uint32_t passed = 0;
                passed += kFakeSamplesPerEvent;
                uint32_t counts[8] = { passed - (passed >> 1), passed >> 1, 0, 0, passed - (passed >> 1), passed >> 1, 0, 0 };
                memcpy(GuestPtr<uint8_t>(PhysicalToGuest(address & ~3u)), counts, sizeof(counts));
                break;
            }
            case kOpEventWriteExt:
            {
                // Screen extents, as six 16-bit values; Xenia reports a full
                // 2560x2560 screen.
                LogOpcode(opcode, "event write (extents)");
                uint32_t initiator = reader.Read();
                uint32_t address = reader.Read();
                WriteRegister(kRegVgtEventInitiator, initiator & kEventTypeMask);
                // x min/max, y min/max (in 8-pixel units), z min/max.
                constexpr uint16_t kExtents[] = { 0, 8192 >> 3, 0, 8192 >> 3, 0, 1 };
                for (uint32_t i = 0; i < 6; i++)
                    GuestRef<uint16_t>(PhysicalToGuest(address & ~3u) + i * 2) = kExtents[i];
                break;
            }
            case kOpInterrupt:
            {
                LogOpcode(opcode, "interrupt");
                // Delivered before the next packet runs, as Xenia does: the
                // packets after an interrupt (D3D's GPU-to-CPU callback
                // handshake) expect the handler to have run.
                uint32_t cpuMask = reader.Read();
                for (uint32_t cpu = 0; cpu < kCpuCount; cpu++)
                {
                    if (cpuMask & (1u << cpu))
                        DeliverInterrupt(*g_cpInterruptContext, kInterruptCommandProcessor, cpu);
                }
                break;
            }
            case kOpcodeHostSwap:
            {
                LogOpcode(opcode, "swap");
                uint32_t frame = ++g_swapCounter;
                capture::OnSwap(frame);
                // VdSwap's payload: front buffer (physical), width, height.
                uint32_t frontbuffer = reader.Read();
                uint32_t width = reader.Read();
                uint32_t height = reader.Read();
                renderer::OnSwap(frontbuffer, width, height);
                static uint32_t swaps = 0;
                if (g_logWaits && ++swaps % kWaitReportInterval == 0)
                    ReportWaitStats();
                video::OnFramePresented(frame);
                if (g_log && frame % 60 == 0)
                    fprintf(stderr, "[gpu] %u frames presented\n", frame);
                break;
            }
            default:
                // Draws, state, shader and constant loads: nothing the CPU sees.
                LogOpcode(opcode, "skipped");
                break;
            }

            if (consumed() < count)
                reader.Skip(count - consumed());
        }

        // Executes the next packet of any type.
        void ExecutePacket(Reader& reader)
        {
            uint32_t header = reader.Read();
            switch (header >> 30)
            {
            case 0:  // register writes: count, base index, write-one-register flag
            {
                uint32_t count = ((header >> 16) & 0x3FFF) + 1;
                uint32_t base = header & 0x7FFF;
                bool oneRegister = (header >> 15) & 1;
                for (uint32_t i = 0; i < count && reader.Remaining() > 0; i++)
                    CpWriteRegister(oneRegister ? base : base + i, reader.Read());
                break;
            }
            case 1:  // two register writes
            {
                uint32_t first = header & 0x7FF;
                uint32_t second = (header >> 11) & 0x7FF;
                CpWriteRegister(first, reader.Read());
                CpWriteRegister(second, reader.Read());
                break;
            }
            case 2:  // NOP
                break;
            case 3:
                ExecuteType3(reader, header);
                break;
            }
        }

        // Executes packets until the reader's end.
        void Execute(Reader& reader)
        {
            while (reader.Remaining() > 0)
                ExecutePacket(reader);
        }

        // Tells the CPU how far the ring buffer has been read (register, and
        // the write-back address if D3D enabled one).
        void PublishReadPointer(uint32_t readPointer)
        {
            WriteRegister(kRegCpRbRptr, readPointer);
            if (uint32_t writeback = g_writeback.load())
                GuestRef<uint32_t>(writeback) = readPointer;
        }

        // The command processor thread: executes the ring buffer between the
        // read pointer and the CPU's write pointer, forever.
        void CommandProcessorThread()
        {
            // A guest thread, so it can run the interrupt callback itself.
            static InterruptContext context;
            InitInterruptContext(context);
            g_cpInterruptContext = &context;

            uint32_t readPointer = 0;
            for (;;)
            {
                MakeCoherent();
                uint32_t size = g_ringDwords.load();
                uint32_t writePointer = ReadRegister(kRegCpRbWptr) % size;
                if (writePointer == readPointer)
                {
                    // PERF: polls the write pointer; it's plain memory, so the
                    // CPU's update can't wake this thread directly (hooking
                    // D3D's kick function could).
                    std::this_thread::sleep_for(kIdleSleep);
                    continue;
                }

                // Publish the read pointer after every packet: a packet can
                // wait on the CPU, which may be waiting for ring space.
                Reader reader{ g_ringBase.load(), size, readPointer, writePointer, true };
                while (reader.Remaining() > 0)
                {
                    ExecutePacket(reader);
                    PublishReadPointer(reader.index);
                }
                readPointer = writePointer;
            }
        }

        // Raises the 60 Hz vblank interrupt, on a guest thread of its own.
        // Command processor interrupts run on the command processor thread.
        void InterruptThread()
        {
            InterruptContext context;
            InitInterruptContext(context);

            constexpr auto kVblankPeriod = std::chrono::nanoseconds(1'000'000'000 / kVblankHz);
            auto nextVblank = std::chrono::steady_clock::now() + kVblankPeriod;
            for (;;)
            {
                std::this_thread::sleep_until(nextVblank);
                nextVblank += kVblankPeriod;
                DeliverInterrupt(context, kInterruptVblank, kVblankCpu);
            }
        }
    }

    void Initialize()
    {
        // The values Xenia's graphics system returns for these registers
        // (EDRAM timing is an opaque hardware value D3D checks).
        constexpr uint32_t kEdramTiming = 0x08100748;
        constexpr uint32_t kDisplayWidth = 1280, kDisplayHeight = 720;
        WriteRegister(kRegRbEdramTiming, kEdramTiming);
        WriteRegister(kRegRbBcSpares, 0);
        WriteRegister(kRegVblankStatus, 1);
        WriteRegister(kRegD1ModeViewportSize, (kDisplayWidth << 16) | kDisplayHeight);
    }

    uint32_t MakePacketType3(uint32_t opcode, uint32_t payloadDwords)
    {
        return (3u << 30) | (((payloadDwords - 1) & 0x3FFF) << 16) | ((opcode & 0x7F) << 8);
    }

    GammaRamp CurrentGammaRamp()
    {
        std::lock_guard lock(g_gammaMutex);
        return g_gammaRamp;
    }

    uint32_t ReadRegister(uint32_t index)
    {
        return GuestRef<uint32_t>(kGpuRegisterBase + (index % kRegisterCount) * 4);
    }

    void WriteRegister(uint32_t index, uint32_t value)
    {
        GuestRef<uint32_t>(kGpuRegisterBase + (index % kRegisterCount) * 4) = value;
    }

    void InitializeRingBuffer(uint32_t physicalAddress, uint32_t sizeLog2)
    {
        g_ringBase = PhysicalToGuest(physicalAddress);
        g_ringDwords = (1u << (sizeLog2 + 3)) / 4;
        if (g_log)
            fprintf(stderr, "[gpu] ring buffer at 0x%08X, %u dwords\n", g_ringBase.load(), g_ringDwords.load());

        static std::once_flag started;
        std::call_once(started, [] { std::thread(CommandProcessorThread).detach(); });
    }

    void EnableReadPointerWriteBack(uint32_t physicalAddress, uint32_t blockSizeLog2)
    {
        g_writeback = PhysicalToGuest(physicalAddress);
        if (g_log)
            fprintf(stderr, "[gpu] read pointer write-back to 0x%08X (block 2^%u)\n", g_writeback.load(), blockSizeLog2);
    }

    void SetInterruptCallback(uint32_t callback, uint32_t userData)
    {
        g_interruptData = userData;
        g_interruptCallback = callback;
        if (g_log)
            fprintf(stderr, "[gpu] interrupt callback 0x%08X (data 0x%08X)\n", callback, userData);

        static std::once_flag started;
        std::call_once(started, [] { std::thread(InterruptThread).detach(); });
    }
}
