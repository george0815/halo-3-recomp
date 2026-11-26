# Subsystem map (coarse, .text)

Which part of the game each range of the executable's code belongs to, for
tools/re/progress.py (the progress page's per-subsystem numbers). MSVC
places each object file's functions together, roughly in source tree order;
the ranges come from named functions and the surviving `__FILE__` strings.
Approximate: ranges overlap in places (the first matching row wins), and
code between ranges counts as "other".

| Range (approx.) | Subsystem | Evidence |
|---|---|---|
| 0x82080000-0x820B0000 | interface, game, objects (early), HaloScript core | user_interface.cpp, hs_thread_return_value |
| 0x820E0000-0x82120000 | objects, units, players | object_*, unit_*, players_* |
| 0x82138000-0x82148000 | sound (XMA voices), rasterizer sync, particles | xma_voice_*, rasterizer_wait_for_fence |
| 0x82192000-0x821AB000 | rasterizer present, main loop | rasterizer_swap, main.cpp, main_loop |
| 0x821B0000-0x821E3000 | game, game time, saved films | game.cpp, game_time_*, saved_film_*.cpp |
| 0x821F7000-0x82220000 | players, sound, camera, game_engine, campaign | game_engine.cpp, sound_* |
| 0x82219000 | crash dumps | xenon_minidump.cpp |
| 0x82223000-0x82245000 | AI (part), physics, cinematics, effects | effects.cpp, cinematic_* |
| 0x8224D000-0x822DC000 | HaloScript handlers (hs_*) | the script function table |
| 0x82284000-0x822F8000 | saved games, content catalogue, storage | content_catalogue*.cpp, c_storage_device.cpp |
| 0x82300000-0x82365000 | cinematics, objects, units, vehicles, devices, scenery, weapons | weapons.cpp, unit_*, device_*, vehicle_* |
| 0x8239D000-0x82445000 | networking (online, sessions, statistics), saved films | online_*.cpp, network_session_interface.cpp |
| 0x82446000-0x8246C000 | rasterizer (Xenon), textures | rasterizer_xenon_main.cpp, texture_* |
| 0x82470000-0x824BB000 | campaign progress, HUD, GUI screens, profiles | chud_*, gui_*.cpp, c_player_profile*.cpp |
| 0x824F0000-0x82515000 | AI (squads, actors), UI mission list | ai_* (40+), cs_*, flock_* |
| 0x8254F000-0x825A0000 | AI (rest), CRT | memset |
| 0x8259F000-0x825B8000 | XAPI fibers, XMemCpy, Direct3D 9 (static lib) | D3D_* |
| 0x8261E000-0x826AA000 | Havok physics | hk*.cpp |
| 0x826C0000- | XMA / XAudio | xma_kick_contexts |
