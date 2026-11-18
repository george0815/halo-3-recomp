---
title: Home
nav_order: 1
---

# Halo 3 Recomp

Halo 3 running natively on PC (Linux and Windows), made by **statically
recompiling** the Xbox 360 game's own code into C++, with the 360's
operating system, graphics and audio rebuilt around it. No emulator, and no
part of Halo 3 is distributed: the installer builds the game from your own
disc image, on your PC.

**Players:** start with the [README](https://github.com/george0815/halo-3-recomp#readme)
(features, installing), then the [player guide](guide.html) and the [FAQ](faq.html).

**Developers:** [how the code fits together](ARCHITECTURE.html), [building
from source](building.html), [how changes are tested](testing.html),
[debugging switches](debugging.html), [the ISO tool](ISO_TOOL.html), and the
[roadmap](ROADMAP.html).

**News:** [project updates](blog.html).

## Where things stand

Version 0.75: campaign, co-op, multiplayer, Forge and Theater play through
at 60 Hz on Linux and Windows, with keyboard and mouse made for PC, split
screen and system link. Next: the native port, replacing the recompiled code
with hand-written C++ subsystem by subsystem ([status](NATIVE_PORT_STATUS.html)).
