---
title: Home
nav_order: 1
---

<div class="hero">
  <h1>Halo 3 Recomp</h1>
  <p class="tagline">Halo 3, running natively on PC. Recompiled, not emulated.</p>
  <a class="btn btn-primary" href="https://github.com/george0815/halo-3-recomp#installing">Install</a>
  <a class="btn" href="{{ '/faq.html' | relative_url }}">FAQ</a>
  <a class="btn" href="{{ '/ARCHITECTURE.html' | relative_url }}">How it works</a>
  <a class="btn" href="https://github.com/george0815/halo-3-recomp">GitHub</a>
</div>

The Xbox 360 game's own code, **statically recompiled** into a native
program for Linux and Windows, with the 360's operating system, graphics and
audio rebuilt around it. No part of Halo 3 is distributed: the installer
builds the game from **your own disc image**, on your PC.

<div class="feature-grid">
  <div class="hud-panel"><h3>The whole game</h3><p>Campaign solo and co-op, multiplayer Custom Games, Forge and Theater, on Halo 3's own engine and rules.</p></div>
  <div class="hud-panel"><h3>60 frames per second</h3><p>With fixes for everything the 360 tied to 30 ticks, checked tick by tick against recorded films.</p></div>
  <div class="hud-panel"><h3>Made for PC</h3><p>Direct mouse look, a mouse pointer in the menus, your keys in the game's prompts, FOV, centered crosshair.</p></div>
  <div class="hud-panel"><h3>Play together</h3><p>Split screen for four, and system link over your LAN or the internet, Linux and Windows together.</p></div>
  <div class="hud-panel"><h3>Sharper</h3><p>2x and 3x the 360's resolution, FXAA, any window size, fullscreen.</p></div>
  <div class="hud-panel"><h3>Bring your own disc</h3><p>The installer checks your disc image, translates the game's code and compiles it in a few minutes.</p></div>
</div>

## Find your way

| You want to | Read |
|---|---|
| Install and play | the [README](https://github.com/george0815/halo-3-recomp#readme), then the [player guide](guide.html) |
| Know if it's for you | the [FAQ](faq.html) |
| See how it's built | [architecture](ARCHITECTURE.html), [the ISO tool](ISO_TOOL.html), [testing](testing.html) |
| Work on it | [building](building.html), [debugging switches](debugging.html), the [roadmap](ROADMAP.html) |
| Follow along | [project updates](blog.html) |

## Where things stand

<div class="hud-panel" markdown="1">
**Version 0.75:** campaign, co-op, multiplayer, Forge and Theater play
through at 60 Hz on Linux and Windows, with keyboard and mouse made for PC,
split screen and system link. **Next:** the native port, rewriting the
recompiled code as hand-written C++, subsystem by subsystem
([status](NATIVE_PORT_STATUS.html)).
</div>
