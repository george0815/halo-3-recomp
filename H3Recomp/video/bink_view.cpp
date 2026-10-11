// Finds the movie player's Bink frame buffers in guest memory (bink_view.h).

#include "video/bink_view.h"

#include "memory.h"

namespace h3::video
{
    namespace
    {
        // The movie player's BINKFRAMEBUFFERS in default.xex's .data (found
        // by searching memory for the intro's 1280x960 / 640x480 sizes):
        //   TotalFrames, YABufferWidth, YABufferHeight, cRcBBufferWidth,
        //   cRcBBufferHeight, FrameNum, then per frame buffer set four
        //   BINKPLANEs (Y, cR, cB, A) of { Allocate, Buffer, BufferPitch }.
        constexpr uint32_t kFrameBuffers = 0x829E7C9C;
        constexpr uint32_t kPlaneSetOffset = 24;
        constexpr uint32_t kPlaneSetSize = 48;
        constexpr uint32_t kPlaneSize = 12;
        constexpr uint32_t kMaxSets = 2;

        constexpr uint32_t kPhysicalEnd = 0xC0000000;

        // A big-endian word of guest memory.
    uint32_t Read(uint32_t address)
        {
            return GuestRef<uint32_t>(address);
        }

        // A plane buffer must lie inside physical memory.
        bool ValidPlane(uint32_t buffer, uint32_t pitch, uint32_t rows)
        {
            return buffer >= kPhysicalMemoryBase && pitch != 0 && uint64_t(buffer) + uint64_t(pitch) * rows <= kPhysicalEnd;
        }
    }

    bool GetBinkFrame(BinkFrame& frame)
    {
        uint32_t sets = Read(kFrameBuffers);
        uint32_t width = Read(kFrameBuffers + 4);
        uint32_t height = Read(kFrameBuffers + 8);
        uint32_t chromaWidth = Read(kFrameBuffers + 12);
        uint32_t chromaHeight = Read(kFrameBuffers + 16);
        uint32_t current = Read(kFrameBuffers + 20);
        if (sets == 0 || sets > kMaxSets || current >= sets || width == 0 || width > 4096 || height == 0 ||
            height > 4096 || chromaWidth != width / 2 || chromaHeight != height / 2)
            return false;

        uint32_t set = kFrameBuffers + kPlaneSetOffset + current * kPlaneSetSize;
        uint32_t y = Read(set + 4), yPitch = Read(set + 8);
        uint32_t cr = Read(set + kPlaneSize + 4), crPitch = Read(set + kPlaneSize + 8);
        uint32_t cb = Read(set + kPlaneSize * 2 + 4), cbPitch = Read(set + kPlaneSize * 2 + 8);
        if (!ValidPlane(y, yPitch, height) || !ValidPlane(cr, crPitch, chromaHeight) || !ValidPlane(cb, cbPitch, chromaHeight))
            return false;

        frame = { width, height, GuestPtr<uint8_t>(y), GuestPtr<uint8_t>(cb), GuestPtr<uint8_t>(cr), yPitch, cbPitch, crPitch };
        return true;
    }
}
