#include "panel.h"

#include <algorithm>
#include <format>
#include <map>
#include <memory>
#include <vector>

#include <wil/resource.h>

#include "common.h"
#include "logging.h"
#include "resource.h"
#include "runner.h"
#include "runner_dialog.h"
#include "runner_events.h"
#include "wilx/win32_helpers.h"

namespace desktops::panel
{
    namespace
    {
        constexpr DWORD kProbeBudgetMs = 5000;       // dialog readiness budget
        constexpr DWORD kSwitchConfirmMs = 3000;     // undo budget: input must actually arrive

        HWND g_panel = nullptr;
        HINSTANCE g_instance = nullptr;

        // What a new desktop is seeded with. This names *what* to run; keeping the
        // console alive (-NoExit) is Runner knowledge, not the caller's.
        constexpr wchar_t kAnchorInput[] = L"powershell.exe";

        // The panel reports launch failures in its own business flow (see on_new),
        // so this sink only records them — unlike the RunnerDialog sink, which pops
        // a box on the desktop it lives on.
        struct PanelSink : Runner::Sink
        {
            void OnLaunchFailed(const std::wstring& desktop, const std::wstring& detail) override
            {
                log::Warn(std::format(L"[launch] '{}' on '{}'", detail, desktop));
            }
        };

        // One Runner per caller, never shared with a RunnerDialog. These two become
        // Manager members in step 4.
        PanelSink g_sink;
        Runner g_runner(g_sink);

        // The dialog registry: one RunnerDialog per desktop, keyed by name. Erased
        // only on kFinished — an entry outlives its thread, never the reverse.
        // Becomes Manager members in step 4.
        std::map<std::wstring, std::unique_ptr<RunnerDialog>> g_dialogs;

        std::wstring trim(const std::wstring& value)
        {
            const auto begin = value.find_first_not_of(L" \t");
            if (begin == std::wstring::npos)
                return {};
            const auto end = value.find_last_not_of(L" \t");
            return value.substr(begin, end - begin + 1);
        }

        void refresh_list()
        {
            HWND list = GetDlgItem(g_panel, IDC_DESKTOP_LIST);
            SendMessageW(list, LB_RESETCONTENT, 0, 0);
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kDefaultDesktop));
            for (const std::wstring& name : ListExtraDesktops())
                SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        }

        std::wstring selected_desktop()
        {
            HWND list = GetDlgItem(g_panel, IDC_DESKTOP_LIST);
            const int index = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
            if (index == LB_ERR)
                return {};
            const int length = static_cast<int>(SendMessageW(list, LB_GETTEXTLEN, index, 0));
            std::wstring name(static_cast<size_t>(length), L'\0');
            SendMessageW(list, LB_GETTEXT, index, reinterpret_cast<LPARAM>(name.data()));
            name.resize(static_cast<size_t>(length));
            return name;
        }

        void recall_panel()
        {
            ShowWindow(g_panel, SW_SHOW);
            SetForegroundWindow(g_panel);
        }

        void on_go_home()
        {
            const std::wstring input = wilx::TryGetInputDesktopName();
            if (_wcsicmp(input.c_str(), kDefaultDesktop) != 0)
                SwitchInputTo(kDefaultDesktop);
            refresh_list();
            recall_panel();
            log::Info(L"[home] panel is home");
        }

        // True when a window of this process already exists on that desktop — the
        // ground truth for "is there a dialog there", even one we did not start.
        bool runner_present(const std::wstring& desktop)
        {
            return ProbeProcessWindow(desktop, GetCurrentProcessId(), 0);
        }

        void spawn_runner(const std::wstring& desktop)
        {
            if (g_dialogs.contains(desktop))
                return;
            g_dialogs.emplace(desktop, std::make_unique<RunnerDialog>(desktop, g_panel, g_instance));
        }

        void drain_runner_events()
        {
            for (const RunnerEvent& event : TakeRunnerEvents())
            {
                switch (event.kind)
                {
                case RunnerEvent::Kind::kHomeReturned:
                    on_go_home();
                    break;
                case RunnerEvent::Kind::kSpawnFailed:
                    MessageBoxW(g_panel, std::format(L"Could not attach to desktop '{}'.", event.desktop).c_str(),
                        L"Virtual Desktop", MB_ICONERROR | MB_TASKMODAL);
                    refresh_list();
                    break;
                case RunnerEvent::Kind::kFinished:
                    // Destroys the RunnerDialog, whose destructor joins the thread —
                    // safe here because the thread has already reported it is done.
                    g_dialogs.erase(event.desktop);
                    break;
                }
            }
        }

        // Brings the dialog up on the named desktop and moves input there.
        // Returns true when the input desktop has verifiably arrived. The panel
        // thread never exits during any of this, so it can always take the user
        // back home — that is the undo agent.
        bool attach_and_switch(const std::wstring& name)
        {
            if (!runner_present(name))
                spawn_runner(name);
            if (!ProbeProcessWindow(name, GetCurrentProcessId(), kProbeBudgetMs))
            {
                if (auto it = g_dialogs.find(name); it != g_dialogs.end())
                    it->second->RequestClose();
                log::Err(std::format(L"[switch] dialog on '{}' never appeared", name), 0);
                MessageBoxW(g_panel, std::format(L"Could not attach to desktop '{}'.", name).c_str(),
                    L"Virtual Desktop", MB_ICONERROR | MB_TASKMODAL);
                return false;
            }
            if (!SwitchInputTo(name))
                return false;

            const std::uint64_t deadline = GetTickCount64() + kSwitchConfirmMs;
            while (GetTickCount64() < deadline)
            {
                const std::wstring input = wilx::TryGetInputDesktopName();
                if (_wcsicmp(input.c_str(), name.c_str()) == 0)
                {
                    log::Info(std::format(L"[switch] input desktop is now '{}'", name));
                    if (auto it = g_dialogs.find(name); it != g_dialogs.end())
                        it->second->Activate();
                    return true;
                }
                Sleep(100);
            }
            log::Err(std::format(L"[switch] '{}' never became the input desktop", name), 0);
            SwitchInputTo(kDefaultDesktop);
            MessageBoxW(g_panel, std::format(L"Switching to '{}' did not take effect.", name).c_str(),
                L"Virtual Desktop", MB_ICONWARNING | MB_TASKMODAL);
            return false;
        }

        void switch_to(const std::wstring& name)
        {
            if (name.empty() || _wcsicmp(name.c_str(), kDefaultDesktop) == 0)
                return;
            log::Info(std::format(L"[switch] to '{}'", name));
            attach_and_switch(name);
        }

        std::wstring next_free_name(const std::vector<std::wstring>& taken)
        {
            for (int index = 1;; ++index)
            {
                const std::wstring candidate = std::format(L"Desktop{}", index);
                const bool used = std::any_of(taken.begin(), taken.end(), [&](const std::wstring& name) {
                    return _wcsicmp(name.c_str(), candidate.c_str()) == 0;
                });
                if (!used)
                    return candidate;
            }
        }

        // The [New] name prompt: prefilled with the next free DesktopN, fully editable.
        std::wstring g_pendingName;

        INT_PTR CALLBACK new_desktop_proc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
        {
            switch (message)
            {
            case WM_INITDIALOG:
                g_pendingName = *reinterpret_cast<std::wstring*>(lParam);
                SetDlgItemTextW(dialog, IDC_NEW_NAME, g_pendingName.c_str());
                SendDlgItemMessageW(dialog, IDC_NEW_NAME, EM_SETSEL, 0, -1);
                SetFocus(GetDlgItem(dialog, IDC_NEW_NAME));
                return FALSE;
            case WM_COMMAND:
                switch (LOWORD(wParam))
                {
                case IDOK:
                {
                    wchar_t buffer[128] = {};
                    GetDlgItemTextW(dialog, IDC_NEW_NAME, buffer, 128);
                    g_pendingName = trim(buffer);
                    if (g_pendingName.empty())
                    {
                        MessageBoxW(dialog, L"Please enter a desktop name.", L"New Desktop",
                            MB_ICONEXCLAMATION | MB_TASKMODAL);
                        return TRUE;
                    }
                    EndDialog(dialog, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(dialog, IDCANCEL);
                    return TRUE;
                }
                break;
            }
            return FALSE;
        }

        bool prompt_desktop_name(const std::wstring& suggested, std::wstring& chosen)
        {
            g_pendingName = suggested;
            const INT_PTR result = DialogBoxParamW(g_instance, MAKEINTRESOURCEW(IDD_NEW_DESKTOP),
                g_panel, new_desktop_proc, reinterpret_cast<LPARAM>(&g_pendingName));
            chosen = g_pendingName;
            return result == IDOK && !chosen.empty();
        }

        void on_new()
        {
            // [New] creates and seeds, deliberately without switching: the seed
            // console is what pins the desktop, and the user enters it later via
            // Switch To. The created desktop dies if the seed cannot land — an
            // unentered desktop is not kept, by design.
            const std::vector<std::wstring> taken = ListExtraDesktops();
            std::wstring name;
            if (!prompt_desktop_name(next_free_name(taken), name))
                return;

            const bool duplicates = _wcsicmp(name.c_str(), kDefaultDesktop) == 0 ||
                std::any_of(taken.begin(), taken.end(), [&](const std::wstring& existing) {
                    return _wcsicmp(existing.c_str(), name.c_str()) == 0;
                });
            if (duplicates)
            {
                MessageBoxW(g_panel, std::format(L"A desktop named '{}' already exists.", name).c_str(),
                    L"New Desktop", MB_ICONEXCLAMATION | MB_TASKMODAL);
                return;
            }

            log::Info(std::format(L"[new] creating '{}'", name));
            // Held open until the seed proves it landed: a desktop with no window
            // on it dies with its last handle.
            wil::unique_hdesk created(CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr));
            if (!created)
            {
                log::Err(std::format(L"[new] CreateDesktopW('{}') failed", name), GetLastError());
                MessageBoxW(g_panel, std::format(L"Could not create desktop '{}' (error {}).", name, GetLastError()).c_str(),
                    L"Virtual Desktop", MB_ICONERROR | MB_TASKMODAL);
                return;
            }

            const Runner::LaunchResult anchor = g_runner.Launch(name, kAnchorInput);
            const bool seeded = anchor.arrived;
            if (seeded)
                created.reset();   // the seed console pins the desktop from here on
            else
                log::Warn(std::format(L"[new] seed on '{}' did not land; desktop discarded", name));

            if (!seeded)
            {
                MessageBoxW(g_panel, std::format(
                    L"The console on '{}' did not start properly.\nThe desktop was discarded.", name).c_str(),
                    L"Virtual Desktop", MB_ICONWARNING | MB_TASKMODAL);
                return;
            }

            refresh_list();
            log::Info(std::format(L"[new] '{}' ready (not switched)", name));
        }

        INT_PTR CALLBACK panel_proc(HWND dialog, UINT message, WPARAM wParam, LPARAM)
        {
            switch (message)
            {
            case WM_INITDIALOG:
                g_panel = dialog;
                refresh_list();
                return TRUE;

            case WM_COMMAND:
                if (LOWORD(wParam) == IDC_DESKTOP_LIST && HIWORD(wParam) == LBN_DBLCLK)
                {
                    switch_to(selected_desktop());
                    return TRUE;
                }
                switch (LOWORD(wParam))
                {
                case IDC_NEW:
                    on_new();
                    return TRUE;
                case IDC_SWITCH:
                    switch_to(selected_desktop());
                    return TRUE;
                }
                break;

            case kWmAppEventsPending:
                drain_runner_events();
                return TRUE;

            case WM_CLOSE:
                DestroyWindow(dialog);
                return TRUE;

            case WM_DESTROY:
                // Best effort and non-blocking: the destructor would join each
                // thread, which can stall if a dialog sits in a modal box.
                for (auto& [name, runner] : g_dialogs)
                    runner->RequestClose();
                g_panel = nullptr;
                PostQuitMessage(0);
                return TRUE;
            }
            return FALSE;
        }
    }

    int run(HINSTANCE instance, HANDLE goHome)
    {
        g_instance = instance;
        g_panel = CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_PANEL), nullptr, panel_proc, 0);
        if (!g_panel)
        {
            log::Err(L"[startup] panel creation failed", GetLastError());
            return -1;
        }
        ShowWindow(g_panel, SW_SHOW);

        MSG message;
        for (;;)
        {
            const DWORD wait = MsgWaitForMultipleObjects(1, &goHome, FALSE, INFINITE, QS_ALLINPUT);
            if (wait == WAIT_OBJECT_0)
            {
                on_go_home();
                continue;
            }
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                if (message.message == WM_QUIT)
                    return static_cast<int>(message.wParam);
                if (g_panel && !IsDialogMessageW(g_panel, &message))
                {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
        }
    }
}
