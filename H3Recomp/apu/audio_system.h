// XAudio render driver: audio output (see the comment below).

#pragma once

#include <cstdint>

// The XAudio render driver (Xenia's AudioSystem): the game registers a
// callback, a worker thread calls it whenever the output has room for
// another frame, and the callback submits one mixed frame (256 samples of 6
// channels, 48 kHz, big-endian floats, one channel after another) through
// XAudioSubmitRenderDriverFrame. Frames go to the default SDL audio device.
namespace h3::apu
{
    constexpr uint32_t kFrameSamples = 256;
    constexpr uint32_t kFrameChannels = 6;
    constexpr uint32_t kSampleRate = 48000;

    // Returns the client index, or -1 when all slots are taken.
    int RegisterClient(uint32_t callback, uint32_t callbackArg);
    void UnregisterClient(uint32_t index);

    // Queues the frame at guestAddress for output.
    void SubmitFrame(uint32_t index, uint32_t guestAddress);
}
