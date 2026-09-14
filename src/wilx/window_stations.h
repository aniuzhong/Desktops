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
#ifndef __WILX_WINDOW_STATIONS_INCLUDED
#define __WILX_WINDOW_STATIONS_INCLUDED

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
concept window_station_enum_callback =
    std::invocable<TCallback, PCWSTR> &&
    (std::same_as<std::invoke_result_t<TCallback, PCWSTR>, void> ||
        std::same_as<std::invoke_result_t<TCallback, PCWSTR>, bool> ||
        std::same_as<std::invoke_result_t<TCallback, PCWSTR>, HRESULT>);

namespace details
{
    template <window_station_enum_callback TCallback>
    BOOL __stdcall EnumWindowStationsCallbackNoThrow(LPWSTR lpszWindowStationName, LPARAM lParam)
    {
        auto pCallback = reinterpret_cast<TCallback*>(lParam);
        using result_t = decltype((*pCallback)(static_cast<PCWSTR>(lpszWindowStationName)));
        if constexpr (std::is_void_v<result_t>)
        {
            (*pCallback)(lpszWindowStationName);
            return TRUE;
        }
        else if constexpr (std::is_same_v<result_t, HRESULT>)
        {
            return (S_OK == (*pCallback)(lpszWindowStationName)) ? TRUE : FALSE;
        }
        else
        {
            return (*pCallback)(lpszWindowStationName) ? TRUE : FALSE;
        }
    }

    template <typename TEnumApi, window_station_enum_callback TCallback>
    void DoEnumWindowStationsNoThrow(TEnumApi&& enumApi, TCallback&& callback) noexcept
    {
        enumApi(EnumWindowStationsCallbackNoThrow<TCallback>, reinterpret_cast<LPARAM>(&callback));
    }

#ifdef WIL_ENABLE_EXCEPTIONS
    template <window_station_enum_callback TCallback>
    struct EnumWindowStationsCallbackData
    {
        std::exception_ptr exception;
        TCallback* pCallback;
    };

    template <window_station_enum_callback TCallback>
    BOOL __stdcall EnumWindowStationsCallback(LPWSTR lpszWindowStationName, LPARAM lParam)
    {
        auto pCallbackData = reinterpret_cast<EnumWindowStationsCallbackData<TCallback>*>(lParam);
        try
        {
            auto pCallback = pCallbackData->pCallback;
            using result_t = decltype((*pCallback)(static_cast<PCWSTR>(lpszWindowStationName)));
            if constexpr (std::is_void_v<result_t>)
            {
                (*pCallback)(lpszWindowStationName);
                return TRUE;
            }
            else if constexpr (std::is_same_v<result_t, HRESULT>)
            {
                return (S_OK == (*pCallback)(lpszWindowStationName)) ? TRUE : FALSE;
            }
            else
            {
                return (*pCallback)(lpszWindowStationName) ? TRUE : FALSE;
            }
        }
        catch (...)
        {
            pCallbackData->exception = std::current_exception();
            return FALSE;
        }
    }

    template <typename TEnumApi, window_station_enum_callback TCallback>
    void DoEnumWindowStations(TEnumApi&& enumApi, TCallback&& callback)
    {
        EnumWindowStationsCallbackData<TCallback> callbackData = {nullptr, &callback};
        enumApi(EnumWindowStationsCallback<TCallback>, reinterpret_cast<LPARAM>(&callbackData));
        if (callbackData.exception)
        {
            std::rethrow_exception(callbackData.exception);
        }
    }
#endif // WIL_ENABLE_EXCEPTIONS
} // namespace details

//! Enumerates the window stations of the calling session that the caller may
//! see; a denied enumeration reports itself as no names (total fail-soft).
template <window_station_enum_callback TCallback>
void for_each_window_station_nothrow(TCallback&& callback) noexcept
{
    details::DoEnumWindowStationsNoThrow(EnumWindowStationsW, std::forward<TCallback>(callback));
}

#ifdef WIL_ENABLE_EXCEPTIONS
template <window_station_enum_callback TCallback>
void for_each_window_station(TCallback&& callback)
{
    details::DoEnumWindowStations(EnumWindowStationsW, std::forward<TCallback>(callback));
}
#endif // WIL_ENABLE_EXCEPTIONS
} // namespace wilx
#endif // WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#endif // __WILX_WINDOW_STATIONS_INCLUDED
