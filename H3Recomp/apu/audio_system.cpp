#include "apu/audio_system.h"

#include "kernel/thread.h"
#include "memory.h"

#include <ppc_recomp_shared.h>
#include <SDL3/SDL.h>

#include <atomic>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

namespace h3::apu
{
    namespace
    {
        constexpr int kMaxClients = 8;
        // Frames kept queued at the device: about 43 ms of latency. Fewer
        // underrun when a frame of the game takes long.
        constexpr uint32_t kQueuedFrames = 8;
        constexpr uint32_t kFrameBytes = kFrameSamples * kFrameChannels * sizeof(float);

        struct Client
        {
            bool active = false;
            uint32_t callback = 0;
            uint32_t callbackArg = 0;
        };

        std::mutex g_mutex;
        Client g_clients[kMaxClients];
        SDL_AudioStream* g_stream = nullptr;
        std::atomic<uint32_t> g_submitted{ 0 };
        std::once_flag g_started;

        void OpenDevice()
        {
            if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
            {
                fprintf(stderr, "[audio] SDL audio init failed: %s\n", SDL_GetError());
                return;
            }
            // SDL's 5.1 layout matches XAudio's (FL FR C LFE BL BR) and
            // downmixes to whatever the device has.
            SDL_AudioSpec spec{ SDL_AUDIO_F32, int(kFrameChannels), int(kSampleRate) };
            g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
            if (!g_stream)
            {
                fprintf(stderr, "[audio] can't open the audio device: %s\n", SDL_GetError());
                return;
            }
            SDL_ResumeAudioStreamDevice(g_stream);
        }

        uint32_t QueuedFrames()
        {
            if (!g_stream)
                return 0;
            int queued = SDL_GetAudioStreamQueued(g_stream);
            return queued > 0 ? uint32_t(queued) / kFrameBytes : 0;
        }

        // Calls each client's callback whenever the device has room for a
        // frame (or, without a device, at the real-time frame rate).
        void Worker()
        {
            PPCContext ctx{};
            kernel::AdoptHostThread(ctx, 0x10000);
            uint64_t stack = ctx.r1.u64;
            OpenDevice();

            using Clock = std::chrono::steady_clock;
            const auto framePeriod = std::chrono::nanoseconds(1'000'000'000ull * kFrameSamples / kSampleRate);
            auto nextFrame = Clock::now();
            for (;;)
            {
                bool room = g_stream ? QueuedFrames() < kQueuedFrames : Clock::now() >= nextFrame;
                if (!room)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                nextFrame += framePeriod;

                uint32_t before = g_submitted.load();
                bool any = false;
                for (int i = 0; i < kMaxClients; i++)
                {
                    Client client;
                    {
                        std::lock_guard lock(g_mutex);
                        client = g_clients[i];
                    }
                    if (!client.active)
                        continue;
                    any = true;
                    ctx.r1.u64 = stack;
                    ctx.r3.u64 = client.callbackArg;
                    (PPC_LOOKUP_FUNC(g_base, client.callback))(ctx, g_base);
                }
                // A callback that didn't submit (nothing to play yet, or it
                // submits later): don't spin on it.
                if (!any || g_submitted.load() == before)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    int RegisterClient(uint32_t callback, uint32_t callbackArg)
    {
        int index = -1;
        {
            std::lock_guard lock(g_mutex);
            for (int i = 0; i < kMaxClients && index < 0; i++)
                if (!g_clients[i].active)
                {
                    g_clients[i] = { true, callback, callbackArg };
                    index = i;
                }
        }
        if (index >= 0)
            std::call_once(g_started, [] { std::thread(Worker).detach(); });
        return index;
    }

    void UnregisterClient(uint32_t index)
    {
        std::lock_guard lock(g_mutex);
        if (index < kMaxClients)
            g_clients[index].active = false;
    }

    void SubmitFrame(uint32_t index, uint32_t guestAddress)
    {
        (void)index;
        g_submitted++;
        if (guestAddress == 0)
            return;
        // Guest: each channel's 256 big-endian floats in turn. Host:
        // interleaved native floats.
        const uint32_t* source = GuestPtr<uint32_t>(guestAddress);
        float frame[kFrameSamples * kFrameChannels];
        for (uint32_t channel = 0; channel < kFrameChannels; channel++)
            for (uint32_t sample = 0; sample < kFrameSamples; sample++)
                frame[sample * kFrameChannels + channel] =
                    std::bit_cast<float>(__builtin_bswap32(source[channel * kFrameSamples + sample]));
        if (g_stream)
            SDL_PutAudioStreamData(g_stream, frame, sizeof(frame));

        // H3_AUDIO_DUMP=<file>: also write the output as raw 6-channel
        // 48 kHz float (ffmpeg -f f32le -ac 6 -ar 48000 -i <file> out.wav).
        static FILE* dump = [] {
            const char* path = getenv("H3_AUDIO_DUMP");
            return path ? fopen(path, "wb") : nullptr;
        }();
        if (dump)
            fwrite(frame, sizeof(frame), 1, dump);

        // H3_LOG_AUDIO=1: frames submitted and their peak level, once a second.
        static const bool log = getenv("H3_LOG_AUDIO") != nullptr;
        if (log)
        {
            static float peak = 0;
            for (float value : frame)
                peak = std::max(peak, std::abs(value));
            static uint32_t frames = 0;
            if (++frames % (kSampleRate / kFrameSamples) == 0)
            {
                fprintf(stderr, "[audio] %u frames, peak %.3f\n", frames, peak);
                peak = 0;
            }
        }
    }
}
