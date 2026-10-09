#include "command_processor.h"

#include <ppc_recomp_shared.h>

#include "../kernel/thread.h"
#include "../memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <tuple>
#include <utility>

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
            kOpEventWriteExt = 0x59,
        };

        // Interrupt sources passed to the graphics interrupt callback.
        constexpr uint32_t kInterruptVblank = 0;
        constexpr uint32_t kInterruptCommandProcessor = 1;

        std::atomic<uint32_t> g_ringBase{ 0 };   // guest address
        std::atomic<uint32_t> g_ringDwords{ 0 };
        std::atomic<uint32_t> g_writeback{ 0 };  // guest address, 0 if disabled
        std::atomic<uint32_t> g_swapCounter{ 0 };
        std::atomic<uint32_t> g_interruptCallback{ 0 };
        std::atomic<uint32_t> g_interruptData{ 0 };

        std::mutex g_interruptMutex;
        std::condition_variable g_interruptCv;
        std::deque<std::pair<uint32_t, uint32_t>> g_interrupts;  // source, cpu

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

        void QueueInterrupt(uint32_t source, uint32_t cpu)
        {
            std::lock_guard lock(g_interruptMutex);
            g_interrupts.emplace_back(source, cpu);
            g_interruptCv.notify_one();
        }

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

            switch (opcode)
            {
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
            case kOpEventWriteExt:
            {
                // Screen extents, as six 16-bit values; Xenia reports a full
                // 2560x2560 screen.
                LogOpcode(opcode, "event write (extents)");
                uint32_t initiator = reader.Read();
                uint32_t address = reader.Read();
                WriteRegister(kRegVgtEventInitiator, initiator & 0x3F);
                constexpr uint16_t kExtents[] = { 0, 2560 >> 3, 0, 2560 >> 3, 0, 1 };
                for (uint32_t i = 0; i < 6; i++)
                    GuestRef<uint16_t>(PhysicalToGuest(address & ~3u) + i * 2) = kExtents[i];
                break;
            }
            case kOpInterrupt:
            {
                LogOpcode(opcode, "interrupt");
                uint32_t cpuMask = reader.Read();
                for (uint32_t cpu = 0; cpu < 6; cpu++)
                {
                    if (cpuMask & (1u << cpu))
                        QueueInterrupt(kInterruptCommandProcessor, cpu);
                }
                break;
            }
            case kOpcodeHostSwap:
            {
                LogOpcode(opcode, "swap");
                uint32_t frame = ++g_swapCounter;
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

        // Runs the game's graphics interrupt callback for queued interrupts
        // and a 60 Hz vblank, on a guest thread of its own.
        void InterruptThread()
        {
            PPCContext ctx{};
            kernel::AdoptHostThread(ctx, 0x10000);
            uint64_t stack = ctx.r1.u64;

            constexpr auto kVblankPeriod = std::chrono::nanoseconds(1'000'000'000 / 60);
            auto nextVblank = std::chrono::steady_clock::now() + kVblankPeriod;
            for (;;)
            {
                uint32_t source = kInterruptVblank;
                uint32_t cpu = 2;
                {
                    std::unique_lock lock(g_interruptMutex);
                    g_interruptCv.wait_until(lock, nextVblank, [] { return !g_interrupts.empty(); });
                    if (!g_interrupts.empty())
                    {
                        std::tie(source, cpu) = g_interrupts.front();
                        g_interrupts.pop_front();
                    }
                    else
                    {
                        nextVblank += kVblankPeriod;
                    }
                }

                uint32_t callback = g_interruptCallback.load();
                if (callback == 0)
                    continue;
                // The callback ignores a vblank unless the status register
                // says one is pending.
                if (source == kInterruptVblank)
                    WriteRegister(kRegVblankStatus, ReadRegister(kRegVblankStatus) | 1);
                // The callback acknowledges the interrupt for the CPU it runs
                // on (KPCR current processor), so run it "on" the target CPU.
                *GuestPtr<uint8_t>(kernel::CurrentThread()->pcr + kernel::kPcrCurrentCpu) = uint8_t(cpu);
                ctx.r1.u64 = stack;
                ctx.r3.u64 = source;
                ctx.r4.u64 = g_interruptData.load();
                (PPC_LOOKUP_FUNC(g_base, callback))(ctx, g_base);
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
