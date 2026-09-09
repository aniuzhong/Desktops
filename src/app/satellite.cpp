#include "satellite.h"

#include <format>
#include <map>
#include <mutex>
#include <thread>

#include <commdlg.h>
#include <wil/resource.h>

#include "common.h"
#include "logging.h"
#include "runner.h"
#include "resource.h"
#include "wilx/win32_helpers.h"

namespace desktops::satellite
{
    namespace
    {
        HINSTANCE g_module = nullptr;
        std::mutex g_lock;
        std::map<std::wstring, DWORD> g_threads;   // desktop name -> satellite thread id

        struct DialogContext
        {
            std::wstring desktop;
            HWND panel;
            Runner* runner;
        };

        // One satellite per thread; the context lives on the thread's stack and
        // the dialog reaches it through this pointer.
        thread_local DialogContext* t_context = nullptr;

        // The HOME semantic lives here, not in WM_DESTROY: closing the dialog IS
        // the desktop switch, so there is no exit-policy flag to maintain.
        void go_home(DialogContext& context, HWND dialog)
        {
            log::Info(std::format(L"[home] leaving '{}'", context.desktop));
            SwitchInputTo(kDefaultDesktop);
            PostMessageW(context.panel, WM_APP_HOME_RETURN, 0, 0);
            DestroyWindow(dialog);
            PostQuitMessage(0);
        }

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

        INT_PTR CALLBACK run_dialog_proc(HWND dialog, UINT message, WPARAM wParam, LPARAM)
        {
            DialogContext* context = t_context;
            switch (message)
            {
            case WM_INITDIALOG:
                SetWindowTextW(dialog, std::format(L"Run - {}", context->desktop).c_str());
                SetFocus(GetDlgItem(dialog, IDC_COMMAND));
                return FALSE;
            case WM_COMMAND:
                switch (LOWORD(wParam))
                {
                case IDOK:
                    context->runner->Launch(context->desktop, wilx::TryGetWindowText(GetDlgItem(dialog, IDC_COMMAND)));
                    return TRUE;
                case IDC_BROWSE:
                    browse(dialog, GetDlgItem(dialog, IDC_COMMAND));
                    return TRUE;
                case IDCANCEL:   // Esc
                    go_home(*context, dialog);
                    return TRUE;
                }
                break;
            case WM_CLOSE:   // X and Alt+F4
                go_home(*context, dialog);
                return TRUE;
            }
            return FALSE;
        }

        void thread_main(std::wstring desktop, HWND panel)
        {
            // The desktop handle also pins the desktop for as long as the satellite lives.
            wil::unique_hdesk desktopHandle(OpenDesktopW(desktop.c_str(), 0, FALSE, GENERIC_ALL));
            if (!desktopHandle)
            {
                log::Err(std::format(L"[satellite] OpenDesktopW('{}') failed", desktop), GetLastError());
                PostMessageW(panel, WM_APP_SPAWN_FAILED, 0,
                    reinterpret_cast<LPARAM>(new std::wstring(desktop)));
                return;
            }
            if (!SetThreadDesktop(desktopHandle.get()))
            {
                log::Err(L"[satellite] SetThreadDesktop failed", GetLastError());
                return;
            }

            Runner runner;
            DialogContext context{ std::move(desktop), panel, &runner };
            t_context = &context;
            // CreateDialogParamW derives the dialog's desktop from this thread's attachment.
            HWND dialog = CreateDialogParamW(g_module, MAKEINTRESOURCEW(IDD_RUN), nullptr,
                run_dialog_proc, 0);
            if (!dialog)
            {
                log::Err(L"[satellite] dialog creation failed", GetLastError());
                return;
            }
            ShowWindow(dialog, SW_SHOW);

            MSG message;
            while (GetMessageW(&message, nullptr, 0, 0) > 0)
            {
                if (message.hwnd == nullptr && message.message == WM_APP_ACTIVATE)
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

            {
                std::scoped_lock lock(g_lock);
                g_threads.erase(context.desktop);
            }
            log::Info(std::format(L"[satellite] '{}' thread exiting", context.desktop));
        }
    }

    void set_module(HINSTANCE instance)
    {
        g_module = instance;
    }

    void spawn(const std::wstring& desktop, HWND panel)
    {
        std::thread thread(thread_main, desktop, panel);
        {
            std::scoped_lock lock(g_lock);
            g_threads[desktop] = GetThreadId(thread.native_handle());
        }
        thread.detach();
        log::Info(std::format(L"[satellite] spawned for '{}'", desktop));
    }

    void tear_down(const std::wstring& desktop)
    {
        DWORD threadId = 0;
        {
            std::scoped_lock lock(g_lock);
            if (auto it = g_threads.find(desktop); it != g_threads.end())
            {
                threadId = it->second;
                g_threads.erase(it);
            }
        }
        if (threadId)
            PostThreadMessageW(threadId, WM_QUIT, 0, 0);
        log::Info(std::format(L"[satellite] tear-down signaled for '{}'", desktop));
    }

    void activate(const std::wstring& desktop)
    {
        DWORD threadId = 0;
        {
            std::scoped_lock lock(g_lock);
            if (auto it = g_threads.find(desktop); it != g_threads.end())
                threadId = it->second;
        }
        if (threadId)
            PostThreadMessageW(threadId, WM_APP_ACTIVATE, 0, 0);
    }

    bool is_present(const std::wstring& desktop)
    {
        return ProbeProcessWindow(desktop, GetCurrentProcessId(), 0);
    }
}
