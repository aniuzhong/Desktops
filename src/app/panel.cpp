#include "panel.h"

#include <algorithm>
#include <format>
#include <memory>
#include <vector>

#include <wil/resource.h>

#include "common.h"
#include "logging.h"
#include "resource.h"
#include "runner.h"
#include "satellite.h"
#include "wilx/win32_helpers.h"

namespace desktops::panel
{
    namespace
    {
        constexpr DWORD kProbeBudgetMs = 5000;       // satellite readiness budget
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

        // Brings the satellite up on the named desktop and moves input there.
        // Returns true when the input desktop has verifiably arrived. The panel
        // thread never exits during any of this, so it can always take the user
        // back home — that is the undo agent.
        bool attach_and_switch(const std::wstring& name)
        {
            if (!satellite::is_present(name))
                satellite::spawn(name, g_panel);
            if (!ProbeProcessWindow(name, GetCurrentProcessId(), kProbeBudgetMs))
            {
                satellite::tear_down(name);
                log::Err(std::format(L"[switch] satellite on '{}' never appeared", name), 0);
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
                    satellite::activate(name);
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

        INT_PTR CALLBACK panel_proc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
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

            case satellite::WM_APP_HOME_RETURN:
                on_go_home();
                return TRUE;

            case satellite::WM_APP_SPAWN_FAILED:
            {
                std::unique_ptr<std::wstring> name(reinterpret_cast<std::wstring*>(lParam));
                MessageBoxW(dialog, std::format(L"Could not attach to desktop '{}'.", *name).c_str(),
                    L"Virtual Desktop", MB_ICONERROR | MB_TASKMODAL);
                refresh_list();
                return TRUE;
            }

            case WM_CLOSE:
                DestroyWindow(dialog);
                return TRUE;

            case WM_DESTROY:
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
