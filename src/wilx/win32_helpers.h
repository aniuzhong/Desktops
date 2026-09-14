//*********************************************************
//
//    Copyright (c) the Desktops authors.
//    Licensed under the MIT License.
//
//    wilx - WIL-style extensions for Virtual Desktop.
//    Header-only, shaped after wil: machinery earns a
//    header (see desktop.h), single-pattern helpers live
//    in this drawer. API contracts: wilx/README.md.
//    Comments here only explain choices the code cannot show.
//
//*********************************************************
//! @file
//! The wilx drawer: user-object name queries (Get*/TryGet* plus the
//! NoThrow error-code cores), Win32 error messages, UTF-8
//! conversion, window text, window owner queries, and the tray icon RAII.
//! TryGet* here is total fail-soft (see wilx/README.md).
#ifndef __WILX_WIN32_HELPERS_INCLUDED
#define __WILX_WIN32_HELPERS_INCLUDED

#include <WinUser.h>      // GetThreadDesktop, OpenInputDesktop, window text
#include <minwindef.h>    // DWORD, HANDLE, HRESULT, LPARAM
#include <shellapi.h>     // Shell_NotifyIconW
#include <stringapiset.h> // WideCharToMultiByte
#include <winbase.h>      // FormatMessageW

#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include <wil/resource.h>
#include <wil/result_macros.h> // THROW_IF_FAILED: the exception regime's currency

namespace wilx
{
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
//! Error-code cores behind the TryGet* name queries: callers that render or
//! classify the failure (the desktop dump) need the error, which the total
//! fail-soft TryGet* regime deliberately discards. Failure is returned as
//! wil's error-code currency, an HRESULT, and it is always a Win32 one
//! (HRESULT_FROM_WIN32), so HRESULT_CODE recovers what GetLastError would
//! have said - 0 is replaced by ERROR_INVALID_HANDLE because these user-object
//! APIs fail without touching last error for null handles.
inline HRESULT GetUserObjectNameNoThrow(
    _In_opt_ HANDLE userObject, _Out_ std::wstring& name) WI_NOEXCEPT
{
    name.clear();

    if (!userObject)
    {
        return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);
    }

    DWORD bytesNeeded = 0;
    SetLastError(0);
    if (!GetUserObjectInformationW(userObject, UOI_NAME, nullptr, 0, &bytesNeeded) &&
        ERROR_INSUFFICIENT_BUFFER != GetLastError())
    {
        const DWORD lastError = GetLastError();
        return HRESULT_FROM_WIN32(0 == lastError ? ERROR_INVALID_HANDLE : lastError);
    }

    bool filled = false;
    DWORD failure = 0;
    name.resize_and_overwrite(bytesNeeded / sizeof(wchar_t) + 1, [&](wchar_t* buffer, size_t capacity) {
        if (!GetUserObjectInformationW(userObject, UOI_NAME, buffer,
            static_cast<DWORD>(capacity * sizeof(wchar_t)), &bytesNeeded))
        {
            failure = GetLastError();
            return size_t{};
        }
        filled = true;
        return std::wcslen(buffer);
    });
    if (!filled)
    {
        return HRESULT_FROM_WIN32(0 == failure ? ERROR_INVALID_HANDLE : failure);
    }
    return S_OK;
}

inline std::wstring TryGetUserObjectName(_In_ HANDLE userObject)
{
    std::wstring name;
    (void)GetUserObjectNameNoThrow(userObject, name);
    return name;
}

#ifdef WIL_ENABLE_EXCEPTIONS
inline std::wstring GetUserObjectName(_In_opt_ HANDLE userObject)
{
    std::wstring name;
    THROW_IF_FAILED(GetUserObjectNameNoThrow(userObject, name));
    return name;
}
#endif

//! GetThreadDesktop handle is owned by the thread: never CloseDesktop it.
//! GetThreadDesktop fails silently (NULL, last error untouched) for threads
//! it will not name — cross-session and pseudo entries — so capture its
//! error explicitly and substitute when it did not set one.
inline HRESULT GetThreadDesktopNameNoThrow(_In_ DWORD threadId, _Out_ std::wstring& name) WI_NOEXCEPT
{
    SetLastError(0);
    const HANDLE desktop = GetThreadDesktop(threadId);
    const DWORD lastError = GetLastError();
    if (!desktop)
    {
        name.clear();
        return HRESULT_FROM_WIN32(0 == lastError ? ERROR_INVALID_HANDLE : lastError);
    }
    return GetUserObjectNameNoThrow(desktop, name);
}

inline std::wstring TryGetThreadDesktopName(_In_ DWORD threadId)
{
    std::wstring name;
    (void)GetThreadDesktopNameNoThrow(threadId, name);
    return name;
}

inline std::wstring TryGetThreadDesktopName()
{
    return TryGetThreadDesktopName(GetCurrentThreadId());
}

#ifdef WIL_ENABLE_EXCEPTIONS
inline std::wstring GetThreadDesktopName(_In_ DWORD threadId)
{
    std::wstring name;
    THROW_IF_FAILED(GetThreadDesktopNameNoThrow(threadId, name));
    return name;
}

inline std::wstring GetThreadDesktopName()
{
    return GetThreadDesktopName(GetCurrentThreadId());
}
#endif

//! OpenInputDesktop result is owned here: never CloseDesktop it from outside.
inline HRESULT GetInputDesktopNameNoThrow(_Out_ std::wstring& name) WI_NOEXCEPT
{
    wil::unique_hdesk inputDesktop(OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS));
    if (!inputDesktop)
    {
        name.clear();
        return HRESULT_FROM_WIN32(GetLastError());
    }
    return GetUserObjectNameNoThrow(inputDesktop.get(), name);
}

inline std::wstring TryGetInputDesktopName()
{
    std::wstring name;
    (void)GetInputDesktopNameNoThrow(name);
    return name;
}

#ifdef WIL_ENABLE_EXCEPTIONS
inline std::wstring GetInputDesktopName()
{
    std::wstring name;
    THROW_IF_FAILED(GetInputDesktopNameNoThrow(name));
    return name;
}
#endif

struct WindowThreadProcessId
{
    DWORD threadId;
    DWORD processId;
};

//! Mirrors GetThreadProcessId: a zero thread id means the window is invalid
//! and both fields are meaningless.
inline std::optional<WindowThreadProcessId> TryGetWindowThreadProcessId(_In_opt_ HWND window)
{
    DWORD processId = 0;
    const DWORD threadId = ::GetWindowThreadProcessId(window, &processId);
    if (0 == threadId)
    {
        return std::nullopt;
    }
    return WindowThreadProcessId{threadId, processId};
}

inline std::wstring TryGetProcessWindowStationName()
{
    return TryGetUserObjectName(GetProcessWindowStation());
}
#endif // WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)

// Not partitioned: FormatMessageW and WideCharToMultiByte are available to
// every partition, including app containers.

inline std::wstring TryGetWin32ErrorMessage(DWORD error)
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
inline std::wstring TryGetWin32ErrorMessage()
{
    return TryGetWin32ErrorMessage(GetLastError());
}

//! Shrinks to the bytes actually written: a failed conversion yields an
//! empty string, never a stale buffer. This is the drawer's only [[nodiscard]]:
//! it is a pure conversion, so discarding the result discards the whole call -
//! wil marks exactly this kind of helper (`compare_string_ordinal`,
//! `get_module_reference_for_thread`), and leaves getters like
//! GetModuleFileNameW undecorated.
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

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
//! GetWindowTextW does not distinguish "no text" from failure; neither does this.
inline std::wstring TryGetWindowText(_In_ HWND window)
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
#endif // WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
} // namespace wilx
#endif // __WILX_WIN32_HELPERS_INCLUDED
