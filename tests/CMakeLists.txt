add_executable(test_wilx
    main.cpp
    DesktopTests.cpp
    ToolhelpTests.cpp)
target_link_libraries(test_wilx PRIVATE wilx)
target_compile_features(test_wilx PRIVATE cxx_std_23)
target_compile_definitions(test_wilx PRIVATE UNICODE _UNICODE)
add_test(NAME wilx COMMAND test_wilx)
