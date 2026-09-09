#include "runner_dialog.h"

#include <format>

#include <commdlg.h>
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
        void browse(HWND dialog, HWND edit)
        {
            wchar_t file[MAX_PATH] = {};
            OPENFILENAMEW ofn{ .lStructSize = sizeof(ofn) };
            ofn.hwndOwner = dialog;
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
            if (GetOpenFileNameW(&ofn))
                SetWindowTextW(edit, file);
        }
    }

    void RunnerDialog::DialogSink::OnLaunchFailed(const std::wstring& desktop, const std::wstring& detail)
    {
        log::Warn(std::format(L"[launch] '{}' on '{}'", detail, desktop));
        MessageBoxW(nullptr, detail.c_str(), L"Run", MB_ICONWARNING | MB_TOPMOST | MB_TASKMODAL);
    }

    RunnerDialog::RunnerDialog(std::wstring desktop, HWND panel, HINSTANCE module)
        : desktop_(std::move(desktop))
        , panel_(panel)
        , module_(module)
        , runner_(sink_)
    {
        thread_ = std::thread(&RunnerDialog::ThreadMain, this);
        thread_id_ = GetThreadId(thread_.native_handle());
        log::Info(std::format(L"[dialog] spawned for '{}'", desktop_));
    }

    RunnerDialog::~RunnerDialog()
    {
        RequestClose();
        if (thread_.joinable())
            thread_.join();
    }

    void RunnerDialog::RequestClose()
    {
        if (thread_id_ != 0)
            PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
    }

    void RunnerDialog::Activate()
    {
        if (thread_id_ != 0)
            PostThreadMessageW(thread_id_, kWmAppActivate, 0, 0);
    }

    void RunnerDialog::ThreadMain()
    {
        // The desktop handle also pins the desktop for as long as the dialog lives.
        wil::unique_hdesk desktopHandle(OpenDesktopW(desktop_.c_str(), 0, FALSE, GENERIC_ALL));
        if (!desktopHandle)
        {
            log::Err(std::format(L"[dialog] OpenDesktopW('{}') failed", desktop_), GetLastError());
            PostRunnerEvent(panel_, { RunnerEvent::Kind::kSpawnFailed, desktop_ });
            PostRunnerEvent(panel_, { RunnerEvent::Kind::kFinished, desktop_ });
            return;
        }
        if (!SetThreadDesktop(desktopHandle.get()))
        {
            log::Err(L"[dialog] SetThreadDesktop failed", GetLastError());
            PostRunnerEvent(panel_, { RunnerEvent::Kind::kSpawnFailed, desktop_ });
            PostRunnerEvent(panel_, { RunnerEvent::Kind::kFinished, desktop_ });
            return;
        }

        // CreateDialogParamW derives the dialog's desktop from this thread's attachment.
        const HWND dialog = CreateDialogParamW(module_, MAKEINTRESOURCEW(IDD_RUN), nullptr,
            DialogProc, reinterpret_cast<LPARAM>(this));
        if (!dialog)
        {
            log::Err(L"[dialog] dialog creation failed", GetLastError());
            PostRunnerEvent(panel_, { RunnerEvent::Kind::kSpawnFailed, desktop_ });
            PostRunnerEvent(panel_, { RunnerEvent::Kind::kFinished, desktop_ });
            return;
        }
        ShowWindow(dialog, SW_SHOW);

        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            if (message.hwnd == nullptr && message.message == kWmAppActivate)
            {
                // The panel posts this right after switching input here: without
                // it the dialog sits behind whatever owns the desktop's foreground.
                SetForegroundWindow(dialog);
                SetFocus(GetDlgItem(dialog, IDC_COMMAND));
                continue;
            }
            if (!IsDialogMessageW(dialog, &message))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        log::Info(std::format(L"[dialog] '{}' thread exiting", desktop_));
        PostRunnerEvent(panel_, { RunnerEvent::Kind::kFinished, desktop_ });
    }

    // The HOME semantic lives here, not in WM_DESTROY: closing the dialog IS
    // the desktop switch, so there is no exit-policy flag to maintain.
    void RunnerDialog::GoHome(HWND dialog)
    {
        log::Info(std::format(L"[home] leaving '{}'", desktop_));
        SwitchInputTo(kDefaultDesktop);
        PostRunnerEvent(panel_, { RunnerEvent::Kind::kHomeReturned, desktop_ });
        DestroyWindow(dialog);
        PostQuitMessage(0);
    }

    INT_PTR CALLBACK RunnerDialog::DialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_INITDIALOG)
        {
            SetWindowLongPtrW(dialog, GWLP_USERDATA, static_cast<LONG_PTR>(lParam));
            const auto* self = reinterpret_cast<RunnerDialog*>(lParam);
            SetWindowTextW(dialog, std::format(L"Run - {}", self->desktop_).c_str());
            SetFocus(GetDlgItem(dialog, IDC_COMMAND));
            return FALSE;
        }

        auto* self = reinterpret_cast<RunnerDialog*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
        if (!self)
            return FALSE;

        switch (message)
        {
        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case IDOK:
                self->runner_.Launch(self->desktop_, wilx::TryGetWindowText(GetDlgItem(dialog, IDC_COMMAND)));
                return TRUE;
            case IDC_BROWSE:
                browse(dialog, GetDlgItem(dialog, IDC_COMMAND));
                return TRUE;
            case IDCANCEL:   // Esc
                self->GoHome(dialog);
                return TRUE;
            }
            break;
        case WM_CLOSE:   // X and Alt+F4
            self->GoHome(dialog);
            return TRUE;
        }
        return FALSE;
    }
}
