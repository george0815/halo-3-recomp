# H3Launcher: the Qt front end that edits settings.ini (window, render
# scale, profile, controls) and starts H3Recomp. Optional: only built when
# Qt 6 Widgets is installed. Doesn't need the recompiled game.

add_executable(H3Launcher
    main.cpp
    halo_font.cpp
    key_names.cpp
    launcher_window.cpp
    resources.qrc   # the background art, embedded
)
set_target_properties(H3Launcher PROPERTIES AUTORCC ON)
find_package(SDL3 REQUIRED CONFIG)  # gamepad navigation
target_link_libraries(H3Launcher PRIVATE H3Config Qt6::Widgets SDL3::SDL3)
