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

# Windows: the DLLs it loads (Qt, SDL3) and Qt's platform and style plugins
# go next to the executable.
if (WIN32)
    add_custom_command(TARGET H3Launcher POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:H3Launcher> $<TARGET_FILE_DIR:H3Launcher>
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:H3Launcher>/platforms $<TARGET_FILE_DIR:H3Launcher>/styles
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Qt6::QWindowsIntegrationPlugin> $<TARGET_FILE_DIR:H3Launcher>/platforms
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Qt6::QModernWindowsStylePlugin> $<TARGET_FILE_DIR:H3Launcher>/styles
        COMMAND_EXPAND_LISTS)
endif()
