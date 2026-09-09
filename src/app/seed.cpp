#include "seed.h"

#include <format>

#include <wil/resource.h>

#include "logging.h"

namespace desktops::seed
{
    DWORD powershell(const std::wstring& desktop)
    {
        wchar_t systemDirectory[MAX_PATH] = {};
        const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
        if (length == 0 || length > MAX_PATH - 40)
        {
            log::Err(L"[seed] GetSystemDirectoryW failed", GetLastError());
            return 0;
        }
        const std::wstring exe =
            std::wstring(systemDirectory) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";

        STARTUPINFOW si{ .cb = sizeof(si) };
        si.lpDesktop = const_cast<LPWSTR>(desktop.c_str());
        std::wstring command = std::format(L"\"{}\" -NoExit", exe);
        wil::unique_process_information process;
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE,
                nullptr, nullptr, &si, &process))
        {
            log::Err(std::format(L"[seed] launching powershell on '{}' failed", desktop), GetLastError());
            return 0;
        }
        log::Info(std::format(L"[seed] powershell pid {} on '{}'", process.dwProcessId, desktop));
        return process.dwProcessId;
    }
}
