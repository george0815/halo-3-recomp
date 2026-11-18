---
title: Credits
nav_order: 11
---

# Credits


This project stands on other people's work:

- **[Xenia](https://github.com/xenia-project/xenia)**, the Xbox 360
  emulator. Its source is the reference for how the console behaves, and
  the runtime's emulation of the kernel, the Xenos GPU (command stream,
  shaders, EDRAM) and XMA audio follows it closely. The XMA decoder is
  Xenia's `xmaframes` FFmpeg decoder. Without Xenia's years of research,
  this project wouldn't exist.
- **[XenonRecomp](https://github.com/hedge-dev/XenonRecomp)** (hedge-dev),
  the static recompiler that turns the game's PowerPC code into C++, and
  **[Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp)**,
  which showed what a recompiled 360 game can become and is the model for
  this project's approach.
- **Halo: The Master Chief Collection** and the **Halo 3 Editing Kit**,
  studied for how the PC version handles 60 Hz, mouse look and the other PC
  options.
- **Bungie**, for Halo 3, and **343 Industries**.
- The libraries and tools in every build: [SDL](https://libsdl.org),
  [Qt](https://qt.io), [FFmpeg](https://ffmpeg.org),
  [shaderc](https://github.com/google/shaderc) (glslang, SPIRV-Tools),
  [LLVM](https://llvm.org) (the compiler the Windows release ships),
  [xwin](https://github.com/Jake-Shadle/xwin),
  [SIMDe](https://github.com/simd-everywhere/simde),
  [xxHash](https://github.com/Cyan4973/xxHash), [{fmt}](https://fmt.dev) and
  [toml++](https://marzer.github.io/tomlplusplus/).
