#include "command_processor.h"

#include <ppc_recomp_shared.h>

#include "../kernel/thread.h"
#include "../memory.h"
#include "../video/window.h"
#include "frame_capture.h"
#include "renderer.h"
#include "registers.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
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
        constexpr uint32_t kRegD1ModeViewportSize = 0x1961;
        constexpr uint32_t kRegisterCount = 0x8000;

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


        const bool g_log = getenv("H3_LOG_GPU") != nullptr;

        // GPU endian modes, in the low two bits of addresses the GPU writes.
        uint32_t GpuSwap(uint32_t value, uint32_t endian)
        {
            switch (endian & 3)
            {
            case 1: return ((value & 0x00FF00FF) << 8) | ((value >> 8) & 0x00FF00FF);  // 8-in-16
            case 2: return ByteSwap(value);                                                // 8-in-32
            case 3: return (value << 16) | (value >> 16);                                 // 16-in-32
            default: return value;
            }
        }

        // As in Xenia: the swapped value is stored in host byte order, so
        // the common 8-in-32 mode leaves a big-endian word for the CPU.
        void WriteMemory(uint32_t address, uint32_t value)
        {
            uint32_t swapped = GpuSwap(value, address);
            memcpy(GuestPtr<uint8_t>(PhysicalToGuest(address & ~3u)), &swapped, 4);
        }

        uint32_t ReadMemory(uint32_t address)
        {
            uint32_t value;
            memcpy(&value, GuestPtr<uint8_t>(PhysicalToGuest(address & ~3u)), 4);
            return GpuSwap(value, address);
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

            uint32_t Read()
            {
                uint32_t value = GuestRef<uint32_t>(base + index * 4);
                index++;
                if (ring && index == size)
                    index = 0;
                return value;
            }

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
            if (status & 0x80000000)
                WriteRegister(kRegCoherStatusHost, status & ~0x80000000u);
        }

        uint32_t PollValue(uint32_t waitInfo, uint32_t address)
        {
            if (waitInfo & 0x10)
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

        void InitInterruptContext(InterruptContext& context)
        {
            kernel::AdoptHostThread(context.ctx, 0x10000);
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
        void CpWriteRegister(uint32_t index, uint32_t value)
        {
            WriteRegister(index, value);
            if (index >= kRegScratch0 && index <= kRegScratch7)
            {
                uint32_t scratch = index - kRegScratch0;
                if (ReadRegister(kRegScratchUmsk) & (1u << scratch))
                    GuestRef<uint32_t>(PhysicalToGuest(ReadRegister(kRegScratchAddr) + scratch * 4)) = value;
            }
        }

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
                auto start = std::chrono::steady_clock::now();
                bool reported = false;
                while (!Compare(waitInfo, PollValue(waitInfo, address) & mask, ref))
                {
                    if (!reported && std::chrono::steady_clock::now() - start > std::chrono::seconds(1))
                    {
                        fprintf(stderr, "[gpu] stuck waiting on %s 0x%08X: (0x%08X & 0x%08X) func %u ref 0x%08X\n",
                            (waitInfo & 0x10) ? "memory" : "register", address, PollValue(waitInfo, address), mask,
                            waitInfo & 7, ref);
                        reported = true;
                    }
                    std::this_thread::yield();
                }
                break;
            }
            case kOpRegRmw:
            {
                LogOpcode(opcode, "register read-modify-write");
                uint32_t info = reader.Read();
                uint32_t andMask = reader.Read();
                uint32_t orMask = reader.Read();
                uint32_t value = ReadRegister(info & 0x1FFF);
                value &= (info >> 31) ? ReadRegister(andMask & 0x1FFF) : andMask;
                value |= ((info >> 30) & 1) ? ReadRegister(orMask & 0x1FFF) : orMask;
                CpWriteRegister(info & 0x1FFF, value);
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
                    if (waitInfo & 0x100)
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
                WriteRegister(kRegVgtEventInitiator, initiator & 0x3F);
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
                WriteRegister(kRegVgtEventInitiator, initiator & 0x3F);
                uint32_t address = ReadRegister(kRegRbSampleCountAddr);
                if (address == 0)
                    break;
                static uint32_t passed = 0;
                passed += 1000;
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
                WriteRegister(kRegVgtEventInitiator, initiator & 0x3F);
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
                for (uint32_t cpu = 0; cpu < 6; cpu++)
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

        void Execute(Reader& reader)
        {
            while (reader.Remaining() > 0)
                ExecutePacket(reader);
        }

        void PublishReadPointer(uint32_t readPointer)
        {
            WriteRegister(kRegCpRbRptr, readPointer);
            if (uint32_t writeback = g_writeback.load())
                GuestRef<uint32_t>(writeback) = readPointer;
        }

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
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
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

            constexpr auto kVblankPeriod = std::chrono::nanoseconds(1'000'000'000 / 60);
            auto nextVblank = std::chrono::steady_clock::now() + kVblankPeriod;
            for (;;)
            {
                std::this_thread::sleep_until(nextVblank);
                nextVblank += kVblankPeriod;
                DeliverInterrupt(context, kInterruptVblank, 2);
            }
        }
    }

    void Initialize()
    {
        // The values Xenia's graphics system returns for these registers.
        WriteRegister(kRegRbEdramTiming, 0x08100748);
        WriteRegister(kRegRbBcSpares, 0);
        WriteRegister(kRegVblankStatus, 1);
        WriteRegister(kRegD1ModeViewportSize, (1280 << 16) | 720);
    }

    uint32_t MakePacketType3(uint32_t opcode, uint32_t payloadDwords)
    {
        return (3u << 30) | (((payloadDwords - 1) & 0x3FFF) << 16) | ((opcode & 0x7F) << 8);
    }

    uint32_t PhysicalToGuest(uint32_t address)
    {
        return kPhysicalMemoryBase + (address & 0x1FFFFFFF);
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
