#include "apu/xma_decoder.h"

#include "memory.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <tuple>

// A port of Xenia's XmaContext (src/xenia/apu/xma_context.cc): packets are
// split into frames here (frames span packets), and each frame goes to the
// xmaframes decoder on its own.
namespace h3::apu
{
    namespace
    {
        constexpr uint32_t kContextCount = 320;
        constexpr uint32_t kRegisterBase = 0x7FEA0000;
        constexpr uint32_t kRegContextArrayAddress = 0x0600;

        constexpr uint32_t kBytesPerPacket = 2048;
        constexpr uint32_t kBitsPerPacket = kBytesPerPacket * 8;
        constexpr uint32_t kBitsPerHeader = 33;
        constexpr uint32_t kBytesPerSample = 2;
        constexpr uint32_t kSamplesPerFrame = 512;
        constexpr uint32_t kSamplesPerSubframe = 128;
        constexpr uint32_t kBytesPerFrameChannel = kSamplesPerFrame * kBytesPerSample;
        constexpr uint32_t kBytesPerSubframeChannel = kSamplesPerSubframe * kBytesPerSample;
        constexpr uint32_t kMaxFrameLength = 0x7FFF;

        uint8_t* Physical(uint32_t address) { return g_base + kPhysicalMemoryBase + (address & 0x1FFFFFFF); }

        // XMA_CONTEXT_DATA: 16 big-endian dwords; the bit fields below are
        // laid out for the dwords after byte swapping (LSB first).
        struct ContextData
        {
            // DWORD 0
            uint32_t input_buffer_0_packet_count : 12;
            uint32_t loop_count : 8;
            uint32_t input_buffer_0_valid : 1;
            uint32_t input_buffer_1_valid : 1;
            uint32_t output_buffer_block_count : 5;
            uint32_t output_buffer_write_offset : 5;
            // DWORD 1
            uint32_t input_buffer_1_packet_count : 12;
            uint32_t loop_subframe_start : 2;
            uint32_t loop_subframe_end : 3;
            uint32_t loop_subframe_skip : 3;
            uint32_t subframe_decode_count : 4;
            uint32_t subframe_skip_count : 3;
            uint32_t sample_rate : 2;
            uint32_t is_stereo : 1;
            uint32_t unk_dword_1_c : 1;
            uint32_t output_buffer_valid : 1;
            // DWORD 2
            uint32_t input_buffer_read_offset : 26;
            uint32_t unk_dword_2 : 6;
            // DWORD 3
            uint32_t loop_start : 26;
            uint32_t unk_dword_3 : 6;
            // DWORD 4
            uint32_t loop_end : 26;
            uint32_t packet_metadata : 5;
            uint32_t current_buffer : 1;
            // DWORDs 5-8
            uint32_t input_buffer_0_ptr;
            uint32_t input_buffer_1_ptr;
            uint32_t output_buffer_ptr;
            uint32_t work_buffer_ptr;
            // DWORD 9
            uint32_t output_buffer_read_offset : 5;
            uint32_t : 25;
            uint32_t stop_when_done : 1;
            uint32_t interrupt_when_done : 1;
            // DWORDs 10-15
            uint32_t unk_dwords_10_15[6];

            void Load(const uint8_t* guest)
            {
                uint32_t* words = reinterpret_cast<uint32_t*>(this);
                for (uint32_t i = 0; i < 16; i++)
                {
                    uint32_t word;
                    std::memcpy(&word, guest + i * 4, 4);
                    words[i] = __builtin_bswap32(word);
                }
            }
            void Store(uint8_t* guest) const
            {
                const uint32_t* words = reinterpret_cast<const uint32_t*>(this);
                for (uint32_t i = 0; i < 16; i++)
                {
                    uint32_t word = __builtin_bswap32(words[i]);
                    std::memcpy(guest + i * 4, &word, 4);
                }
            }
        };
        static_assert(sizeof(ContextData) == 64);

        // Xenia's BitStream: big-endian bit order.
        class BitStream
        {
        public:
            BitStream(uint8_t* buffer, size_t sizeBits) : m_buffer(buffer), m_sizeBits(sizeBits) {}
            void SetOffset(size_t offset) { m_offset = std::min(offset, m_sizeBits); }
            size_t Offset() const { return m_offset; }
            size_t BitsRemaining() const { return m_sizeBits - m_offset; }
            uint64_t Peek(size_t bits) const
            {
                size_t bytes = m_offset >> 3, rel = m_offset & 7;
                uint64_t value;
                std::memcpy(&value, m_buffer + bytes, 8);
                value = __builtin_bswap64(value);
                value >>= 64 - (rel + bits);
                return value & ((1ull << bits) - 1);
            }
            uint64_t Read(size_t bits)
            {
                uint64_t value = Peek(bits);
                Advance(bits);
                return value;
            }
            void Advance(size_t bits) { SetOffset(m_offset + bits); }
            // Copies bits keeping their position within a byte; returns it.
            size_t Copy(uint8_t* dest, size_t bits)
            {
                size_t bytes = m_offset >> 3, rel = m_offset & 7;
                size_t left = bits, out = 0;
                if (rel)
                {
                    uint64_t value = Peek(8 - rel);
                    dest[out] &= uint8_t(~((1u << rel) - 1));
                    dest[out] |= uint8_t(value);
                    left -= 8 - rel;
                    Advance(8 - rel);
                    out++;
                }
                if (left >= 8)
                {
                    std::memcpy(dest + out, m_buffer + bytes + out, left / 8);
                    out += left / 8;
                    Advance((left / 8) * 8);
                    left -= (left / 8) * 8;
                }
                if (left)
                {
                    uint64_t value = Peek(left) << (8 - left);
                    dest[out] &= uint8_t((1u << (8 - left)) - 1);
                    dest[out] |= uint8_t(value);
                    Advance(left);
                }
                return rel;
            }

        private:
            uint8_t* m_buffer;
            size_t m_sizeBits;
            size_t m_offset = 0;
        };

        // The output ring (Xenia's RingBuffer): read == write means empty.
        struct Ring
        {
            uint8_t* buffer;
            uint32_t capacity, read, write;
            uint32_t WriteCount() const
            {
                if (read == write)
                    return capacity;
                return write < read ? read - write : capacity - write + read;
            }
            void Write(const uint8_t* data, uint32_t count)
            {
                uint32_t first = std::min(count, capacity - write);
                std::memcpy(buffer + write, data, first);
                std::memcpy(buffer, data + first, count - first);
                write = (write + count) % capacity;
            }
        };

        uint32_t PacketFrameOffset(const uint8_t* packet)
        {
            return uint32_t(((packet[0] & 3) << 13) | (packet[1] << 5) | (packet[2] >> 3)) + 32;
        }
        uint32_t PacketSkipCount(const uint8_t* packet) { return packet[3]; }

        int SampleRate(uint32_t id)
        {
            static constexpr int kRates[] = { 24000, 32000, 44100, 48000 };
            return kRates[id & 3];
        }

        class Context
        {
        public:
            uint32_t id = 0;
            uint32_t guest = 0;  // guest address of the context data
            std::mutex lock;
            bool allocated = false;
            bool enabled = false;

            bool Work()
            {
                std::lock_guard guard(lock);
                if (!allocated || !enabled)
                    return false;
                enabled = false;
                uint8_t* pointer = g_base + guest;
                ContextData data;
                data.Load(pointer);
                Decode(data);
                data.Store(pointer);
                return true;
            }

            void Enable()
            {
                std::lock_guard guard(lock);
                enabled = true;
            }

            void Disable()
            {
                std::lock_guard guard(lock);
                enabled = false;
            }

            void Release()
            {
                std::lock_guard guard(lock);
                allocated = false;
                enabled = false;
                std::memset(g_base + guest, 0, sizeof(ContextData));
                ResetDecoder();
            }

        private:
            AVCodecContext* m_codec = nullptr;
            AVPacket* m_packet = nullptr;
            AVFrame* m_frame = nullptr;
            int m_sampleRate = 0, m_channels = 0;

            uint32_t m_packetsSkip = 0;
            uint32_t m_splitFrameLen = 0;
            uint32_t m_splitFrameLenPartial = 0;
            uint8_t m_splitFramePaddingStart = 0;
            std::array<uint8_t, 1 + 4096> m_frameData{};
            std::array<uint8_t, kBytesPerFrameChannel * 2> m_rawFrame{};

            void ResetDecoder()
            {
                m_packetsSkip = 0;
                m_splitFrameLen = m_splitFrameLenPartial = 0;
                m_splitFramePaddingStart = 0;
            }

            bool PrepareDecoder(uint32_t sampleRateId, bool stereo)
            {
                int rate = SampleRate(sampleRateId), channels = stereo ? 2 : 1;
                if (m_codec && rate == m_sampleRate && channels == m_channels)
                    return true;
                avcodec_free_context(&m_codec);
                static const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
                if (!codec)
                    return false;
                m_codec = avcodec_alloc_context3(codec);
                m_codec->sample_rate = rate;
                av_channel_layout_default(&m_codec->ch_layout, channels);
                if (avcodec_open2(m_codec, codec, nullptr) < 0)
                {
                    avcodec_free_context(&m_codec);
                    return false;
                }
                if (!m_packet)
                    m_packet = av_packet_alloc();
                if (!m_frame)
                    m_frame = av_frame_alloc();
                m_sampleRate = rate;
                m_channels = channels;
                return true;
            }

            static void SwapInputBuffer(ContextData& data)
            {
                if (data.current_buffer == 0)
                    data.input_buffer_0_valid = 0;
                else
                    data.input_buffer_1_valid = 0;
                data.current_buffer ^= 1;
                data.input_buffer_read_offset = 0;
            }

            static bool TrySetupNextLoop(ContextData& data, bool ignoreInputBufferOffset)
            {
                if (data.loop_count > 0 && data.loop_start < data.loop_end &&
                    (ignoreInputBufferOffset || data.input_buffer_read_offset >= data.loop_end))
                {
                    data.input_buffer_read_offset = data.loop_start;
                    if (data.loop_count < 255)
                        data.loop_count--;
                    return true;
                }
                return false;
            }

            static int FramePacketNumber(size_t size, size_t bitOffset)
            {
                if (bitOffset >= size * 8)
                    return -1;
                return int((bitOffset >> 3) / kBytesPerPacket);
            }

            static bool ValidFrameOffset(uint8_t* block, size_t size, size_t frameOffset)
            {
                int packetNumber = FramePacketNumber(size, frameOffset);
                if (packetNumber < 0)
                    return false;
                uint8_t* packet = block + packetNumber * kBytesPerPacket;
                size_t relative = frameOffset % kBitsPerPacket;
                uint32_t first = PacketFrameOffset(packet);
                if (first > kBitsPerPacket)
                    return false;
                BitStream stream(packet, kBitsPerPacket);
                stream.SetOffset(first);
                for (;;)
                {
                    if (stream.Offset() == relative)
                        return true;
                    if (stream.BitsRemaining() < 15)
                        return false;
                    uint64_t length = stream.Read(15);
                    if (length - 15 > stream.BitsRemaining() || length == 0x7FFF)
                        return false;
                    stream.Advance(length - 16);
                    if (stream.Read(1) == 0)
                        return false;
                }
            }

            static size_t NextFrame(uint8_t* block, size_t size, size_t bitOffset)
            {
                int packetIndex = FramePacketNumber(size, bitOffset);
                BitStream stream(block, size * 8);
                stream.SetOffset(bitOffset);
                if (stream.BitsRemaining() < 15)
                    return 0;
                uint64_t length = stream.Read(15);
                if (length - 15 > stream.BitsRemaining() || length >= kMaxFrameLength)
                    return 0;
                stream.Advance(length - 16);
                if (stream.Read(1) == 0)
                    return 0;
                bitOffset += length;
                if (packetIndex < FramePacketNumber(size, bitOffset))
                    return 0;
                return bitOffset;
            }

            static std::tuple<int, int> FrameNumber(uint8_t* block, size_t size, size_t bitOffset)
            {
                int packetIndex = FramePacketNumber(size, bitOffset);
                if (packetIndex < 0 || (packetIndex + 1) * kBytesPerPacket > size)
                    return { packetIndex, -2 };
                if (bitOffset == 0)
                    return { packetIndex, -1 };
                uint8_t* packet = block + packetIndex * kBytesPerPacket;
                BitStream stream(block, size * 8);
                stream.SetOffset(packetIndex * kBitsPerPacket + PacketFrameOffset(packet));
                int frameIndex = 0;
                for (;;)
                {
                    if (stream.BitsRemaining() < 15 || stream.Offset() == bitOffset)
                        break;
                    uint64_t length = stream.Read(15);
                    if (length - 15 > stream.BitsRemaining() || length == 0x7FFF)
                        break;
                    stream.Advance(length - 16);
                    if (stream.Read(1) == 0)
                        break;
                    frameIndex++;
                }
                return { packetIndex, frameIndex };
            }

            // Frames in the packet (counting a truncated last one), and
            // whether the last continues in a later packet.
            static std::tuple<int, bool> PacketFrameCount(uint8_t* packet)
            {
                uint32_t first = PacketFrameOffset(packet);
                if (first > kBitsPerPacket - kBitsPerHeader)
                    return { 0, false };
                BitStream stream(packet, kBitsPerPacket);
                stream.SetOffset(first);
                int count = 0;
                for (;;)
                {
                    count++;
                    if (stream.BitsRemaining() < 15)
                        return { count, true };
                    uint64_t length = stream.Read(15);
                    if (length - 15 > stream.BitsRemaining() || length == 0x7FFF)
                        return { count, true };
                    stream.Advance(length - 16);
                    if (stream.Read(1) == 0)
                        return { count, false };
                }
            }

            // Planar floats to big-endian 16-bit, interleaved when stereo.
            void ConvertFrame(bool stereo)
            {
                auto out = reinterpret_cast<uint16_t*>(m_rawFrame.data());
                uint32_t channels = stereo ? 2 : 1;
                for (uint32_t c = 0; c < channels; c++)
                {
                    const float* in = reinterpret_cast<const float*>(m_frame->data[c]);
                    for (uint32_t i = 0; i < kSamplesPerFrame; i++)
                    {
                        float value = std::clamp(in[i] * 32767.0f, -32768.0f, 32767.0f);
                        out[i * channels + c] = __builtin_bswap16(uint16_t(int16_t(std::lrintf(value))));
                    }
                }
            }

            // Ends the current input buffer: loop back if looping, else move
            // to the other buffer.
            static void EndOfBuffer(ContextData& data, bool& reuseInputBuffer)
            {
                if (!reuseInputBuffer)
                    reuseInputBuffer = TrySetupNextLoop(data, true);
                if (!reuseInputBuffer)
                    SwapInputBuffer(data);
            }

            void Decode(ContextData& data)
            {
                if (!data.output_buffer_valid)
                    return;
                if (!data.input_buffer_0_valid && !data.input_buffer_1_valid)
                {
                    data.output_buffer_valid = 0;
                    return;
                }

                uint8_t* in0 = data.input_buffer_0_valid ? Physical(data.input_buffer_0_ptr) : nullptr;
                uint8_t* in1 = data.input_buffer_1_valid ? Physical(data.input_buffer_1_ptr) : nullptr;
                uint8_t* input = data.current_buffer ? in1 : in0;
                size_t size0 = size_t(data.input_buffer_0_packet_count) * kBytesPerPacket;
                size_t size1 = size_t(data.input_buffer_1_packet_count) * kBytesPerPacket;
                size_t inputSize = data.current_buffer ? size1 : size0;
                size_t inputPackets = inputSize / kBytesPerPacket;
                if (!input || inputSize == 0)
                {
                    SwapInputBuffer(data);
                    return;
                }

                Ring output{ Physical(data.output_buffer_ptr), data.output_buffer_block_count * kBytesPerSubframeChannel,
                    data.output_buffer_read_offset * kBytesPerSubframeChannel,
                    data.output_buffer_write_offset * kBytesPerSubframeChannel };
                if (output.capacity == 0)
                    return;
                uint32_t outputRemaining = output.WriteCount();
                outputRemaining -= outputRemaining % (kBytesPerFrameChannel << data.is_stereo);

                bool reuseInputBuffer = false;
                while (outputRemaining > 0)
                {
                    if (!data.input_buffer_0_valid && !data.input_buffer_1_valid)
                        break;
                    reuseInputBuffer = TrySetupNextLoop(data, false);

                    int packetIndex, frameIndex, frameCount;
                    bool frameLastSplit;
                    uint8_t* packet;
                    BitStream stream(input, inputSize * 8);
                    stream.SetOffset(data.input_buffer_read_offset);

                    if (m_packetsSkip > 0)
                    {
                        packetIndex = FramePacketNumber(inputSize, data.input_buffer_read_offset);
                        while (m_packetsSkip > 0)
                        {
                            m_packetsSkip--;
                            packetIndex++;
                            if (packetIndex >= int(inputPackets))
                            {
                                EndOfBuffer(data, reuseInputBuffer);
                                return;
                            }
                        }
                        data.input_buffer_read_offset = packetIndex * kBitsPerPacket;
                    }

                    if (m_splitFrameLen)
                    {
                        // The rest of a frame that began in an earlier packet.
                        packetIndex = FramePacketNumber(inputSize, data.input_buffer_read_offset);
                        packet = input + packetIndex * kBytesPerPacket;
                        std::tie(frameCount, frameLastSplit) = PacketFrameCount(packet);
                        frameIndex = -1;
                        stream = BitStream(input, (packetIndex + 1) * kBitsPerPacket);
                        stream.SetOffset(packetIndex * kBitsPerPacket + 32);
                        if (m_splitFrameLen > kMaxFrameLength)
                        {
                            // Even the length was split: finish reading it.
                            size_t offset = stream.Offset();
                            stream.Copy(m_frameData.data() + 1 + ((m_splitFrameLenPartial + m_splitFramePaddingStart) / 8),
                                15 - m_splitFrameLenPartial);
                            stream.SetOffset(offset);
                            BitStream length(m_frameData.data() + 1, 15 + m_splitFramePaddingStart);
                            length.Advance(m_splitFramePaddingStart);
                            m_splitFrameLen = uint32_t(length.Read(15));
                        }
                        stream.Copy(m_frameData.data() + 1 + ((m_splitFrameLenPartial + m_splitFramePaddingStart) / 8),
                            m_splitFrameLen - m_splitFrameLenPartial);
                    }
                    else
                    {
                        if (data.input_buffer_read_offset % kBitsPerPacket == 0)
                        {
                            // At a packet start: go to its first frame.
                            int packetNumber = FramePacketNumber(inputSize, data.input_buffer_read_offset);
                            if (packetNumber < 0)
                                return;
                            data.input_buffer_read_offset +=
                                PacketFrameOffset(input + kBytesPerPacket * packetNumber);
                        }
                        if (!ValidFrameOffset(input, inputSize, data.input_buffer_read_offset))
                        {
                            SwapInputBuffer(data);
                            return;
                        }
                        std::tie(packetIndex, frameIndex) = FrameNumber(input, inputSize, data.input_buffer_read_offset);
                        if (packetIndex < 0 || frameIndex < 0)
                        {
                            SwapInputBuffer(data);
                            return;
                        }
                        packet = input + packetIndex * kBytesPerPacket;
                        std::tie(frameCount, frameLastSplit) = PacketFrameCount(packet);
                        if (!PrepareDecoder(data.sample_rate, data.is_stereo))
                        {
                            data.output_buffer_valid = 0;
                            return;
                        }
                        bool frameIsSplit = frameLastSplit && frameIndex >= frameCount - 1;

                        stream = BitStream(input, (packetIndex + 1) * kBitsPerPacket);
                        stream.SetOffset(data.input_buffer_read_offset);
                        m_splitFrameLenPartial = uint32_t(stream.BitsRemaining());
                        m_splitFrameLen = m_splitFrameLenPartial >= 15 ? uint32_t(stream.Peek(15)) : kMaxFrameLength + 1;
                        m_frameData.fill(0);
                        m_splitFramePaddingStart =
                            uint8_t(stream.Copy(m_frameData.data() + 1, std::min(m_splitFrameLen, m_splitFrameLenPartial)));

                        if (frameIsSplit)
                        {
                            // Continue in the next packet that belongs to
                            // this stream.
                            m_packetsSkip = PacketSkipCount(packet) + 1;
                            while (m_packetsSkip > 0)
                            {
                                m_packetsSkip--;
                                packetIndex++;
                                if (packetIndex >= int(inputPackets))
                                {
                                    EndOfBuffer(data, reuseInputBuffer);
                                    return;
                                }
                            }
                            data.input_buffer_read_offset = packetIndex * kBitsPerPacket;
                            continue;
                        }
                    }

                    // One whole frame in m_frameData, after a padding byte.
                    uint32_t bits = m_splitFramePaddingStart + m_splitFrameLen;
                    m_packet->data = m_frameData.data();
                    m_packet->size = int(1 + bits / 8 + (bits % 8 ? 1 : 0));
                    uint32_t paddingEnd = uint32_t(m_packet->size) * 8 - (8 + bits);
                    m_frameData[0] = uint8_t(((m_splitFramePaddingStart & 7) << 5) | ((paddingEnd & 7) << 2));
                    m_splitFrameLen = m_splitFrameLenPartial = 0;
                    m_splitFramePaddingStart = 0;

                    if (!m_codec || avcodec_send_packet(m_codec, m_packet) < 0 || avcodec_receive_frame(m_codec, m_frame) < 0)
                    {
                        // A bad frame: drop the rest of this buffer.
                        SwapInputBuffer(data);
                        break;
                    }
                    ConvertFrame(data.is_stereo);
                    uint32_t byteCount = kBytesPerFrameChannel << data.is_stereo;
                    output.Write(m_rawFrame.data(), byteCount);
                    outputRemaining -= byteCount;
                    data.output_buffer_write_offset = output.write / 256;

                    uint32_t offset = uint32_t(NextFrame(input, inputSize, data.input_buffer_read_offset));
                    if (frameIndex + 1 >= frameCount)
                    {
                        // Past the packet's last frame: next packet of this
                        // stream.
                        m_packetsSkip = PacketSkipCount(packet) + 1;
                        while (m_packetsSkip > 0)
                        {
                            m_packetsSkip--;
                            packetIndex++;
                            if (packetIndex >= int(inputPackets))
                            {
                                EndOfBuffer(data, reuseInputBuffer);
                                return;
                            }
                        }
                        packet = input + packetIndex * kBytesPerPacket;
                        offset = PacketFrameOffset(packet) + packetIndex * kBitsPerPacket;
                    }
                    if (offset == 0 || frameIndex == -1)
                    {
                        if (packetIndex >= int(inputPackets))
                        {
                            EndOfBuffer(data, reuseInputBuffer);
                            break;
                        }
                        offset = PacketFrameOffset(packet) + packetIndex * kBitsPerPacket;
                    }
                    data.input_buffer_read_offset = offset;
                }

                if (output.write == output.read)
                    data.output_buffer_valid = 0;
            }
        };

        Context g_contexts[kContextCount];
        uint32_t g_arrayBase = 0;  // guest address of context 0
        std::mutex g_allocMutex;
        std::mutex g_wakeMutex;
        std::condition_variable g_wake;
        std::atomic<bool> g_pending{ false };

        void Worker()
        {
            for (;;)
            {
                bool didWork = false;
                for (Context& context : g_contexts)
                    didWork = context.Work() || didWork;
                if (!didWork)
                {
                    std::unique_lock lock(g_wakeMutex);
                    g_wake.wait_for(lock, std::chrono::milliseconds(5), [] { return g_pending.load(); });
                    g_pending = false;
                }
            }
        }
    }

    void InitXma()
    {
        // 256-aligned physical memory for the context array.
        g_arrayBase = PhysicalHeap().Alloc(kContextCount * sizeof(ContextData), 0x1000);
        if (!g_arrayBase)
        {
            fprintf(stderr, "[xma] can't allocate the context array\n");
            return;
        }
        std::memset(g_base + g_arrayBase, 0, kContextCount * sizeof(ContextData));
        for (uint32_t i = 0; i < kContextCount; i++)
        {
            g_contexts[i].id = i;
            g_contexts[i].guest = g_arrayBase + i * sizeof(ContextData);
        }
        // The game reads the registers byte-reversed (lwbrx): store them in
        // host order.
        uint32_t physical = g_arrayBase & 0x1FFFFFFF;
        std::memcpy(g_base + kRegisterBase + kRegContextArrayAddress * 4, &physical, 4);
        std::thread(Worker).detach();
    }

    uint32_t AllocateXmaContext()
    {
        std::lock_guard lock(g_allocMutex);
        for (Context& context : g_contexts)
        {
            std::lock_guard guard(context.lock);
            if (!context.allocated)
            {
                context.allocated = true;
                context.enabled = false;
                return context.guest;
            }
        }
        return 0;
    }

    void ReleaseXmaContext(uint32_t guestAddress)
    {
        if (guestAddress < g_arrayBase || guestAddress >= g_arrayBase + kContextCount * sizeof(ContextData))
            return;
        g_contexts[(guestAddress - g_arrayBase) / sizeof(ContextData)].Release();
    }

    void EnableXmaContext(uint32_t id)
    {
        if (id >= kContextCount)
            return;
        g_contexts[id].Enable();
        g_pending = true;
        g_wake.notify_one();
    }

    void DisableXmaContext(uint32_t id)
    {
        if (id < kContextCount)
            g_contexts[id].Disable();
    }
}
