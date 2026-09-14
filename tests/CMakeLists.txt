add_executable(test_wilx
    main.cpp
    DesktopTests.cpp
    ToolhelpTests.cpp
    Win32HelpersTests.cpp)
target_link_libraries(test_wilx PRIVATE wilx)
target_compile_features(test_wilx PRIVATE cxx_std_23)
target_compile_definitions(test_wilx PRIVATE UNICODE _UNICODE)
add_test(NAME wilx COMMAND test_wilx)

add_executable(dump_desktops dump_desktops.cpp)
target_link_libraries(dump_desktops PRIVATE wilx)
target_compile_features(dump_desktops PRIVATE cxx_std_23)
target_compile_definitions(dump_desktops PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(dump_desktops PRIVATE /W4)
add_test(NAME dump_desktops COMMAND dump_desktops)
