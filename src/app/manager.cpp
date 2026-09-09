#include "manager.h"

#include <algorithm>
#include <cstdint>
#include <format>

#include <wil/resource.h>

#include "common.h"
#include "logging.h"
#include "resource.h"
#include "runner_events.h"
#include "wilx/win32_helpers.h"

namespace desktops
{
    namespace
    {
        constexpr DWORD kProbeBudgetMs = 5000;       // dialog readiness budget
        constexpr DWORD kSwitchConfirmMs = 3000;     // undo budget: input must actually arrive

        // What a new desktop is seeded with. This names *what* to run; keeping the
        // console alive (-NoExit) is Runner knowledge, not the caller's.
        constexpr wchar_t kAnchorInput[] = L"powershell.exe";

        std::wstring trim(const std::wstring& value)
        {
            const auto begin = value.find_first_not_of(L" \t");
            if (begin == std::wstring::npos)
                return {};
            const auto end = value.find_last_not_of(L" \t");
            return value.substr(begin, end - begin + 1);
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

        void set_list_items(HWND list, const std::vector<std::wstring>& items)
        {
            SendMessageW(list, LB_RESETCONTENT, 0, 0);
            for (const std::wstring& item : items)
                SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
        }

        std::wstring selected_list_item(HWND list)
        {
            const int index = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
            if (index == LB_ERR)
                return {};
            const int length = static_cast<int>(SendMessageW(list, LB_GETTEXTLEN, index, 0));
            std::wstring name(static_cast<size_t>(length), L'\0');
            SendMessageW(list, LB_GETTEXT, index, reinterpret_cast<LPARAM>(name.data()));
            name.resize(static_cast<size_t>(length));
            return name;
        }
    }

    Manager::Manager(HINSTANCE instance, HANDLE goHome)
        : instance_(instance)
        , go_home_(goHome)
        , runner_(*this)
    {
    }

    int Manager::Run(HINSTANCE instance, HANDLE goHome)
    {
        Manager manager(instance, goHome);
        return manager.Pump();
    }

    int Manager::Pump()
    {
        panel_ = CreateDialogParamW(instance_, MAKEINTRESOURCEW(IDD_PANEL), nullptr,
            PanelProc, reinterpret_cast<LPARAM>(this));
        if (!panel_)
        {
            log::Err(L"[startup] panel creation failed", GetLastError());
            return -1;
        }
        ShowWindow(panel_, SW_SHOW);

        MSG message;
        for (;;)
        {
            const DWORD wait = MsgWaitForMultipleObjects(1, &go_home_, FALSE, INFINITE, QS_ALLINPUT);
            if (wait == WAIT_OBJECT_0)
            {
                OnGoHome();
                continue;
            }
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                if (message.message == WM_QUIT)
                    return static_cast<int>(message.wParam);
                if (panel_ && !IsDialogMessageW(panel_, &message))
                {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
        }
    }

    void Manager::OnLaunchFailed(const std::wstring& desktop, const std::wstring& detail)
    {
        log::Warn(std::format(L"[launch] '{}' on '{}'", detail, desktop));
    }

    INT_PTR CALLBACK Manager::PanelProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_INITDIALOG)
        {
            SetWindowLongPtrW(dialog, GWLP_USERDATA, static_cast<LONG_PTR>(lParam));
            auto* self = reinterpret_cast<Manager*>(lParam);
            self->panel_ = dialog;
            self->RefreshList();
            return TRUE;
        }

        auto* self = reinterpret_cast<Manager*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
        if (!self)
            return FALSE;

        switch (message)
        {
        case WM_COMMAND:
            return self->OnCommand(wParam);
        case kWmAppEventsPending:
            self->OnRunnerEvents();
            return TRUE;
        case WM_CLOSE:
            self->OnClose(dialog);
            return TRUE;
        case WM_DESTROY:
            self->OnDestroy();
            return TRUE;
        }
        return FALSE;
    }

    bool Manager::OnCommand(WPARAM wParam)
    {
        if (LOWORD(wParam) == IDC_DESKTOP_LIST && HIWORD(wParam) == LBN_DBLCLK)
        {
            SwitchTo(SelectedDesktop());
            return true;
        }
        switch (LOWORD(wParam))
        {
        case IDC_NEW:
            OnNew();
            return true;
        case IDC_SWITCH:
            SwitchTo(SelectedDesktop());
            return true;
        }
        return false;
    }

    void Manager::OnRunnerEvents()
    {
        for (const RunnerEvent& event : TakeRunnerEvents())
        {
            switch (event.kind)
            {
            case RunnerEvent::Kind::kHomeReturned:
                OnGoHome();
                break;
            case RunnerEvent::Kind::kSpawnFailed:
                MessageBoxW(panel_, std::format(L"Could not attach to desktop '{}'.", event.desktop).c_str(),
                    L"Virtual Desktop", MB_ICONERROR | MB_TASKMODAL);
                RefreshList();
                break;
            case RunnerEvent::Kind::kFinished:
                // Destroys the RunnerDialog, whose destructor joins the thread —
                // safe here because the thread has already reported it is done.
                dialogs_.erase(event.desktop);
                break;
            }
        }
    }

    void Manager::OnClose(HWND dialog)
    {
        DestroyWindow(dialog);
    }

    void Manager::OnDestroy()
    {
        // Best effort and non-blocking: the destructor would join each thread,
        // which can stall if a dialog sits in a modal box.
        for (auto& entry : dialogs_)
            entry.second->RequestClose();
        panel_ = nullptr;
        PostQuitMessage(0);
    }

    void Manager::RecallPanel()
    {
        ShowWindow(panel_, SW_SHOW);
        SetForegroundWindow(panel_);
    }

    void Manager::RefreshList()
    {
        std::vector<std::wstring> items{ kDefaultDesktop };
        for (const std::wstring& name : ListExtraDesktops())
            items.push_back(name);
        set_list_items(GetDlgItem(panel_, IDC_DESKTOP_LIST), items);
    }

    std::wstring Manager::SelectedDesktop() const
    {
        return selected_list_item(GetDlgItem(panel_, IDC_DESKTOP_LIST));
    }

    void Manager::OnGoHome()
    {
        const std::wstring input = wilx::TryGetInputDesktopName();
        if (_wcsicmp(input.c_str(), kDefaultDesktop) != 0)
            SwitchInputTo(kDefaultDesktop);
        RefreshList();
        RecallPanel();
        log::Info(L"[home] panel is home");
    }

    bool Manager::RunnerPresent(const std::wstring& desktop) const
    {
        return ProbeProcessWindow(desktop, GetCurrentProcessId(), 0);
    }

    void Manager::SpawnRunner(const std::wstring& desktop)
    {
        if (dialogs_.contains(desktop))
            return;
        dialogs_.emplace(desktop, std::make_unique<RunnerDialog>(desktop, panel_, instance_));
    }

    bool Manager::AttachAndSwitch(const std::wstring& name)
    {
        if (!RunnerPresent(name))
            SpawnRunner(name);
        if (!ProbeProcessWindow(name, GetCurrentProcessId(), kProbeBudgetMs))
        {
            if (auto it = dialogs_.find(name); it != dialogs_.end())
                it->second->RequestClose();
            log::Err(std::format(L"[switch] dialog on '{}' never appeared", name), 0);
            MessageBoxW(panel_, std::format(L"Could not attach to desktop '{}'.", name).c_str(),
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
                if (auto it = dialogs_.find(name); it != dialogs_.end())
                    it->second->Activate();
                return true;
            }
            Sleep(100);
        }
        log::Err(std::format(L"[switch] '{}' never became the input desktop", name), 0);
        SwitchInputTo(kDefaultDesktop);
        MessageBoxW(panel_, std::format(L"Switching to '{}' did not take effect.", name).c_str(),
            L"Virtual Desktop", MB_ICONWARNING | MB_TASKMODAL);
        return false;
    }

    void Manager::SwitchTo(const std::wstring& name)
    {
        if (name.empty() || _wcsicmp(name.c_str(), kDefaultDesktop) == 0)
            return;
        log::Info(std::format(L"[switch] to '{}'", name));
        AttachAndSwitch(name);
    }

    INT_PTR CALLBACK Manager::NewDesktopProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_INITDIALOG)
        {
            SetWindowLongPtrW(dialog, GWLP_USERDATA, static_cast<LONG_PTR>(lParam));
            const auto* self = reinterpret_cast<Manager*>(lParam);
            SetDlgItemTextW(dialog, IDC_NEW_NAME, self->pending_name_.c_str());
            SendDlgItemMessageW(dialog, IDC_NEW_NAME, EM_SETSEL, 0, -1);
            SetFocus(GetDlgItem(dialog, IDC_NEW_NAME));
            return FALSE;
        }

        auto* self = reinterpret_cast<Manager*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
        if (!self)
            return FALSE;

        switch (message)
        {
        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case IDOK:
            {
                wchar_t buffer[128] = {};
                GetDlgItemTextW(dialog, IDC_NEW_NAME, buffer, 128);
                self->pending_name_ = trim(buffer);
                if (self->pending_name_.empty())
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

    bool Manager::PromptDesktopName(const std::wstring& suggested, std::wstring& chosen)
    {
        pending_name_ = suggested;
        const INT_PTR result = DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_NEW_DESKTOP),
            panel_, NewDesktopProc, reinterpret_cast<LPARAM>(this));
        chosen = pending_name_;
        return result == IDOK && !chosen.empty();
    }

    void Manager::OnNew()
    {
        // [New] creates and seeds, deliberately without switching: the seed console
        // is what pins the desktop, and the user enters it later via Switch To. The
        // created desktop dies if the seed cannot land — an unentered desktop is not
        // kept, by design.
        const std::vector<std::wstring> taken = ListExtraDesktops();
        std::wstring name;
        if (!PromptDesktopName(next_free_name(taken), name))
            return;

        const bool duplicates = _wcsicmp(name.c_str(), kDefaultDesktop) == 0 ||
            std::any_of(taken.begin(), taken.end(), [&](const std::wstring& existing) {
                return _wcsicmp(existing.c_str(), name.c_str()) == 0;
            });
        if (duplicates)
        {
            MessageBoxW(panel_, std::format(L"A desktop named '{}' already exists.", name).c_str(),
                L"New Desktop", MB_ICONEXCLAMATION | MB_TASKMODAL);
            return;
        }

        log::Info(std::format(L"[new] creating '{}'", name));
        // Held open until the seed proves it landed: a desktop with no window on it
        // dies with its last handle.
        wil::unique_hdesk created(CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr));
        if (!created)
        {
            log::Err(std::format(L"[new] CreateDesktopW('{}') failed", name), GetLastError());
            MessageBoxW(panel_, std::format(L"Could not create desktop '{}' (error {}).", name, GetLastError()).c_str(),
                L"Virtual Desktop", MB_ICONERROR | MB_TASKMODAL);
            return;
        }

        const Runner::LaunchResult anchor = runner_.Launch(name, kAnchorInput);
        const bool seeded = anchor.arrived;
        if (seeded)
            created.reset();   // the seed console pins the desktop from here on
        else
            log::Warn(std::format(L"[new] seed on '{}' did not land; desktop discarded", name));

        if (!seeded)
        {
            MessageBoxW(panel_, std::format(
                L"The console on '{}' did not start properly.\nThe desktop was discarded.", name).c_str(),
                L"Virtual Desktop", MB_ICONWARNING | MB_TASKMODAL);
            return;
        }

        RefreshList();
        log::Info(std::format(L"[new] '{}' ready (not switched)", name));
    }
}
