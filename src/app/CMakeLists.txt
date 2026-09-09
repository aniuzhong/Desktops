set(WILX_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/app.ico")
set(WILX_APP_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/app.manifest")
configure_file(app.rc.in app.rc @ONLY)

add_executable(VirtualDesktop WIN32
    main.cpp
    panel.cpp
    runner_dialog.cpp
    runner_events.cpp
    runner.cpp
    common.cpp
    logging.cpp
    panel.h
    runner_dialog.h
    runner_events.h
    runner.h
    common.h
    logging.h
    resource.h
    app.manifest
    "${CMAKE_CURRENT_BINARY_DIR}/app.rc"
)
target_include_directories(VirtualDesktop PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(VirtualDesktop PRIVATE wilx spdlog::spdlog shlwapi)
target_compile_features(VirtualDesktop PRIVATE cxx_std_23)
target_compile_definitions(VirtualDesktop PRIVATE UNICODE _UNICODE _WINDOWS)
target_compile_options(VirtualDesktop PRIVATE /W4 /utf-8)
# The .rc embeds app.manifest (common controls v6, PerMonitorV2); letting the
# linker also generate one would add a second, conflicting RT_MANIFEST resource.
target_link_options(VirtualDesktop PRIVATE "/MANIFEST:NO")
set_target_properties(VirtualDesktop PROPERTIES
    OUTPUT_NAME "Virtual Desktop"
    VS_GLOBAL_KEYWORD "Win32Proj"
)
set_property(TARGET VirtualDesktop PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
