//*********************************************************
//
//    Copyright (c) the Desktops authors.
//    Licensed under the MIT License.
//
//    wilx - WIL-style extensions for Virtual Desktop.
//    API contracts: wilx/README.md. Comments here only
//    explain choices the code cannot show.
//
//*********************************************************
//! @file
//! The window station / desktop subsystem: enumeration of the session's window
//! stations, their desktops, and the windows on one desktop — the three
//! user-object families Win32 exposes through Enum* APIs — plus the name
//! queries that read a desktop's identity back and the one that places a
//! process in its session. All three enumerators share
//! the trampoline in details, the way wil's windowing.h shares one for its
//! three window enumerators; it stays there until a second API family needs it.
#ifndef __WILX_DESKTOP_INCLUDED
#define __WILX_DESKTOP_INCLUDED

#include <WinUser.h>   // EnumDesktopsW, EnumDesktopWindows, GetProcessWindowStation, GetThreadDesktop
#include <minwindef.h> // HDESK, HWINSTA, LPARAM
#include <processthreadsapi.h> // GetCurrentProcessId, ProcessIdToSessionId

#include <concepts>
#include <cwchar>
#include <exception>
#include <string>
#include <type_traits>
#include <utility>

#include <wil/common.h>
#include <wil/result_macros.h> // THROW_IF_FAILED: the exception regime's currency

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
namespace wilx
{
//! The callback's return type is the enumeration's control flow: void
//! continues, false stops, S_OK continues (any other HRESULT stops). The
//! value it receives is only ever valid for the callback's duration.
template <typename TCallback, typename TValue>
concept enum_callback =
    std::invocable<TCallback, TValue> &&
    (std::same_as<std::invoke_result_t<TCallback, TValue>, void> ||
        std::same_as<std::invoke_result_t<TCallback, TValue>, bool> ||
        std::same_as<std::invoke_result_t<TCallback, TValue>, HRESULT>);

namespace details
{
    //! WI_NOEXCEPT, like wil's own C callbacks (FallbackTelemetryCallback,
    //! FailfastWithContextCallback): a callback that throws here breaks the
    //! nothrow variant's contract and terminates - the annotation makes that
    //! defined instead of UB at the C boundary.
    template <typename TCallback, typename TValue>
    requires enum_callback<TCallback, TValue>
    BOOL __stdcall EnumApiCallbackNoThrow(TValue value, LPARAM lParam) WI_NOEXCEPT
    {
        auto pCallback = reinterpret_cast<TCallback*>(lParam);
        using result_t = std::invoke_result_t<TCallback, TValue>;
        if constexpr (std::is_void_v<result_t>)
        {
            (*pCallback)(value);
            return TRUE;
        }
        else if constexpr (std::is_same_v<result_t, HRESULT>)
        {
            // Only S_OK continues, and it works for NTSTATUS too: ERROR_SUCCESS is also 0.
            return (S_OK == (*pCallback)(value)) ? TRUE : FALSE;
        }
        else
        {
            return (*pCallback)(value) ? TRUE : FALSE;
        }
    }

    //! TValue must be the trampoline's first parameter *as the API spells it*
    //! (LPWSTR, HWND, ...): the C function-pointer types are exact, and any
    //! cv-qualification difference makes the callback incompatible with them.
    template <typename TValue, typename TEnumApi, typename TCallback>
    void DoEnumApiNoThrow(TEnumApi&& enumApi, TCallback&& callback) noexcept
    {
        enumApi(EnumApiCallbackNoThrow<TCallback, TValue>, reinterpret_cast<LPARAM>(&callback));
    }

#ifdef WIL_ENABLE_EXCEPTIONS
    template <typename TCallback, typename TValue>
    struct EnumApiCallbackData
    {
        std::exception_ptr exception;
        TCallback* pCallback;
    };

    template <typename TCallback, typename TValue>
    requires enum_callback<TCallback, TValue>
    BOOL __stdcall EnumApiCallback(TValue value, LPARAM lParam)
    {
        auto pCallbackData = reinterpret_cast<EnumApiCallbackData<TCallback, TValue>*>(lParam);
        try
        {
            auto pCallback = pCallbackData->pCallback;
            using result_t = std::invoke_result_t<TCallback, TValue>;
            if constexpr (std::is_void_v<result_t>)
            {
                (*pCallback)(value);
                return TRUE;
            }
            else if constexpr (std::is_same_v<result_t, HRESULT>)
            {
                return (S_OK == (*pCallback)(value)) ? TRUE : FALSE;
            }
            else
            {
                return (*pCallback)(value) ? TRUE : FALSE;
            }
        }
        catch (...)
        {
            pCallbackData->exception = std::current_exception();
            return FALSE;
        }
    }

    template <typename TValue, typename TEnumApi, typename TCallback>
    void DoEnumApi(TEnumApi&& enumApi, TCallback&& callback)
    {
        EnumApiCallbackData<TCallback, TValue> callbackData = {nullptr, &callback};
        enumApi(EnumApiCallback<TCallback, TValue>, reinterpret_cast<LPARAM>(&callbackData));
        if (callbackData.exception)
        {
            std::rethrow_exception(callbackData.exception);
        }
    }
#endif // WIL_ENABLE_EXCEPTIONS
} // namespace details

//! PCWSTR passed to the callback is system-owned: valid only for its duration.
template <typename TCallback>
concept desktop_enum_callback = enum_callback<TCallback, PCWSTR>;

template <desktop_enum_callback TCallback>
void for_each_desktop_nothrow(TCallback&& callback) noexcept
{
    details::DoEnumApiNoThrow<LPWSTR>(
        [](DESKTOPENUMPROCW enumproc, LPARAM lParam) noexcept -> BOOL {
            return EnumDesktopsW(GetProcessWindowStation(), enumproc, lParam);
        },
        std::forward<TCallback>(callback));
}

template <desktop_enum_callback TCallback>
void for_each_desktop_nothrow(_In_ HWINSTA windowStation, TCallback&& callback) noexcept
{
    details::DoEnumApiNoThrow<LPWSTR>(
        [windowStation](DESKTOPENUMPROCW enumproc, LPARAM lParam) noexcept -> BOOL {
            return EnumDesktopsW(windowStation, enumproc, lParam);
        },
        std::forward<TCallback>(callback));
}

#ifdef WIL_ENABLE_EXCEPTIONS
template <desktop_enum_callback TCallback>
void for_each_desktop(TCallback&& callback)
{
    details::DoEnumApi<LPWSTR>(
        [](DESKTOPENUMPROCW enumproc, LPARAM lParam) -> BOOL {
            return EnumDesktopsW(GetProcessWindowStation(), enumproc, lParam);
        },
        std::forward<TCallback>(callback));
}

template <desktop_enum_callback TCallback>
void for_each_desktop(_In_ HWINSTA windowStation, TCallback&& callback)
{
    details::DoEnumApi<LPWSTR>(
        [windowStation](DESKTOPENUMPROCW enumproc, LPARAM lParam) -> BOOL {
            return EnumDesktopsW(windowStation, enumproc, lParam);
        },
        std::forward<TCallback>(callback));
}
#endif // WIL_ENABLE_EXCEPTIONS

//! The desktop handle must carry DESKTOP_READOBJECTS; a denied enumeration
//! reports itself as no windows (total fail-soft).
template <typename TCallback>
concept desktop_window_enum_callback = enum_callback<TCallback, HWND>;

template <desktop_window_enum_callback TCallback>
void for_each_desktop_window_nothrow(_In_ HDESK desktop, TCallback&& callback) noexcept
{
    details::DoEnumApiNoThrow<HWND>(
        [desktop](WNDENUMPROC enumproc, LPARAM lParam) noexcept -> BOOL {
            return EnumDesktopWindows(desktop, enumproc, lParam);
        },
        std::forward<TCallback>(callback));
}

#ifdef WIL_ENABLE_EXCEPTIONS
template <desktop_window_enum_callback TCallback>
void for_each_desktop_window(_In_ HDESK desktop, TCallback&& callback)
{
    details::DoEnumApi<HWND>(
        [desktop](WNDENUMPROC enumproc, LPARAM lParam) -> BOOL {
            return EnumDesktopWindows(desktop, enumproc, lParam);
        },
        std::forward<TCallback>(callback));
}
#endif // WIL_ENABLE_EXCEPTIONS

//! PCWSTR passed to the callback is system-owned: valid only for its duration.
template <typename TCallback>
concept window_station_enum_callback = enum_callback<TCallback, PCWSTR>;

//! Enumerates the window stations of the calling session that the caller may
//! see; a denied enumeration reports itself as no names (total fail-soft).
template <window_station_enum_callback TCallback>
void for_each_window_station_nothrow(TCallback&& callback) noexcept
{
    details::DoEnumApiNoThrow<LPWSTR>(EnumWindowStationsW, std::forward<TCallback>(callback));
}

#ifdef WIL_ENABLE_EXCEPTIONS
template <window_station_enum_callback TCallback>
void for_each_window_station(TCallback&& callback)
{
    details::DoEnumApi<LPWSTR>(EnumWindowStationsW, std::forward<TCallback>(callback));
}
#endif // WIL_ENABLE_EXCEPTIONS

//! Error-code core behind the name queries: callers that must classify a
//! failure (not just survive it) need the error, which the total fail-soft
//! TryGet* regime deliberately discards. Failure is returned as wil's
//! error-code currency, an HRESULT, and it is always a Win32 one
//! (HRESULT_FROM_WIN32), so HRESULT_CODE recovers what GetLastError would have
//! said - 0 is replaced by ERROR_INVALID_HANDLE because these user-object APIs
//! fail without touching last error for null handles.
//! Accepts HDESK and HWINSTA alike.
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
#endif // WIL_ENABLE_EXCEPTIONS

//! Session of the calling process, fixed at creation: no API moves a running
//! process to another one, so callers may read it once. Deliberately no
//! fail-soft sibling: 0 is a real session (services), so unlike an empty name
//! there is no result left that could mean "failed".
inline HRESULT GetCurrentSessionIdNoThrow(_Out_ DWORD& session) WI_NOEXCEPT
{
    session = 0;

    SetLastError(0);
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session))
    {
        const DWORD lastError = GetLastError();
        // The API documents ERROR_INVALID_PARAMETER; the stand-in only matters
        // if it ever failed without setting last error at all.
        return HRESULT_FROM_WIN32(0 == lastError ? ERROR_INVALID_PARAMETER : lastError);
    }
    return S_OK;
}
} // namespace wilx
#endif // WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#endif // __WILX_DESKTOP_INCLUDED
