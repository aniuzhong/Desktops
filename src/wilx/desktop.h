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
//! Enumeration over the window station / desktop subsystem: the session's
//! window stations, their desktops, and the windows on one desktop - the three
//! user-object families Win32 exposes through Enum* APIs. All three share the
//! trampoline in enum_callbacks.h, the way wil's windowing.h shares one for
//! its window enumerators.
#ifndef __WILX_DESKTOP_INCLUDED
#define __WILX_DESKTOP_INCLUDED

#include <WinUser.h>   // EnumDesktopsW, EnumDesktopWindows, GetProcessWindowStation
#include <minwindef.h> // HDESK, HWINSTA, LPARAM

#include <wil/common.h>

#include "enum_callbacks.h"

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
namespace wilx
{
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
} // namespace wilx
#endif // WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#endif // __WILX_DESKTOP_INCLUDED
