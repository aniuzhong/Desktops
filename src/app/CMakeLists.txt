# The Desktops application: manager (panel process) + per-desktop dock
# processes (same exe, --dock mode). See doc/simplified-plan.md.
set(DESKTOPS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/desktops.ico")
configure_file(app.rc.in app.rc @ONLY)

find_package(Qt6 COMPONENTS Widgets Network Svg REQUIRED)
qt_standard_project_setup()
set(CMAKE_AUTORCC ON)

qt_add_executable(Desktops WIN32
    main.cpp
    mainwindow.cpp
    dock.cpp
    mainwindow.h
    dock.h
    desktops.qrc
    app.rc
)
target_include_directories(Desktops PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Desktops PRIVATE Qt6::Widgets Qt6::Network Qt6::Svg wilx)
target_compile_features(Desktops PRIVATE cxx_std_23)
target_compile_definitions(Desktops PRIVATE UNICODE _UNICODE _WINDOWS)
target_compile_options(Desktops PRIVATE /W4 /utf-8)
# Common-Controls v6 for the modern widget style, injected into the linker-
# generated manifest (there is no manifest resource in this project). Qt 6
# enables PerMonitorV2 DPI awareness itself.
target_link_options(Desktops PRIVATE
    "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' publicKeyToken='6595b64144ccf1df' language='*' processorArchitecture='*'"
)
set_target_properties(Desktops PROPERTIES
    OUTPUT_NAME "Desktops"
    VS_GLOBAL_KEYWORD "Win32Proj"
)
set_property(TARGET Desktops PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
