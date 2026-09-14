//*********************************************************
//
//    wilx - WIL-style extensions for Virtual Desktop.
//    Header-only, shaped after wil: machinery earns a
//    header (see desktops.h), single-pattern helpers live
//    in this drawer. API contracts: wilx/README.md.
//    Comments here only explain choices the code cannot show.
//
//*********************************************************
//! @file
//! The wilx drawer: user-object name queries (TryGet* wrappers plus the
//! NoThrow cores that carry the error code), Win32 error messages, UTF-8
//! conversion, window text, window owner queries, and the tray icon RAII.
//! TryGet* here is total fail-soft (see wilx/README.md).
#ifndef __WILX_WIN32_HELPERS_INCLUDED
#define __WILX_WIN32_HELPERS_INCLUDED

#include <windows.h>
#include <shellapi.h>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include <wil/resource.h>

namespace wilx
{
//! NoThrow core behind the TryGet* name queries: callers that render or
//! classify the failure (the desktop dump) need the error code, which the
//! total fail-soft TryGet* regime deliberately discards. false == failure
//! with *lastError set (0 is replaced: these user-object APIs fail without
//! touching last error for null handles); true == *name filled (empty names
//! do not occur for window stations or desktops).
[[nodiscard]] inline bool GetUserObjectNameNoThrow(
    _In_opt_ HANDLE userObject, _Out_ std::wstring& name, _Out_ DWORD& lastError)
{
    name.clear();

    if (!userObject)
    {
        lastError = ERROR_INVALID_HANDLE;
        return false;
    }

    DWORD bytesNeeded = 0;
    SetLastError(0);
    if (!GetUserObjectInformationW(userObject, UOI_NAME, nullptr, 0, &bytesNeeded) &&
        ERROR_INSUFFICIENT_BUFFER != GetLastError())
    {
        lastError = GetLastError();
        if (0 == lastError)
            lastError = ERROR_INVALID_HANDLE;
        return false;
    }

    bool filled = false;
    name.resize_and_overwrite(bytesNeeded / sizeof(wchar_t) + 1, [&](wchar_t* buffer, size_t capacity) {
        if (!GetUserObjectInformationW(userObject, UOI_NAME, buffer,
            static_cast<DWORD>(capacity * sizeof(wchar_t)), &bytesNeeded))
        {
            lastError = GetLastError();
            if (0 == lastError)
                lastError = ERROR_INVALID_HANDLE;
            return size_t{};
        }
        filled = true;
        return std::wcslen(buffer);
    });
    if (!filled)
    {
        return false;
    }
    lastError = 0; // success must not leak the probe call's expected ERROR_INSUFFICIENT_BUFFER
    return true;
}

[[nodiscard]] inline std::wstring TryGetUserObjectName(_In_ HANDLE userObject)
{
    std::wstring name;
    DWORD error = 0;
    (void)GetUserObjectNameNoThrow(userObject, name, error);
    return name;
}

//! GetThreadDesktop handle is owned by the thread: never CloseDesktop it.
//! GetThreadDesktop fails silently (NULL, last error untouched) for threads
//! it will not name — cross-session and pseudo entries — so capture its
//! error explicitly and substitute when it did not set one.
[[nodiscard]] inline bool GetThreadDesktopNameNoThrow(_In_ DWORD threadId, _Out_ std::wstring& name, _Out_ DWORD& lastError)
{
    SetLastError(0);
    const HANDLE desktop = GetThreadDesktop(threadId);
    lastError = GetLastError();
    if (!desktop)
    {
        name.clear();
        if (0 == lastError)
            lastError = ERROR_INVALID_HANDLE;
        return false;
    }
    return GetUserObjectNameNoThrow(desktop, name, lastError);
}

[[nodiscard]] inline std::wstring TryGetThreadDesktopName(_In_ DWORD threadId)
{
    std::wstring name;
    DWORD error = 0;
    (void)GetThreadDesktopNameNoThrow(threadId, name, error);
    return name;
}

[[nodiscard]] inline std::wstring TryGetThreadDesktopName()
{
    return TryGetThreadDesktopName(GetCurrentThreadId());
}

//! OpenInputDesktop result is owned here: never CloseDesktop it from outside.
[[nodiscard]] inline bool GetInputDesktopNameNoThrow(_Out_ std::wstring& name, _Out_ DWORD& lastError)
{
    wil::unique_hdesk inputDesktop(OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS));
    if (!inputDesktop)
    {
        name.clear();
        lastError = GetLastError();
        return false;
    }
    return GetUserObjectNameNoThrow(inputDesktop.get(), name, lastError);
}

[[nodiscard]] inline std::wstring TryGetInputDesktopName()
{
    std::wstring name;
    DWORD error = 0;
    (void)GetInputDesktopNameNoThrow(name, error);
    return name;
}

struct WindowThreadProcessId
{
    DWORD threadId;
    DWORD processId;
};

//! Mirrors GetThreadProcessId: a zero thread id means the window is invalid
//! and both fields are meaningless.
[[nodiscard]] inline std::optional<WindowThreadProcessId> TryGetWindowThreadProcessId(_In_opt_ HWND window)
{
    DWORD processId = 0;
    const DWORD threadId = ::GetWindowThreadProcessId(window, &processId);
    if (0 == threadId)
    {
        return std::nullopt;
    }
    return WindowThreadProcessId{threadId, processId};
}

[[nodiscard]] inline std::wstring TryGetProcessWindowStationName()
{
    return TryGetUserObjectName(GetProcessWindowStation());
}

[[nodiscard]] inline std::wstring TryGetWin32ErrorMessage(DWORD error)
{
    wil::unique_hlocal buffer;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(buffer.put()), 0, nullptr);
    if (!buffer)
    {
        return {};
    }

    std::wstring message(static_cast<PWSTR>(buffer.get()));
    while (!message.empty() && (message.back() == L'\n' || message.back() == L'\r'))
    {
        message.pop_back();
    }
    return message;
}

//! Reads the calling thread's last error; call immediately after the failing
//! API, before anything clobbers it.
[[nodiscard]] inline std::wstring TryGetWin32ErrorMessage()
{
    return TryGetWin32ErrorMessage(GetLastError());
}

//! Shrinks to the bytes actually written: a failed conversion yields an
//! empty string, never a stale buffer.
[[nodiscard]] inline std::string TryGetUtf8String(std::wstring_view text)
{
    if (text.empty())
    {
        return {};
    }

    const int byteCount = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (byteCount <= 0)
    {
        return {};
    }

    std::string utf8;
    utf8.resize_and_overwrite(static_cast<size_t>(byteCount), [&text](char* buffer, size_t capacity) {
        return static_cast<size_t>(WideCharToMultiByte(CP_UTF8, 0, text.data(),
            static_cast<int>(text.size()), buffer, static_cast<int>(capacity), nullptr, nullptr));
    });
    return utf8;
}

//! GetWindowTextW does not distinguish "no text" from failure; neither does this.
[[nodiscard]] inline std::wstring TryGetWindowText(_In_ HWND window)
{
    const int length = GetWindowTextLengthW(window);
    if (length <= 0)
    {
        return {};
    }

    std::wstring text;
    text.resize_and_overwrite(static_cast<size_t>(length) + 1, [window](wchar_t* buffer, size_t capacity) {
        GetWindowTextW(window, buffer, static_cast<int>(capacity));
        return std::wcslen(buffer);
    });
    return text;
}

namespace details
{
    inline void __stdcall DeleteNotifyIcon(_In_ NOTIFYICONDATAW* data) WI_NOEXCEPT
    {
        Shell_NotifyIconW(NIM_DELETE, data);
    }
} // namespace details

//! Always-clear: NIM_DELETE on a never-added icon fails harmlessly
//! (wil's unique_prop_variant semantics).
using unique_notify_icon_data =
    wil::unique_struct<NOTIFYICONDATAW, decltype(&details::DeleteNotifyIcon), details::DeleteNotifyIcon>;
} // namespace wilx
#endif // __WILX_WIN32_HELPERS_INCLUDED
