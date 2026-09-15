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

    // Name queries: total fail-soft, error-code, and exception regimes.
    const std::wstring currentDesktop = wilx::TryGetThreadDesktopName();
    CHECK(!currentDesktop.empty());

    std::wstring named;
    CHECK(SUCCEEDED(wilx::GetThreadDesktopNameNoThrow(GetCurrentThreadId(), named)));
    CHECK(named == currentDesktop);
    CHECK(SUCCEEDED(wilx::GetUserObjectNameNoThrow(GetProcessWindowStation(), named)));

    std::wstring badName;
    const HRESULT badHr = wilx::GetThreadDesktopNameNoThrow(0, badName);
    CHECK(FAILED(badHr));
    CHECK(badName.empty());
    CHECK(HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE) == badHr);
    CHECK(wilx::TryGetThreadDesktopName(GetCurrentThreadId()) == currentDesktop);

#ifdef WIL_ENABLE_EXCEPTIONS
    CHECK(wilx::GetThreadDesktopName(GetCurrentThreadId()) == currentDesktop);
    CHECK(wilx::GetThreadDesktopName() == currentDesktop);

    bool threw = false;
    try
    {
        (void)wilx::GetThreadDesktopName(0);
    }
    catch (...)
    {
        threw = true;
    }
    CHECK(threw);
#endif

    return true;
}
