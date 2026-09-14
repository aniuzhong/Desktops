//*********************************************************
//
//    Copyright (c) the Desktops authors.
//    Licensed under the MIT License.
//
//    wilx - WIL-style extensions for Virtual Desktop.
//    The shared machinery behind wilx's *Enum\** enumerators.
//    Internal: this header is not part of the wilx surface -
//    include the header that owns the enumerator instead.
//    API contracts: wilx/README.md. Comments here only
//    explain choices the code cannot show.
//
//*********************************************************
//! @file
//! One parameterised C trampoline serving every Win32 Enum* enumerator: they
//! all smuggle the C++ callback through a single LPARAM and differ only in
//! the callback's first parameter, so the machinery is written once - the
//! same economy wil's windowing.h buys for its three window enumerators.
#ifndef __WILX_ENUM_CALLBACKS_INCLUDED
#define __WILX_ENUM_CALLBACKS_INCLUDED

#include <minwindef.h> // BOOL, LPARAM
#include <wtypes.h>    // HRESULT, S_OK

#include <concepts>
#include <exception>
#include <type_traits>
#include <utility>

#include <wil/common.h>

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
} // namespace wilx
#endif // __WILX_ENUM_CALLBACKS_INCLUDED
