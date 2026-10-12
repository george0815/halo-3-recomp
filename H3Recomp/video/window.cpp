// The SDL window and event loop (window.h). Window size, fullscreen and
// vsync come from settings.ini ([video]); F11 or Alt+Enter toggles
// fullscreen.

#include "video/window.h"
#include "config/env.h"
#include "config/settings.h"

#include "video/bink_view.h"
#include "gpu/frame_capture.h"
#include "gpu/renderer.h"
#include "input/input.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace h3::debug
{
    extern std::atomic<uint32_t> g_traceFrame;
    void UpdateTraceWindow(uint32_t frame);
    void UpdateProfiler(uint32_t frame);
    void UpdateWriteWatch(uint32_t frame);
}

namespace h3::hooks
{
    void UpdateCampaignUnlock();
    void LogTickRate();
}

namespace h3::kernel
{
    void DumpWaitTotals();
}

namespace h3::video
{
    namespace
    {
        SDL_Window* g_window = nullptr;
        SDL_Renderer* g_renderer = nullptr;
        SDL_GPUDevice* g_device = nullptr;
        SDL_Texture* g_binkTexture = nullptr;

        std::atomic<uint32_t> g_framesPresented{ 0 };
        uint32_t g_quitEvent = 0;
    }

    namespace
    {
        // The renderer's latest frame, through an SDL texture wrapping its
        // GPU texture (one per GPU texture).
        bool DrawGameFrame()
        {
            gpu::renderer::Frame frame;
            if (!gpu::renderer::AcquireFrame(frame))
                return false;
            static std::unordered_map<SDL_GPUTexture*, SDL_Texture*> wrapped;
            SDL_Texture*& texture = wrapped[frame.texture];
            if (!texture)
            {
                SDL_PropertiesID props = SDL_CreateProperties();
                SDL_SetPointerProperty(props, SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER, frame.texture);
                SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_ABGR8888);
                SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, frame.textureWidth);
                SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, frame.textureHeight);
                texture = SDL_CreateTextureWithProperties(g_renderer, props);
                SDL_DestroyProperties(props);
                if (!texture)
                {
                    fprintf(stderr, "[video] can't wrap the frame texture: %s\n", SDL_GetError());
                    return false;
                }
            }
            int windowWidth = 0, windowHeight = 0;
            SDL_GetCurrentRenderOutputSize(g_renderer, &windowWidth, &windowHeight);
            float scale = std::min(float(windowWidth) / frame.width, float(windowHeight) / frame.height);
            SDL_FRect source{ float(frame.x), float(frame.y), float(frame.width), float(frame.height) };
            SDL_FRect dest{ (windowWidth - frame.width * scale) / 2, (windowHeight - frame.height * scale) / 2,
                frame.width * scale, frame.height * scale };
            SDL_RenderTexture(g_renderer, texture, &source, &dest);
            return true;
        }

        // The Bink movie's current frame straight from its YUV buffers (shown
        // until the renderer has a frame of its own).
        void DrawBinkFrame()
        {
            BinkFrame frame;
            if (!GetBinkFrame(frame))
                return;

            float width = 0, height = 0;
            if (g_binkTexture)
                SDL_GetTextureSize(g_binkTexture, &width, &height);
            if (!g_binkTexture || width != frame.width || height != frame.height)
            {
                SDL_DestroyTexture(g_binkTexture);
                g_binkTexture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING,
                    int(frame.width), int(frame.height));
                if (!g_binkTexture)
                    return;
                // Bink video is BT.601 limited range.
                SDL_PropertiesID props = SDL_GetTextureProperties(g_binkTexture);
                SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_COLORSPACE_NUMBER, SDL_COLORSPACE_BT601_LIMITED);
            }

            // Bink may be decoding into these planes right now; tearing is
            // acceptable for a debug view.
            SDL_UpdateYUVTexture(g_binkTexture, nullptr, frame.y, int(frame.yPitch), frame.u, int(frame.uPitch), frame.v,
                int(frame.vPitch));

            // Fit the frame inside the window, keeping its aspect ratio.
            int windowWidth = 0, windowHeight = 0;
            SDL_GetCurrentRenderOutputSize(g_renderer, &windowWidth, &windowHeight);
            float scale = std::min(float(windowWidth) / frame.width, float(windowHeight) / frame.height);
            SDL_FRect dest{ (windowWidth - frame.width * scale) / 2, (windowHeight - frame.height * scale) / 2,
                frame.width * scale, frame.height * scale };
            SDL_RenderTexture(g_renderer, g_binkTexture, nullptr, &dest);
        }
    }

    bool CreateMainWindow()
    {
        // NVIDIA's on-disk pipeline cache is 256 MiB per application by
        // default; the game's pipelines outgrow it, and evicted ones cost
        // up to seconds to rebuild on the next run. Read when the driver
        // loads, so set it first (a value the user set wins).
        SDL_setenv_unsafe("__GL_SHADER_DISK_CACHE_SIZE", "4294967296", 0);

        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD))
        {
            fprintf(stderr, "[video] SDL_Init failed: %s\n", SDL_GetError());
            return false;
        }

        // Size and mode from settings.ini ([video]). Fullscreen is
        // borderless at the desktop resolution (no mode switch).
        const config::VideoSettings& video = config::Active().video;
        SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | (video.fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
        g_window = SDL_CreateWindow("Halo 3 Recomp", video.width, video.height, flags);
        if (!g_window)
        {
            fprintf(stderr, "[video] SDL_CreateWindow failed: %s\n", SDL_GetError());
            return false;
        }

        // One SDL_GPU (Vulkan) device: the command processor renders the
        // game with it, and the window's SDL renderer sits on top of it to
        // present frames (and the Bink debug view).
        g_device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr);
        if (g_device)
            g_renderer = SDL_CreateGPURenderer(g_device, g_window);
        if (!g_renderer)
        {
            fprintf(stderr, "[video] no GPU device (%s); falling back to the default renderer\n", SDL_GetError());
            if (g_device)
                SDL_DestroyGPUDevice(g_device);
            g_device = nullptr;
            g_renderer = SDL_CreateRenderer(g_window, nullptr);
            if (!g_renderer)
            {
                fprintf(stderr, "[video] SDL_CreateRenderer failed: %s\n", SDL_GetError());
                return false;
            }
        }
        SDL_SetRenderVSync(g_renderer, video.vsync ? 1 : 0);
        if (g_device && !gpu::renderer::Initialize(g_device))
            fprintf(stderr, "[video] host rendering is off\n");

        input::Initialize();
        g_quitEvent = SDL_RegisterEvents(1);
        return true;
    }

    int RunEventLoop()
    {
        using Clock = std::chrono::steady_clock;

        int exitCode = 0;
        uint32_t lastFrame = 0;
        auto lastTitleUpdate = Clock::now();

        for (;;)
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                if (event.type == SDL_EVENT_QUIT)
                    return exitCode;
                if (event.type == g_quitEvent)
                    return event.user.code;
                // F9: capture the next frame for debugging (captures/,
                // gpu/frame_capture.h).
                if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == SDL_SCANCODE_F9)
                {
                    gpu::capture::RequestCapture();
                    fprintf(stderr, "[video] F9: capture requested\n");
                }
                // F10: the same plus color target 0 after every draw (slow:
                // the game pauses while hundreds of images are written).
                if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == SDL_SCANCODE_F10)
                {
                    gpu::capture::RequestCapture(true);
                    fprintf(stderr, "[video] F10: deep capture requested\n");
                }
                // F11 or Alt+Enter toggles fullscreen.
                if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                    (event.key.scancode == SDL_SCANCODE_F11 || (event.key.scancode == SDL_SCANCODE_RETURN && (event.key.mod & SDL_KMOD_ALT))))
                {
                    bool fullscreen = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0;
                    SDL_SetWindowFullscreen(g_window, !fullscreen);
                    continue;
                }
                input::HandleEvent(event);
            }
            input::Update(g_framesPresented.load());

            // Twice a second: frames presented and the rate since last time.
            auto now = Clock::now();
            double elapsed = std::chrono::duration<double>(now - lastTitleUpdate).count();
            if (elapsed >= 0.5)
            {
                uint32_t frame = g_framesPresented.load();
                char title[128];
                snprintf(title, sizeof(title), "Halo 3 Recomp - frame %u (%.0f fps)", frame,
                    (frame - lastFrame) / elapsed);
                SDL_SetWindowTitle(g_window, title);
                lastFrame = frame;
                lastTitleUpdate = now;
            }

            // The GPU draws nothing yet; show the Bink video if one is
            // decoding. Vsync paces the loop.
            SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
            SDL_RenderClear(g_renderer);
            if (!DrawGameFrame())
                DrawBinkFrame();
            SDL_RenderPresent(g_renderer);
        }
    }

    void RequestQuit(int exitCode)
    {
        SDL_Event event{};
        event.type = g_quitEvent;
        event.user.code = exitCode;
        SDL_PushEvent(&event);
    }

    void OnFramePresented(uint32_t frame)
    {
        g_framesPresented.store(frame);

        // H3_LOG_FPS: frames presented per second, and the slowest frame.
        static const bool logFps = EnvFlag("H3_LOG_FPS");
        if (logFps)
        {
            using Clock = std::chrono::steady_clock;
            static Clock::time_point windowStart = Clock::now(), last = windowStart;
            static uint32_t windowFrames = 0;
            static double slowest = 0;
            Clock::time_point now = Clock::now();
            slowest = std::max(slowest, std::chrono::duration<double, std::milli>(now - last).count());
            last = now;
            windowFrames++;
            double elapsed = std::chrono::duration<double>(now - windowStart).count();
            if (elapsed >= 1.0)
            {
                fprintf(stderr, "[fps] frame %u: %.1f fps, slowest %.1f ms\n", frame, windowFrames / elapsed, slowest);
                windowStart = now;
                windowFrames = 0;
                slowest = 0;
            }
        }

        if (frame % 150 == 0)
            kernel::DumpWaitTotals();  // H3_LOG_WAITS

        debug::g_traceFrame.store(frame);
        debug::UpdateTraceWindow(frame);
        debug::UpdateProfiler(frame);
        debug::UpdateWriteWatch(frame);
        hooks::UpdateCampaignUnlock();
        hooks::LogTickRate();
    }
}
