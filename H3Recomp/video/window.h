#pragma once

#include <cstdint>

namespace h3::video
{
    // The host window. SDL wants its window and event loop on the process's
    // main thread, so the guest's main thread runs on a thread of its own
    // (see main.cpp) and the host main thread sits in RunEventLoop.
    //
    // Nothing is drawn yet: the window is cleared every frame and its title
    // shows how many frames the game has presented through VdSwap.

    // Creates the window. Call on the main thread before RunEventLoop.
    bool CreateMainWindow();

    // Pumps window events until the window is closed or RequestQuit is
    // called. Returns the exit code passed to RequestQuit (0 when closed).
    int RunEventLoop();

    // Thread-safe. Ends RunEventLoop.
    void RequestQuit(int exitCode);

    // Thread-safe. Called by the command processor for each VdSwap.
    void OnFramePresented(uint32_t frame);
}
