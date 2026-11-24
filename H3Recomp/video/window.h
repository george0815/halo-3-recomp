// The SDL window: creation (with the SDL_GPU device the renderer uses), the
// event loop, and presenting frames.

#pragma once

#include <cstdint>

namespace h3::video
{
    // The host window. SDL wants its window and event loop on the process's
    // main thread, so the guest's main thread runs on a thread of its own
    // (see main.cpp) and the host main thread sits in RunEventLoop.
    //
    // Each loop iteration shows the renderer's latest frame (scaled to fit,
    // letterboxed), or before the first one the Bink video's frame buffers,
    // and the title shows the frame rate.

    // Creates the window. Call on the main thread before RunEventLoop.
    bool CreateMainWindow();

    // Gives the window the Halo 3 icon (assets/icon.png, compiled in).
    // Call after CreateMainWindow.
    void SetWindowIcon();

    // Pumps window events until the window is closed or RequestQuit is
    // called. Returns the exit code passed to RequestQuit (0 when closed).
    int RunEventLoop();

    // Thread-safe. Ends RunEventLoop.
    void RequestQuit(int exitCode);

    // Thread-safe. Called by the command processor for each VdSwap.
    void OnFramePresented(uint32_t frame);

    // Main thread: a point in the window (SDL window coordinates) as a
    // fraction of the game frame drawn in it (0..1 across the picture,
    // letterbox excluded). False before the first frame or off the picture.
    bool WindowToFrame(float windowX, float windowY, float& frameX, float& frameY);
}
