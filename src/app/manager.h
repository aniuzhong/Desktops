#pragma once

#include <windows.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "runner.h"
#include "runner_dialog.h"

namespace desktops
{
    // The Default-desktop panel. Owns the desktop orchestration (create, switch
    // into, discard), one Runner, and the registry of RunnerDialogs.
    //
    // A second launch signals the go-home event instead of starting a second
    // Manager; Pump() waits on that event alongside the message queue.
    class Manager : private Runner::Sink
    {
    public:
        // Creates the panel and pumps until it closes. `goHome` is the auto-reset
        // event a second instance signals to recall the user from wherever they are.
        static int Run(HINSTANCE instance, HANDLE goHome);

    private:
        Manager(HINSTANCE instance, HANDLE goHome);

        Manager(const Manager&) = delete;
        Manager& operator=(const Manager&) = delete;

        // Runner::Sink. The panel renders launch failures in its own business flow
        // (see OnNew), so this only records them — unlike the RunnerDialog sink,
        // which pops a box on the desktop it lives on.
        void OnLaunchFailed(const std::wstring& desktop, const std::wstring& detail) override;

        int Pump();

        static INT_PTR CALLBACK PanelProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);
        static INT_PTR CALLBACK NewDesktopProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);

        // Returns false for a command it does not handle.
        bool OnCommand(WPARAM wParam);

        void OnRunnerEvents();
        void OnClose(HWND dialog);
        void OnDestroy();

        void OnNew();
        void SwitchTo(const std::wstring& name);
        void OnGoHome();

        // Brings the dialog up on the named desktop and moves input there. Returns
        // true when the input desktop has verifiably arrived. The panel thread never
        // exits during any of this, so it can always take the user back home — that
        // is the undo agent.
        bool AttachAndSwitch(const std::wstring& name);

        bool PromptDesktopName(const std::wstring& suggested, std::wstring& chosen);

        void RecallPanel();
        void RefreshList();
        std::wstring SelectedDesktop() const;

        bool RunnerPresent(const std::wstring& desktop) const;
        void SpawnRunner(const std::wstring& desktop);

        HINSTANCE instance_ = nullptr;
        HANDLE go_home_ = nullptr;
        HWND panel_ = nullptr;
        std::wstring pending_name_;
        Runner runner_;

        // One RunnerDialog per desktop, keyed by name. Erased only on kFinished —
        // an entry outlives its thread, never the reverse.
        std::map<std::wstring, std::unique_ptr<RunnerDialog>> dialogs_;
    };
}
