---
title: Player guide
nav_order: 2
---

# Player guide

Everything the launcher and the game's PC features do, in detail.

## Launcher

`build/release/H3Launcher/H3Launcher` edits the settings and starts the game.
Keyboard, mouse (right-click goes back) or a gamepad; the layout follows the
window size.
It finds `private/default.xex` by itself (or use GAME LOCATION, or pass the
path as its argument). Its menus use Halo 3's own font, read from the game's
`maps/fonts/font_package.bin`.

- **Video**: window size, fullscreen (F11 or Alt+Enter toggles it in game),
  vsync, the render resolution (1x native 1280x720, 2x or 3x), anti-aliasing
  (FXAA; Halo 3 itself has none), precompiling shaders at start, and background shader compilation (no stutter the first
  time something new appears, but it can be missing for a few frames).
- **Controls**: up to three keys or mouse buttons per action, mouse
  sensitivity and inversion. Gamepads always use the standard layout.
- **Debug**: the logging and tracing switches below (set in `settings.ini`
  and applied by the game at start, so they work without the launcher too),
  writing the console output to `private/h3recomp.log`, and unlocking every
  campaign mission.
- **Profile**: gamertag, a gamer picture (shown on the launcher's profile
  card; Halo 3 itself shows your emblem), and the profile options Halo 3
  copies into a new player profile (invert look, vibration, auto-aim,
  auto-center).

Settings are saved to `settings.ini` next to `default.xex` (plain text, safe
to edit by hand; see `H3Recomp/config/settings.h`), and the game reads them
at start. Whatever the game saves to your profile (its own settings, controls
and emblem) persists in `private/hdd/profile_settings.bin`.

## Controls

Controller 1 is the keyboard and mouse merged with the first gamepad SDL
finds. The keyboard and mouse bindings are set in the launcher; the
defaults:

| Key | Controller |
|---|---|
| W A S D | Left stick (move) |
| I J K L | Right stick (look) |
| Arrow keys | D-pad (menus) |
| Space, Enter | A |
| Backspace, V | B |
| F | X |
| Q | Y |
| G / R | Left / right bumper |
| Left Shift | Left trigger |
| Left Ctrl, E | Right trigger |
| C / Z | Left / right stick click |
| Escape | Start |
| Tab | Back |
| Mouse | Right stick (look); click the window to capture the mouse, F1 releases it |
| Left mouse button | Right trigger (fire) |
| Right mouse button | Right stick click (zoom) |
| Middle mouse button | B (melee) |
| Mouse wheel | Y (switch weapons) |

In menus the mouse is a pointer: the mouse is released while a menu is
open (main menu, lobbies, the start menu, the pause menu) and captured again
when play resumes. Pointing at an item selects it (with the D-pad's sound),
a left click chooses it (A), a right click goes back (B) and the wheel
moves up and down (D-pad; over a slider, left and right).
Only the menu that has the input responds (not the lobby behind a popup or
the start menu). Off with "Mouse in menus" (launcher, Controls;
`menu_cursor`).

Crouch can be a toggle and zoom a hold on the keyboard and mouse (launcher,
Controls; `toggle_crouch`, `hold_to_zoom`): the game's own are hold to
crouch and press to step through a weapon's zoom levels and out. Hold to
zoom zooms to the first level while the key is held and straight out when
it's let go; pressing a second zoom key while holding one (Mouse Right and
Z by default) steps to the next level (a sniper rifle's 10x).
Keys in English prompts use short names ("[Esc]", "[MMB]"). For the 360's
own text and button icons, turn off "PC wording" and "Keys in prompts"
(launcher, Video; `pc_wording`, `pc_key_names`). "Skip intro video" (Video;
`skip_intro`) goes straight to the main menu. "Field of view" (Video; `fov`,
60-120, the game's 70) widens the view by scaling every biped's and
vehicle's camera FOV as a map loads; with more than one player on the PC
the game's own FOV stays, as in MCC. "Centered crosshair" (Video;
`centered_crosshair`) puts the crosshair, and the aim, in the middle of the
screen instead of below it.

While you play on the keyboard and mouse, the button icons in English menus
and prompts show the keys bound to them ("[Space] Select"); with a gamepad,
the buttons. Screens already open switch the next time they're opened. The
text also says "PC" where it said "Xbox 360 console" (`H3_NO_PC_TEXT=1`
keeps the original wording).

### Split screen

Up to four players, as on the 360: two in campaign co-op, four in
multiplayer (Custom Games, Forge). Gamepads after the first are controllers
2-4 in the order they connect; a controller's player joins by pressing
START (until then the game doesn't see it, so a spare gamepad or Steam's
virtual copy of one adds no player) and is signed in with the profile
named in the launcher ("Player 2 gamertag", `gamertag_2` to `gamertag_4` in
`[profile]`). Signed-in players are in every lobby. With "Keyboard is its
own player" (`keyboard_own_player` in `[controls]`) the keyboard and mouse
are player 1 alone and the first gamepad is player 2. A player whose
controller is unplugged stays signed in: the game pauses with its
reconnect message and goes on when it's back. Holding BACK and START
together for two seconds signs a player out (START joins again). Each
player's game profile (controls, emblem) is saved to
`profile_settings_2.bin` to `_4.bin` next to player 1's.

### Mouse look

"Mouse look: Native" in the launcher (`native_mouse_look` in `[controls]`)
turns the view straight from the mouse, as MCC's PC version does, instead of
driving the right stick (which brings the controller's response curve and
acceleration). Mouse sensitivity is 0.022 degrees per count at 1.0, the
usual PC shooter scale. Zoomed in, the mouse slows with the magnification
(by tan(zoomed FOV / 2) / tan(normal FOV / 2): the view moves across the
screen as fast as unzoomed), and "Vehicle mouse sensitivity"
(`vehicle_mouse_sensitivity`) scales it in vehicles and turrets.

### System link

With System link on (launcher, Network; `[network]` in settings.ini) Custom
Games and Forge can host and find System Link games. Each game uses one UDP
port (41001 by default): on a LAN that's all; over the internet, list
friends' addresses (`host` or `host:port`) and have the host forward the
port (only the host needs a reachable port: other players reach each
other through the games they know). Everyone needs the same version of
this project, the same game executable (disc and title update), the same
frame rate setting and the 60 Hz fixes on or off alike; packets from other
games are ignored and logged. Finding, joining and playing work
(tested with three games on one PC). With system link on, shaders compile
in the background (as with "Background shader compilation"): a map's first
load on a PC that hasn't seen its shaders would otherwise stall long enough
to drop the others; instead some objects show up a few frames late the
first time.