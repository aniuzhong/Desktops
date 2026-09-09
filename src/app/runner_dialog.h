#pragma once

#include <windows.h>

#include <string>
#include <thread>

#include "runner.h"

namespace desktops
{
    // A Run dialog living on one desktop, and the thread that hosts it.
    //
    // It acquires and releases its own desktop attachment: SetThreadDesktop can
    // only be called by the thread that will host the window, and moving input
    // back to Default is the inverse of that same attachment — so the pair stays
    // together here rather than in Manager.
    //
    // One instance per desktop. The destructor joins the thread, so an instance
    // must outlive it: erase one only after it has reported kFinished.
    class RunnerDialog
    {
    public:
        RunnerDialog(std::wstring desktop, HWND panel, HINSTANCE module);

        ~RunnerDialog();

        RunnerDialog(const RunnerDialog&) = delete;
        RunnerDialog& operator=(const RunnerDialog&) = delete;

        // Manager -> dialog: bring the Run dialog to the front.
        void Activate();

        // Manager -> dialog: ask the thread to exit. Never blocks — the object is
        // destroyed later, once the thread reports kFinished.
        void RequestClose();

    private:
        // Launch failures belong on the desktop this dialog lives on: the user is
        // standing there when they typed the command.
        struct DialogSink : Runner::Sink
        {
            void OnLaunchFailed(const std::wstring& desktop, const std::wstring& detail) override;
        };

        void ThreadMain();

        static INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);

        void GoHome(HWND dialog);

        std::wstring desktop_;
        HWND panel_;
        HINSTANCE module_;
        DialogSink sink_;
        Runner runner_;
        std::thread thread_;
        DWORD thread_id_ = 0;
    };
}
