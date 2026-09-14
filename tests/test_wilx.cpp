// wilx behavior tests: run via ctest, or the test_wilx executable directly.
#include <windows.h>
#include <wilx/desktops.h>
#include <wilx/desktop_windows.h>
#include <wilx/toolhelp.h>
#include <wilx/window_stations.h>
#include <wilx/win32_helpers.h>

#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

#define CHECK(x) \
    do { if (!(x)) { std::printf("CHECK failed: %s (%s:%d)\n", #x, __FILE__, __LINE__); return 1; } } while (0)

int main()
{
    // TryGet* is total fail-soft: empty == failure, never an exception.
    CHECK(!wilx::TryGetWin32ErrorMessage(ERROR_FILE_NOT_FOUND).empty());
    CHECK(wilx::TryGetUtf8String(L"").empty());
    CHECK(wilx::TryGetUtf8String(L"abc") == "abc");
    CHECK(wilx::TryGetUtf8String(L"\x4f60\x597d") == "\xe4\xbd\xa0\xe5\xa5\xbd");
    CHECK(wilx::TryGetWindowText(nullptr).empty());

    CHECK(!wilx::TryGetUserObjectName(GetProcessWindowStation()).empty());
    CHECK(!wilx::TryGetThreadDesktopName().empty());

    HWND window = CreateWindowExW(0, L"STATIC", L"hello", 0, 0, 0, 0, 0,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    CHECK(window != nullptr);
    CHECK(wilx::TryGetWindowText(window) == L"hello");

    auto owner = wilx::TryGetWindowThreadProcessId(window);
    CHECK(owner && owner->threadId == GetCurrentThreadId());
    CHECK(owner && owner->processId == GetCurrentProcessId());
    CHECK(!wilx::TryGetWindowThreadProcessId(nullptr).has_value());
    DestroyWindow(window);

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

    bool foundSelfProcess = false;
    wilx::for_each_process([&](const PROCESSENTRY32W& entry) {
        foundSelfProcess = foundSelfProcess || entry.th32ProcessID == GetCurrentProcessId();
        return true;
    });
    CHECK(foundSelfProcess);

    bool foundSelfThread = false;
    wilx::for_each_thread([&](const THREADENTRY32& entry) {
        foundSelfThread = foundSelfThread || entry.th32ThreadID == GetCurrentThreadId();
        return true;
    });
    CHECK(foundSelfThread);

    std::wstring desktopName;
    DWORD nameError = 0;
    CHECK(wilx::GetThreadDesktopNameNoThrow(GetCurrentThreadId(), desktopName, nameError));
    CHECK(!desktopName.empty() && nameError == 0);
    std::wstring badName;
    DWORD badError = 0;
    CHECK(!wilx::GetThreadDesktopNameNoThrow(0, badName, badError));
    CHECK(badName.empty() && badError != 0);
    CHECK(wilx::TryGetThreadDesktopName(GetCurrentThreadId()) == desktopName);

    static_assert(std::is_base_of_v<NOTIFYICONDATAW, wilx::unique_notify_icon_data>);
    wilx::unique_notify_icon_data icon;
    icon.cbSize = sizeof(icon);
    CHECK(icon.szTip[0] == L'\0');

    return 0;
}
