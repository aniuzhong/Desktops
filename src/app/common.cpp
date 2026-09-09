#include "common.h"

#include <algorithm>
#include <format>

#include <wil/resource.h>

#include "logging.h"
#include "wilx/desktop_windows.h"
#include "wilx/desktops.h"

namespace desktops
{
    namespace
    {
        bool is_system_desktop(const std::wstring& name)
        {
            return _wcsicmp(name.c_str(), L"Winlogon") == 0 || _wcsicmp(name.c_str(), L"Disconnect") == 0;
        }
    }

    std::vector<std::wstring> list_extra_desktops()
    {
        std::vector<std::wstring> names;
        wilx::for_each_desktop_nothrow([&](PCWSTR rawName) {
            std::wstring name(rawName);
            if (_wcsicmp(name.c_str(), kDefaultDesktop) == 0 || is_system_desktop(name))
                return true;
            wil::unique_hdesk probe(OpenDesktopW(name.c_str(), 0, FALSE, DESKTOP_SWITCHDESKTOP | DESKTOP_READOBJECTS));
            if (probe)
                names.push_back(std::move(name));
            return true;
        });
        std::sort(names.begin(), names.end(),
            [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
        return names;
    }

    bool switch_input_to(const std::wstring& name)
    {
        log::Info(std::format(L"[switch] moving input desktop to '{}'", name));
        wil::unique_hdesk desktop(OpenDesktopW(name.c_str(), 0, FALSE, DESKTOP_SWITCHDESKTOP));
        if (!desktop)
        {
            log::Err(std::format(L"[switch] OpenDesktopW('{}') failed", name), GetLastError());
            return false;
        }
        if (!SwitchDesktop(desktop.get()))
        {
            log::Err(std::format(L"[switch] SwitchDesktop('{}') failed", name), GetLastError());
            return false;
        }
        return true;
    }

    bool probe_process_window(const std::wstring& desktop, DWORD pid, DWORD timeoutMs)
    {
        wil::unique_hdesk handle(OpenDesktopW(desktop.c_str(), 0, FALSE, DESKTOP_READOBJECTS));
        if (!handle)
        {
            log::Err(std::format(L"[probe] OpenDesktopW('{}') failed", desktop), GetLastError());
            return false;
        }
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        for (;;)
        {
            bool found = false;
            wilx::for_each_desktop_window_nothrow(handle.get(), [&](HWND window) {
                DWORD owner = 0;
                GetWindowThreadProcessId(window, &owner);
                found = found || owner == pid;
            });
            if (found)
                return true;
            if (GetTickCount64() >= deadline)
                return false;
            Sleep(100);
        }
    }

    std::vector<DWORD> desktop_window_pids(const std::wstring& desktop)
    {
        std::vector<DWORD> pids;
        wil::unique_hdesk handle(OpenDesktopW(desktop.c_str(), 0, FALSE, DESKTOP_READOBJECTS));
        if (!handle)
            return pids;
        wilx::for_each_desktop_window_nothrow(handle.get(), [&](HWND window) {
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            if (owner)
                pids.push_back(owner);
        });
        return pids;
    }

    bool probe_new_window(const std::wstring& desktop, const std::vector<DWORD>& before, DWORD timeoutMs)
    {
        const auto arrived = [&before](const std::vector<DWORD>& pids) {
            return std::any_of(pids.begin(), pids.end(), [&](DWORD pid) {
                return std::find(before.begin(), before.end(), pid) == before.end();
            });
        };
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        for (;;)
        {
            if (arrived(desktop_window_pids(desktop)))
                return true;
            if (GetTickCount64() >= deadline)
                return false;
            Sleep(100);
        }
    }
}
