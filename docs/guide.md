---
title: Player guide
nav_order: 2
---

# Player guide
{: .no_toc }

Everything the launcher and the game's PC features do, in detail.

1. TOC
{:toc}

## The launcher

`H3Launcher` (`H3Launcher.exe` on Windows), in the folder you unpacked the
release to, installs the game, changes its settings and starts it. You can
use it with the keyboard, the mouse (right-click goes back) or a gamepad,
and its layout adapts to the window size. Its menus use Halo 3's own font,
read from your installed game.

- **INSTALL** (until the game is installed): choose your disc image. The
  launcher checks it, copies the game's files into `game/` next to itself,
  translates and compiles the game's code, and prepares the shaders so the
  first start doesn't stutter on every new effect. This takes a few
  minutes; if it fails, running it again continues where it stopped.
  Afterwards the item becomes **LAUNCH**.
- **VIDEO**: window size, fullscreen (F11 or Alt+Enter switches in game),
  vsync, the render resolution (1x, as on the 360, or 2x or 3x), the frame
  rate (30, as on the 360, or 60), anti-aliasing (FXAA; Halo 3 has none of
  its own), and the PC options described under [Controls](#controls) below.
  "Precompile shaders" rebuilds the shaders you've seen before at start;
  "Background compilation" avoids the stutter the first time something new
  appears, at the cost of it being invisible for a few frames.
- **CONTROLS**: up to three keys or mouse buttons for each action, and the
  mouse options. Gamepads always use the standard layout.
- **PROFILE**: your gamertag, a gamer picture for the launcher's profile
  card (in the game, Halo 3 shows your emblem), the profile options Halo 3
  copies into a new player profile (invert look, vibration, auto-aim,
  auto-center), and the names of players 2-4 for split screen.
- **NETWORK**: system link (see [System link](#system-link)).
- **DEBUG**: unlocking every campaign mission, writing the game's console
  output to `h3recomp.log` next to `default.xex`, and the logging switches
  from the [Debugging](debugging.html) page.
- **GAME LOCATION**: run a different `default.xex`. A launcher built from
  source finds `private/default.xex` by itself (or pass the path as its
  argument).

The settings are saved to `settings.ini` next to `default.xex` (in `game/`
in an install). It's plain text and safe to edit by hand; the options are
described in `H3Recomp/config/settings.h`. The game reads it when it
starts. What the game saves to your profile (its own settings, controls and
emblem) is kept in `game/hdd/profile_settings.bin`, along with the rest of
the emulated hard drive (saves, films, screenshots).

## Controls

Player 1 uses the keyboard and mouse together with the first gamepad that's
connected. The keyboard and mouse bindings are set in the launcher; the
defaults are:

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

### The mouse in menus

Whenever a menu is open (the main menu, lobbies, the start and pause
menus), the mouse is released and works as a pointer; it's captured again
when play resumes. Pointing at an item selects it (with the D-pad's sound),
a left click chooses it (A), a right click goes back (B), and the wheel
moves up and down (or left and right, over a slider). Only the menu that
has the focus responds, not the lobby behind a popup. To turn this off, set
"Mouse in menus" to Off (CONTROLS; `menu_cursor`).

### Crouch and zoom

On the keyboard and mouse, crouch can be a toggle and zoom can be held
(CONTROLS; `toggle_crouch`, `hold_to_zoom`). The game's own behavior is to
hold to crouch, and to press zoom to step through a weapon's zoom levels and
back out. With hold to zoom, the weapon zooms to its first level while the
key is held and straight out when it's released; pressing a second zoom key
while holding the first (Mouse Right and Z by default) steps to the next
level (the sniper rifle's 10x).

### PC wording and options

While you play on the keyboard and mouse, the button icons in English menus
and prompts show the keys you've bound instead ("[Space] Select", with short
names like "[Esc]" and "[MMB]"); with a gamepad, they show its buttons.
Screens that are already open switch the next time they open. The text also
says "PC" where it said "Xbox 360 console". To get the 360's original text
and icons back, turn off "PC wording" and "Keys in prompts" (VIDEO;
`pc_wording`, `pc_key_names`).

Also on the VIDEO page:

- **Skip intro video** (`skip_intro`) goes straight to the main menu.
- **Field of view** (`fov`, 60-120; the game's own is 70) widens the view
  by scaling every character's and vehicle's camera field of view as a map
  loads. With more than one player on the PC, the game's own field of view
  is kept, as in MCC.
- **Centered crosshair** (`centered_crosshair`) puts the crosshair, and
  where you aim, in the middle of the screen instead of below it.

## Mouse look

"Mouse look: Native" in the launcher (CONTROLS; `native_mouse_look`) turns
the view directly with the mouse, as MCC's PC version does, instead of
moving the right stick (which brings the controller's response curve and
acceleration with it). A mouse sensitivity of 1.0 is 0.022 degrees per
mouse count, the usual scale for PC shooters. When you zoom in, the mouse
slows down with the magnification (by tan(zoomed FOV / 2) / tan(normal FOV /
2)), so the view moves across the screen as fast as when unzoomed. "Vehicle
mouse sensitivity" (`vehicle_mouse_sensitivity`) scales it in vehicles and
turrets.

## Split screen

Up to four players can play on one PC, as on the 360: two in campaign
co-op, four in multiplayer (Custom Games and Forge).

- **Joining:** every gamepad after the first is another controller (2-4,
  in the order they connect). A player joins by pressing START on theirs;
  until then the game doesn't see it, so a spare gamepad, or Steam's virtual
  copy of one, doesn't add a player. They're signed in with the name set in
  the launcher (PROFILE, "Player 2 gamertag" and so on; `gamertag_2` to
  `gamertag_4`), and signed-in players are in every lobby.
- **Leaving:** hold BACK and START together for two seconds (START joins
  again).
- **Unplugging:** a player whose controller is unplugged stays signed in;
  the game pauses with its reconnect message and carries on when the
  controller is back.
- **Keyboard as its own player:** with "Keyboard is its own player"
  (CONTROLS; `keyboard_own_player`), the keyboard and mouse are player 1 on
  their own, and the first gamepad becomes player 2.

Each player's game profile (controls, emblem) is saved as
`profile_settings_2.bin` to `profile_settings_4.bin`, next to player 1's.

## System link

With system link on (NETWORK; `[network]` in `settings.ini`), Custom Games
and Forge can host and find System Link games, on a LAN or over the
internet, between Linux and Windows PCs alike. It's been tested with four
players.

Each game uses one UDP port (41001 by default). On a LAN, that's all the
setup there is. Over the internet, the host forwards that port on their
router, and the other players list the host's address under Friends
(`address` or `address:port`). Only the host needs a reachable port: the
other players reach each other through the games they already know. If the
host can't forward a port, a VPN that puts everyone on one network works as
a fallback; list each other's VPN addresses instead.

Everyone needs the same release of this project, the same game version
(disc and title update) and the same frame rate setting; games that don't
match ignore each other (the log says why).

With system link on, shaders always compile in the background (as with
"Background compilation"): otherwise the first load of a map on a PC that
hasn't seen its shaders yet could stall for long enough to drop the other
players. Instead, a few objects may appear a few frames late the first time.

For the step-by-step menus, see "Playing together" in the
[README](https://github.com/george0815/halo-3-recomp#playing-together).
