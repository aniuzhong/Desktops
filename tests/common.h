// wilx behavior tests: run via ctest, or the test_wilx executable directly.
// One file per wilx header (wil's layout: WindowingTests.cpp, ResourceTests.cpp,
// ...); each exposes one runner that main.cpp drives.
#pragma once

#include <cstdio>

// Prints the failing line and abandons the runner: every check after a failed
// one rests on the guarantees it established, so continuing says nothing.
#define CHECK(x)                                                                             \
    do                                                                                       \
    {                                                                                        \
        if (!(x))                                                                            \
        {                                                                                    \
            std::printf("CHECK failed: %s (%s:%d)\n", #x, __FILE__, __LINE__);               \
            return false;                                                                    \
        }                                                                                    \
    } while (0)

bool DesktopTests();
bool ToolhelpTests();
