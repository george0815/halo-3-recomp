// GPU frame capture: a text dump of one frame's draws, resolves and shader
// loads with the render state behind each (debugging).

#pragma once

#include <cstdint>

#include <cstdint>
#include <string>

namespace h3::gpu
{
    // Debug aid: with H3_GPU_CAPTURE=<frame>, writes gpu_capture.txt with
    // every draw, resolve and shader load the command processor sees in that
    // frame (counted in swaps), along with the register state that matters
    // for rendering. Command processor thread only.
    namespace capture
    {
        // Shader microcode was loaded (IM_LOAD / IM_LOAD_IMMEDIATE).
        void OnShaderLoad(uint32_t type, uint32_t guestAddress, uint32_t dwords);

        // A draw packet. indexBase/indexWords are 0 for auto-indexed draws.
        void OnDraw(uint32_t initiator, uint32_t indexBase, uint32_t indexWords);

        // Predication state changed (SET_BIN_MASK / SET_BIN_SELECT).
        void OnBinState(uint64_t mask, uint64_t select);

        // A packet was skipped by predication.
        void OnPredicatedSkip(uint32_t opcode);

        // An occlusion query report (EVENT_WRITE_ZPD) to a sample count
        // address.
        void OnZpd(uint32_t initiator, uint32_t address);

        // A swap: ends the frame.
        void OnSwap(uint32_t frame);

        // F9 (video/window.cpp; any thread): capture the next frame into a
        // new folder, captures/capture_<n>/: gpu_capture.txt, the shaders,
        // the presented frame and every resolve (the renderer's dumps use
        // HotkeyFrame/HotkeyDirectory). For scenes a script can't reach.
        void RequestCapture(bool deep = false);
        // Whether the current hotkey capture is a deep one (F10): the
        // renderer also saves color target 0 after every draw.
        bool HotkeyDeep();
        // The frame the last hotkey capture targets (0: none) and its folder.
        // Command processor thread.
        uint32_t HotkeyFrame();
        const std::string& HotkeyDirectory();
        // A file name inside the current capture's folder (as is without one).
        std::string CapturePath(const std::string& name);
    }
}
