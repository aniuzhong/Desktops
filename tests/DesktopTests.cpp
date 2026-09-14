#include <windows.h>

#include <wilx/desktop.h>

#include <wil/resource.h>

#include <string>
#include <vector>

#include "common.h"

bool DesktopTests()
{
    std::vector<std::wstring> names;
    wilx::for_each_desktop_nothrow([&](PCWSTR name) { names.emplace_back(name); });
    CHECK(!names.empty());

    size_t visited = 0;
    wilx::for_each_desktop_nothrow([&](PCWSTR) -> bool { ++visited; return false; });
    CHECK(visited == 1);

    std::vector<std::wstring> stationNames;
    wilx::for_each_window_station_nothrow([&](PCWSTR name) { stationNames.emplace_back(name); });
    CHECK(!stationNames.empty());

    wil::unique_hdesk inputDesktop(OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS));
    CHECK(inputDesktop);
    size_t windowsSeen = 0;
    wilx::for_each_desktop_window_nothrow(inputDesktop.get(), [&](HWND) { ++windowsSeen; });
    CHECK(windowsSeen > 0);

    return true;
}
