#include "video/window.h"

#include "video/bink_view.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>

namespace h3::video
{
    namespace
    {
        // Halo 3 outputs 1280x720.
        constexpr int kDefaultWidth = 1280;
        constexpr int kDefaultHeight = 720;

        SDL_Window* g_window = nullptr;
        SDL_Renderer* g_renderer = nullptr;
        SDL_Texture* g_binkTexture = nullptr;

        std::atomic<uint32_t> g_framesPresented{ 0 };
        uint32_t g_quitEvent = 0;
    }

    namespace
    {
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
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
        {
            fprintf(stderr, "[video] SDL_Init failed: %s\n", SDL_GetError());
            return false;
        }

        if (!SDL_CreateWindowAndRenderer("Halo 3 Recomp", kDefaultWidth, kDefaultHeight, SDL_WINDOW_RESIZABLE,
                &g_window, &g_renderer))
        {
            fprintf(stderr, "[video] SDL_CreateWindowAndRenderer failed: %s\n", SDL_GetError());
            return false;
        }
        SDL_SetRenderVSync(g_renderer, 1);

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
            }

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
    }
}
