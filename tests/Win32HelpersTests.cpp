#include <windows.h>

#include <wilx/win32_helpers.h>

#include <string>
#include <type_traits>

#include "common.h"

bool Win32HelpersTests()
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

    // Error-code cores: HRESULT, and always a Win32 one when it can be known.
    std::wstring desktopName;
    CHECK(SUCCEEDED(wilx::GetThreadDesktopNameNoThrow(GetCurrentThreadId(), desktopName)));
    CHECK(!desktopName.empty());
    std::wstring badName;
    const HRESULT badHr = wilx::GetThreadDesktopNameNoThrow(0, badName);
    CHECK(FAILED(badHr));
    CHECK(badName.empty());
    CHECK(HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE) == badHr);
    CHECK(wilx::TryGetThreadDesktopName(GetCurrentThreadId()) == desktopName);

    std::wstring inputName;
    CHECK(SUCCEEDED(wilx::GetInputDesktopNameNoThrow(inputName)));
    CHECK(!inputName.empty());

#ifdef WIL_ENABLE_EXCEPTIONS
    // The exception regime: the same queries with the undecorated names.
    CHECK(wilx::GetThreadDesktopName(GetCurrentThreadId()) == desktopName);
    CHECK(wilx::GetThreadDesktopName() == desktopName);
    CHECK(wilx::GetInputDesktopName() == inputName);
    CHECK(wilx::GetUserObjectName(GetProcessWindowStation()) == wilx::TryGetProcessWindowStationName());

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

    static_assert(std::is_base_of_v<NOTIFYICONDATAW, wilx::unique_notify_icon_data>);
    wilx::unique_notify_icon_data icon;
    icon.cbSize = sizeof(icon);
    CHECK(icon.szTip[0] == L'\0');

    return true;
}
