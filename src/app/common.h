#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace desktops
{
    inline constexpr wchar_t kDefaultDesktop[] = L"Default";

    // Switchable desktops minus Default, alphabetically. Winlogon/Disconnect are
    // dropped by name; anything else that refuses DESKTOP_SWITCHDESKTOP access is
    // not ours to offer and dropped as well.
    std::vector<std::wstring> ListExtraDesktops();

    // Moves the input desktop; false (with the Win32 error logged) on refusal.
    bool SwitchInputTo(const std::wstring& name);

    // True once a top-level window owned by pid exists on the named desktop.
    // With timeoutMs == 0 performs a single scan.
    bool ProbeProcessWindow(const std::wstring& desktop, DWORD pid, DWORD timeoutMs);

    // One scan: every top-level window owner on the named desktop (empty when the
    // desktop cannot be opened for reading).
    std::vector<DWORD> DesktopWindowPids(const std::wstring& desktop);

    // True once a window owned by a pid NOT in `before` exists on the desktop.
    // This is the arrival test for launches whose real process id differs from
    // the one CreateProcessW returned: packaged-app aliases return a stub pid
    // while the actual process (and its window) belongs to someone else.
    bool ProbeNewWindow(const std::wstring& desktop, const std::vector<DWORD>& before, DWORD timeoutMs);
}
