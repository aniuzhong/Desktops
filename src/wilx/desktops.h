//*********************************************************
//
//    wilx - WIL-style extensions for Virtual Desktop.
//    Header-only, shaped after wil: machinery earns a
//    header (the trampolines below), single-pattern
//    helpers live in the win32_helpers.h drawer.
//    API contracts: wilx/README.md. Comments here only
//    explain choices the code cannot show.
//
//*********************************************************
#ifndef __WILX_DESKTOPS_INCLUDED
#define __WILX_DESKTOPS_INCLUDED

#include <windows.h>
#include <concepts>
#include <exception>
#include <type_traits>
#include <utility>

#include <wil/common.h>

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
namespace wilx
{
//! PCWSTR passed to the callback is system-owned: valid only for its duration.
template <typename TCallback>
concept desktop_enum_callback =
    std::invocable<TCallback, PCWSTR> &&
    (std::same_as<std::invoke_result_t<TCallback, PCWSTR>, void> ||
        std::same_as<std::invoke_result_t<TCallback, PCWSTR>, bool> ||
        std::same_as<std::invoke_result_t<TCallback, PCWSTR>, HRESULT>);

namespace details
{
    template <desktop_enum_callback TCallback>
    BOOL __stdcall EnumDesktopsCallbackNoThrow(LPWSTR lpszDesktopName, LPARAM lParam)
    {
        auto pCallback = reinterpret_cast<TCallback*>(lParam);
        using result_t = decltype((*pCallback)(static_cast<PCWSTR>(lpszDesktopName)));
        if constexpr (std::is_void_v<result_t>)
        {
            (*pCallback)(lpszDesktopName);
            return TRUE;
        }
        else if constexpr (std::is_same_v<result_t, HRESULT>)
        {
            return (S_OK == (*pCallback)(lpszDesktopName)) ? TRUE : FALSE;
        }
        else
        {
            return (*pCallback)(lpszDesktopName) ? TRUE : FALSE;
        }
    }

    template <typename TEnumApi, desktop_enum_callback TCallback>
    void DoEnumDesktopsNoThrow(TEnumApi&& enumApi, TCallback&& callback) noexcept
    {
        enumApi(EnumDesktopsCallbackNoThrow<TCallback>, reinterpret_cast<LPARAM>(&callback));
    }

#ifdef WIL_ENABLE_EXCEPTIONS
    template <desktop_enum_callback TCallback>
    struct EnumDesktopsCallbackData
    {
        std::exception_ptr exception;
        TCallback* pCallback;
    };

    template <desktop_enum_callback TCallback>
    BOOL __stdcall EnumDesktopsCallback(LPWSTR lpszDesktopName, LPARAM lParam)
    {
        auto pCallbackData = reinterpret_cast<EnumDesktopsCallbackData<TCallback>*>(lParam);
        try
        {
            auto pCallback = pCallbackData->pCallback;
            using result_t = decltype((*pCallback)(static_cast<PCWSTR>(lpszDesktopName)));
            if constexpr (std::is_void_v<result_t>)
            {
                (*pCallback)(lpszDesktopName);
                return TRUE;
            }
            else if constexpr (std::is_same_v<result_t, HRESULT>)
            {
                return (S_OK == (*pCallback)(lpszDesktopName)) ? TRUE : FALSE;
            }
            else
            {
                return (*pCallback)(lpszDesktopName) ? TRUE : FALSE;
            }
        }
        catch (...)
        {
            pCallbackData->exception = std::current_exception();
            return FALSE;
        }
    }

    template <typename TEnumApi, desktop_enum_callback TCallback>
    void DoEnumDesktops(TEnumApi&& enumApi, TCallback&& callback)
    {
        EnumDesktopsCallbackData<TCallback> callbackData = {nullptr, &callback};
        enumApi(EnumDesktopsCallback<TCallback>, reinterpret_cast<LPARAM>(&callbackData));
        if (callbackData.exception)
        {
            std::rethrow_exception(callbackData.exception);
        }
    }
#endif // WIL_ENABLE_EXCEPTIONS
} // namespace details

template <desktop_enum_callback TCallback>
void for_each_desktop_nothrow(TCallback&& callback) noexcept
{
    details::DoEnumDesktopsNoThrow(
        [](DESKTOPENUMPROCW enumproc, LPARAM lParam) noexcept -> BOOL {
            return EnumDesktopsW(GetProcessWindowStation(), enumproc, lParam);
        },
        std::forward<TCallback>(callback));
}

template <desktop_enum_callback TCallback>
void for_each_desktop_nothrow(_In_ HWINSTA hWindowStation, TCallback&& callback) noexcept
{
    auto boundEnumDesktops = [hWindowStation](DESKTOPENUMPROCW enumproc, LPARAM lParam) noexcept -> BOOL {
        return EnumDesktopsW(hWindowStation, enumproc, lParam);
    };
    details::DoEnumDesktopsNoThrow(boundEnumDesktops, std::forward<TCallback>(callback));
}

#ifdef WIL_ENABLE_EXCEPTIONS
template <desktop_enum_callback TCallback>
void for_each_desktop(TCallback&& callback)
{
    details::DoEnumDesktops(
        [](DESKTOPENUMPROCW enumproc, LPARAM lParam) -> BOOL {
            return EnumDesktopsW(GetProcessWindowStation(), enumproc, lParam);
        },
        std::forward<TCallback>(callback));
}

template <desktop_enum_callback TCallback>
void for_each_desktop(_In_ HWINSTA hWindowStation, TCallback&& callback)
{
    auto boundEnumDesktops = [hWindowStation](DESKTOPENUMPROCW enumproc, LPARAM lParam) -> BOOL {
        return EnumDesktopsW(hWindowStation, enumproc, lParam);
    };
    details::DoEnumDesktops(boundEnumDesktops, std::forward<TCallback>(callback));
}
#endif // WIL_ENABLE_EXCEPTIONS
} // namespace wilx
#endif // WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#endif // __WILX_DESKTOPS_INCLUDED
