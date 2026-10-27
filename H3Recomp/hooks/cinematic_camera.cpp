// Cutscene cameras at 60 ticks per second (one of the 60 Hz content fixes,
// hooks::ContentFixes(); hooks/tick_rate.h).
//
// A cinematic's camera (director camera mode 5) follows a track of camera
// frames recorded at kCameraFramesPerSecond: each frame of a scene's shot
// holds a position, forward and up vector and field of view. Every tick the
// director update (sub_82290F80) counts the ticks since the scene started
// and sets the frame to trunc(ticks * seconds per tick * frames per
// second), and the camera (sub_82291B40, through sub_82292D28) is built from
// that one frame. At 30 Hz that's a new frame every tick; at 60 Hz the
// camera holds still for a tick, then jumps two ticks' worth, while the
// objects it films animate every tick. In The Ark's intro the camera turns
// to follow the Pelican, so the Pelican visibly jumped back and forth on
// screen (private/BUGS.md; Sierra 117's ending and The Covenant too).
//
// Here, when the tick falls between two frames, sub_82292D28 builds the
// camera from a frame blended between the current one and the next (by the
// fraction of a frame the tick is past the current one), so the camera
// moves every tick. The blend is built in guest memory below the stack (the
// call runs with the stack pointer moved past it). The field of view, read
// from the frame directly by the camera update, still steps at 30 Hz.
//
// H3_LOG_CAMERA=1 logs the camera mode and the camera the director's camera
// update writes, on every call, with the tick.

#include "config/env.h"
#include "hooks/tick_rate.h"
#include "memory.h"
#include "tags/tags.h"

#include <ppc_recomp_shared.h>

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace
{
    // Game time globals (TLS slot kTlsGameTime): seconds per tick (float)
    // and the current tick (u32).
    constexpr uint32_t kTlsGameTime = 188;
    constexpr uint32_t kGameTimeSecondsPerTick = 8;
    constexpr uint32_t kGameTimeTick = 12;

    // Director globals (TLS slot kTlsDirector): the camera mode (s16), and
    // for the cinematic camera (kModeCinematic) the cinematic tag's datum,
    // the scene's shot, the camera frame (s32; clamped to the shot's frames
    // where used) and the ticks since the scene was set.
    constexpr uint32_t kTlsDirector = 140;
    constexpr uint32_t kDirectorMode = 2;
    constexpr uint32_t kDirectorCinematicTag = 64;
    constexpr uint32_t kDirectorCinematicShot = 68;
    constexpr uint32_t kDirectorCinematicFrame = 72;
    constexpr uint32_t kDirectorCinematicTicks = 96;
    constexpr int16_t kModeCinematic = 5;

    // Cinematic scene tag: its shots block (count, elements), and in each
    // shot its camera frames block.
    constexpr uint32_t kSceneShotCount = 72;
    constexpr uint32_t kSceneShots = 76;
    constexpr uint32_t kShotBytes = 164;
    constexpr uint32_t kShotFrameCount = 152;
    constexpr uint32_t kShotFrames = 156;

    // A camera frame: position, forward, up (3 floats each), then the
    // field of view and the rest (copied from the current frame).
    constexpr uint32_t kFrameBytes = 68;
    constexpr uint32_t kFramePosition = 0;
    constexpr uint32_t kFrameForward = 12;
    constexpr uint32_t kFrameUp = 24;

    // The camera track's frames per second (the float sub_82290F80 scales
    // ticks * seconds per tick by: 30).
    constexpr uint32_t kCameraFramesPerSecond = 0x8202AF7C;

    // Guest stack the blended frame is built in, below the stack pointer,
    // and how far the stack pointer moves for the call (past the frame).
    constexpr uint32_t kBlendFrameBelowStack = 128;
    constexpr uint32_t kBlendStackDrop = 256;

    // Camera out words logged per call (H3_LOG_CAMERA).
    constexpr uint32_t kLogWords = 40;

    float LoadFloat(uint32_t address) { return h3::GuestRef<float>(address); }
    void StoreFloat(uint32_t address, float value) { h3::GuestRef<float>(address) = value; }

    // Writes a + (b - a) * t of the 3-vectors at a and b to out, scaled to
    // unit length when unit is set (the forward and up vectors).
    void BlendVector(uint32_t a, uint32_t b, float t, uint32_t out, bool unit)
    {
        float v[3];
        for (uint32_t i = 0; i < 3; i++)
        {
            float from = LoadFloat(a + i * 4);
            v[i] = from + (LoadFloat(b + i * 4) - from) * t;
        }
        float length = unit ? std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) : 1.0f;
        if (length <= 0.0f)
            length = 1.0f;
        for (uint32_t i = 0; i < 3; i++)
            StoreFloat(out + i * 4, v[i] / length);
    }

    // The cinematic camera's current frame and the fraction of a frame the
    // tick is past it, when it is between that frame and a next one of the
    // same shot; false otherwise (no cinematic camera, a whole frame, the
    // shot's last frame, or the 60 Hz content fixes off).
    bool BetweenFrames(uint32_t tls, uint32_t& frame, float& fraction)
    {
        if (!h3::hooks::ContentFixes())
            return false;
        uint32_t director = h3::GuestRef<uint32_t>(tls + kTlsDirector);
        if (int16_t(h3::GuestRef<int16_t>(director + kDirectorMode)) != kModeCinematic)
            return false;
        uint32_t tag = h3::GuestRef<uint32_t>(director + kDirectorCinematicTag);
        if (tag == 0xFFFFFFFF)
            return false;
        uint32_t scene = h3::native::tags::TagDataBySlot(tag & 0xFFFF);
        int32_t shotCount = h3::GuestRef<int32_t>(scene + kSceneShotCount);
        int32_t shotIndex = h3::GuestRef<int32_t>(director + kDirectorCinematicShot);
        if (shotIndex < 0 || shotIndex >= shotCount)
            return false;
        uint32_t shot = uint32_t(h3::GuestRef<uint32_t>(scene + kSceneShots)) + uint32_t(shotIndex) * kShotBytes;
        int32_t frameCount = h3::GuestRef<int32_t>(shot + kShotFrameCount);
        int32_t frameIndex = h3::GuestRef<int32_t>(director + kDirectorCinematicFrame);
        if (frameIndex < 0 || frameIndex + 1 >= frameCount)
            return false;
        // As sub_82290F80 computes the frame count, in floats.
        uint32_t gameTime = h3::GuestRef<uint32_t>(tls + kTlsGameTime);
        int32_t ticks = h3::GuestRef<int32_t>(director + kDirectorCinematicTicks);
        float frames = float(ticks) * LoadFloat(gameTime + kGameTimeSecondsPerTick) * LoadFloat(kCameraFramesPerSecond);
        fraction = frames - std::trunc(frames);
        if (fraction <= 0.0f)
            return false;
        frame = uint32_t(h3::GuestRef<uint32_t>(shot + kShotFrames)) + uint32_t(frameIndex) * kFrameBytes;
        return true;
    }
}

// cinematic camera from frame(r3 camera frame, or 0 for the current one;
// r4 camera matrix out).
PPC_FUNC_IMPL(__imp__sub_82292D28);
PPC_FUNC(sub_82292D28)
{
    uint32_t frame;
    float fraction;
    uint32_t tls = h3::GuestRef<uint32_t>(ctx.r13.u32);
    if (!BetweenFrames(tls, frame, fraction) || (ctx.r3.u32 != 0 && ctx.r3.u32 != frame))
    {
        __imp__sub_82292D28(ctx, base);
        return;
    }
    uint32_t stack = ctx.r1.u32;
    uint32_t blend = stack - kBlendFrameBelowStack;
    uint32_t next = frame + kFrameBytes;
    for (uint32_t i = 0; i < kFrameBytes; i += 4)
        h3::GuestRef<uint32_t>(blend + i) = uint32_t(h3::GuestRef<uint32_t>(frame + i));
    BlendVector(frame + kFramePosition, next + kFramePosition, fraction, blend + kFramePosition, false);
    BlendVector(frame + kFrameForward, next + kFrameForward, fraction, blend + kFrameForward, true);
    BlendVector(frame + kFrameUp, next + kFrameUp, fraction, blend + kFrameUp, true);
    ctx.r3.u64 = blend;
    ctx.r1.u64 = stack - kBlendStackDrop;
    __imp__sub_82292D28(ctx, base);
    ctx.r1.u64 = stack;
}

// director camera update(r3, r4, r5 camera out): logs (H3_LOG_CAMERA).
PPC_FUNC_IMPL(__imp__sub_82291B40);
PPC_FUNC(sub_82291B40)
{
    static const bool enabled = h3::EnvFlag("H3_LOG_CAMERA");
    uint32_t out = ctx.r5.u32;
    uint32_t tls = h3::GuestRef<uint32_t>(ctx.r13.u32);
    uint32_t director = h3::GuestRef<uint32_t>(tls + kTlsDirector);
    int16_t mode = int16_t(h3::GuestRef<int16_t>(director + kDirectorMode));
    __imp__sub_82291B40(ctx, base);
    if (!enabled)
        return;
    static uint32_t calls = 0;
    uint32_t gameTime = h3::GuestRef<uint32_t>(tls + kTlsGameTime);
    uint32_t tick = gameTime ? uint32_t(h3::GuestRef<uint32_t>(gameTime + kGameTimeTick)) : 0;
    fprintf(stderr, "[camera] call %u tick %u mode %d:", calls++, tick, int(mode));
    for (uint32_t i = 0; i < kLogWords; i++)
        fprintf(stderr, " %.4f", double(LoadFloat(out + i * 4)));
    fprintf(stderr, "\n");
}
