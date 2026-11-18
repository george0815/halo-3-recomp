---
title: Home
nav_order: 1
---

<div class="launcher">
  <p class="title">Halo 3 Recomp</p>
  <p class="tagline">Halo 3 on PC, statically recompiled from the Xbox 360 game. Not a native port (yet).</p>
  <ul class="menu">
    <li><a class="selected" href="https://github.com/george0815/halo-3-recomp#installing" data-desc="Download the release, choose your Halo 3 disc image, and play.">Install</a></li>
    <li><a href="{{ '/guide.html' | relative_url }}" data-desc="The launcher, controls, split screen and system link, in detail.">Player guide</a></li>
    <li><a href="{{ '/faq.html' | relative_url }}" data-desc="What you need, why you bring your own disc, how it runs at 60 fps.">FAQ</a></li>
    <li><a href="{{ '/ARCHITECTURE.html' | relative_url }}" data-desc="How the recompiled game, the runtime and the hooks fit together.">How it works</a></li>
    <li><a href="{{ '/testing.html' | relative_url }}" data-desc="Every change checked tick by tick against recorded Theater films.">Testing</a></li>
    <li><a href="{{ '/building.html' | relative_url }}" data-desc="Build it yourself, and how the releases are made.">Building</a></li>
    <li><a href="{{ '/ROADMAP.html' | relative_url }}" data-desc="Where the project is going: the release, then the native port.">Roadmap</a></li>
    <li><a href="{{ '/blog.html' | relative_url }}" data-desc="Monthly notes on what changed and what's next.">Updates</a></li>
    <li><a href="https://github.com/george0815/halo-3-recomp" data-desc="The source code, issues and releases.">GitHub</a></li>
  </ul>
  <p class="description" id="menu-description">Download the release, choose your Halo 3 disc image, and play.</p>
  <p class="hints"><b>Click</b> Select</p>
</div>

<script>
  (function () {
    var description = document.getElementById("menu-description");
    var items = document.querySelectorAll(".launcher .menu a");
    items.forEach(function (item) {
      item.addEventListener("mouseenter", function () {
        items.forEach(function (other) { other.classList.remove("selected"); });
        item.classList.add("selected");
        description.textContent = item.getAttribute("data-desc");
      });
    });
  })();
</script>

The Xbox 360 game's own code, **statically recompiled** to C++ and running
directly on your CPU, on Linux and Windows. The hardware and operating
system around it, the GPU, XMA audio and the 360's kernel, are emulated at a
high level, built on [Xenia](https://github.com/xenia-project/xenia)'s
research. A true native port, hand-written C++ in place of the recompiled
code, is the long-term goal. No part of Halo 3 is distributed: the
installer builds the game from **your own disc image**, on your PC.

<div class="feature-grid">
  <div class="panel"><h3>The whole game</h3><p>Campaign solo and co-op, multiplayer Custom Games, Forge and Theater, on Halo 3's own engine and rules.</p></div>
  <div class="panel"><h3>60 frames per second</h3><p>With fixes for everything the 360 tied to 30 ticks, checked tick by tick against recorded films.</p></div>
  <div class="panel"><h3>Made for PC</h3><p>Direct mouse look, a pointer in the menus, your keys in the game's prompts, FOV, centered crosshair.</p></div>
  <div class="panel"><h3>Play together</h3><p>Split screen for four, and system link over your LAN or the internet, Linux and Windows together.</p></div>
  <div class="panel"><h3>Sharper</h3><p>2x and 3x the 360's resolution, FXAA, any window size, fullscreen.</p></div>
  <div class="panel"><h3>Bring your own disc</h3><p>The installer checks your disc image, translates the game's code and compiles it in a few minutes.</p></div>
</div>

<div class="panel" markdown="1">
### Where things stand

**Version 0.75:** campaign, co-op, multiplayer, Forge and Theater play
through at 60 Hz on Linux and Windows, with keyboard and mouse made for PC,
split screen and system link. **Next:** the native port, rewriting the
recompiled code as hand-written C++, subsystem by subsystem.
</div>
