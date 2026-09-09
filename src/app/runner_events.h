#pragma once

#include <windows.h>

#include <deque>
#include <string>

namespace desktops
{
    // RunnerDialog -> Manager. The dialogs run on their own threads and never
    // touch Manager state directly: they append to a queue and post a payload-free
    // wake-up to the panel window.
    struct RunnerEvent
    {
        enum class Kind
        {
            kHomeReturned,   // the dialog moved input back to Default
            kSpawnFailed,    // the dialog could not attach to its desktop
            kFinished,       // the dialog thread is exiting
        };

        Kind kind;
        std::wstring desktop;
    };

    // Posted to the panel window with no payload: drain with TakeRunnerEvents().
    inline constexpr UINT kWmAppEventsPending = WM_APP + 1;

    // Posted (PostThreadMessage) to a dialog thread: bring the Run dialog to front.
    inline constexpr UINT kWmAppActivate = WM_APP + 2;

    // Appends an event and wakes the panel. Safe to call from any thread.
    void PostRunnerEvent(HWND panel, RunnerEvent event);

    // Drains the queue. Call only from the panel thread.
    std::deque<RunnerEvent> TakeRunnerEvents();
}
