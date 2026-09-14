//*********************************************************
//
//    Copyright (c) the Desktops authors.
//    Licensed under the MIT License.
//
//    wilx - WIL-style extensions for Virtual Desktop.
//    Header-only, shaped after wil: machinery earns a
//    header (the Toolhelp snapshot iteration below),
//    single-pattern helpers live in the win32_helpers.h
//    drawer. API contracts: wilx/README.md. Comments here
//    only explain choices the code cannot show.
//
//*********************************************************
//! @file
//! System process/thread iteration over Toolhelp snapshots.
//! Deliberate deviation from the user32 enumerators (desktop.h and kin):
//! the callback runs inline on the C++ frame, so there is no exception
//! barrier to maintain and therefore no separate *_nothrow regime — a
//! throwing callback simply propagates. Total fail-soft as everywhere:
//! a failed snapshot enumerates nothing (see wilx/README.md).
#ifndef __WILX_TOOLHELP_INCLUDED
#define __WILX_TOOLHELP_INCLUDED

#include <minwindef.h>  // DWORD, LPARAM (SAL)
#include <tlhelp32.h>   // CreateToolhelp32Snapshot, Process32FirstW, Thread32First

#include <concepts>
#include <type_traits>
#include <utility>

#include <wil/resource.h>

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
namespace wilx
{
//! The entry passed to the callback is reused across iterations: valid only
//! for the callback's duration (copy anything that must outlive it).
template <typename TCallback>
concept process_enum_callback =
    std::invocable<TCallback, const PROCESSENTRY32W&> &&
    (std::same_as<std::invoke_result_t<TCallback, const PROCESSENTRY32W&>, void> ||
        std::same_as<std::invoke_result_t<TCallback, const PROCESSENTRY32W&>, bool> ||
        std::same_as<std::invoke_result_t<TCallback, const PROCESSENTRY32W&>, HRESULT>);

template <typename TCallback>
concept thread_enum_callback =
    std::invocable<TCallback, const THREADENTRY32&> &&
    (std::same_as<std::invoke_result_t<TCallback, const THREADENTRY32&>, void> ||
        std::same_as<std::invoke_result_t<TCallback, const THREADENTRY32&>, bool> ||
        std::same_as<std::invoke_result_t<TCallback, const THREADENTRY32&>, HRESULT>);

template <process_enum_callback TCallback>
void for_each_process(TCallback&& callback)
{
    wil::unique_handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot)
    {
        return;
    }

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.get(), &entry))
    {
        return;
    }

    do
    {
        using result_t = decltype(callback(static_cast<const PROCESSENTRY32W&>(entry)));
        if constexpr (std::is_void_v<result_t>)
        {
            callback(entry);
        }
        else if constexpr (std::is_same_v<result_t, HRESULT>)
        {
            if (S_OK != callback(entry))
            {
                return;
            }
        }
        else
        {
            if (!callback(entry))
            {
                return;
            }
        }
    } while (Process32NextW(snapshot.get(), &entry));
}

template <thread_enum_callback TCallback>
void for_each_thread(TCallback&& callback)
{
    wil::unique_handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
    if (!snapshot)
    {
        return;
    }

    THREADENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    if (!Thread32First(snapshot.get(), &entry))
    {
        return;
    }

    do
    {
        using result_t = decltype(callback(static_cast<const THREADENTRY32&>(entry)));
        if constexpr (std::is_void_v<result_t>)
        {
            callback(entry);
        }
        else if constexpr (std::is_same_v<result_t, HRESULT>)
        {
            if (S_OK != callback(entry))
            {
                return;
            }
        }
        else
        {
            if (!callback(entry))
            {
                return;
            }
        }
    } while (Thread32Next(snapshot.get(), &entry));
}
} // namespace wilx
#endif // WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#endif // __WILX_TOOLHELP_INCLUDED
