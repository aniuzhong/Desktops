//*********************************************************
//
//    wilx - WIL-style extensions for Virtual Desktop.
//    Header-only, shaped after wil: machinery earns a
//    header (the trampolines below), single-pattern helpers
//    live in the win32_helpers.h drawer.
//    API contracts: wilx/README.md. Comments here only
//    explain choices the code cannot show.
//
//*********************************************************
#ifndef __WILX_DESKTOP_WINDOWS_INCLUDED
#define __WILX_DESKTOP_WINDOWS_INCLUDED

#include <windows.h>
#include <concepts>
#include <exception>
#include <type_traits>
#include <utility>

#include <wil/common.h>

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
namespace wilx
{
template <typename TCallback>
concept desktop_window_enum_callback =
    std::invocable<TCallback, HWND> &&
    (std::same_as<std::invoke_result_t<TCallback, HWND>, void> ||
        std::same_as<std::invoke_result_t<TCallback, HWND>, bool> ||
        std::same_as<std::invoke_result_t<TCallback, HWND>, HRESULT>);

namespace details
{
    template <desktop_window_enum_callback TCallback>
    BOOL __stdcall EnumDesktopWindowsCallbackNoThrow(HWND hwnd, LPARAM lParam)
    {
        auto pCallback = reinterpret_cast<TCallback*>(lParam);
        using result_t = decltype((*pCallback)(hwnd));
        if constexpr (std::is_void_v<result_t>)
        {
            (*pCallback)(hwnd);
            return TRUE;
        }
        else if constexpr (std::is_same_v<result_t, HRESULT>)
        {
            return (S_OK == (*pCallback)(hwnd)) ? TRUE : FALSE;
        }
        else
        {
            return (*pCallback)(hwnd) ? TRUE : FALSE;
        }
    }

    template <typename TEnumApi, desktop_window_enum_callback TCallback>
    void DoEnumDesktopWindowsNoThrow(TEnumApi&& enumApi, TCallback&& callback) noexcept
    {
        enumApi(EnumDesktopWindowsCallbackNoThrow<TCallback>, reinterpret_cast<LPARAM>(&callback));
    }

#ifdef WIL_ENABLE_EXCEPTIONS
    template <desktop_window_enum_callback TCallback>
    struct EnumDesktopWindowsCallbackData
    {
        std::exception_ptr exception;
        TCallback* pCallback;
    };

    template <desktop_window_enum_callback TCallback>
    BOOL __stdcall EnumDesktopWindowsCallback(HWND hwnd, LPARAM lParam)
    {
        auto pCallbackData = reinterpret_cast<EnumDesktopWindowsCallbackData<TCallback>*>(lParam);
        try
        {
            auto pCallback = pCallbackData->pCallback;
            using result_t = decltype((*pCallback)(hwnd));
            if constexpr (std::is_void_v<result_t>)
            {
                (*pCallback)(hwnd);
                return TRUE;
            }
            else if constexpr (std::is_same_v<result_t, HRESULT>)
            {
                return (S_OK == (*pCallback)(hwnd)) ? TRUE : FALSE;
            }
            else
            {
                return (*pCallback)(hwnd) ? TRUE : FALSE;
            }
        }
        catch (...)
        {
            pCallbackData->exception = std::current_exception();
            return FALSE;
        }
    }

    template <typename TEnumApi, desktop_window_enum_callback TCallback>
    void DoEnumDesktopWindows(TEnumApi&& enumApi, TCallback&& callback)
    {
        EnumDesktopWindowsCallbackData<TCallback> callbackData = {nullptr, &callback};
        enumApi(EnumDesktopWindowsCallback<TCallback>, reinterpret_cast<LPARAM>(&callbackData));
        if (callbackData.exception)
        {
            std::rethrow_exception(callbackData.exception);
        }
    }
#endif // WIL_ENABLE_EXCEPTIONS
} // namespace details

//! The desktop handle must carry DESKTOP_READOBJECTS; a denied enumeration
//! reports itself as no windows (total fail-soft).
template <desktop_window_enum_callback TCallback>
void for_each_desktop_window_nothrow(_In_ HDESK desktop, TCallback&& callback) noexcept
{
    auto boundEnumDesktopWindows = [desktop](WNDENUMPROC enumproc, LPARAM lParam) noexcept -> BOOL {
        return EnumDesktopWindows(desktop, enumproc, lParam);
    };
    details::DoEnumDesktopWindowsNoThrow(boundEnumDesktopWindows, std::forward<TCallback>(callback));
}

#ifdef WIL_ENABLE_EXCEPTIONS
template <desktop_window_enum_callback TCallback>
void for_each_desktop_window(_In_ HDESK desktop, TCallback&& callback)
{
    auto boundEnumDesktopWindows = [desktop](WNDENUMPROC enumproc, LPARAM lParam) -> BOOL {
        return EnumDesktopWindows(desktop, enumproc, lParam);
    };
    details::DoEnumDesktopWindows(boundEnumDesktopWindows, std::forward<TCallback>(callback));
}
#endif // WIL_ENABLE_EXCEPTIONS
} // namespace wilx
#endif // WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#endif // __WILX_DESKTOP_WINDOWS_INCLUDED
