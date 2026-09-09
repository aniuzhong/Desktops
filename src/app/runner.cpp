#include "runner.h"

#include <format>
#include <utility>
#include <vector>

#include <shlwapi.h>
#include <wil/resource.h>

#include "common.h"
#include "logging.h"

namespace desktops
{
    namespace
    {
        constexpr DWORD kLandingProbeMs = 5000;

        std::wstring trim(const std::wstring& value)
        {
            const auto begin = value.find_first_not_of(L" \t");
            if (begin == std::wstring::npos)
                return {};
            const auto end = value.find_last_not_of(L" \t");
            return value.substr(begin, end - begin + 1);
        }

        bool file_exists(const std::wstring& path)
        {
            const DWORD attributes = GetFileAttributesW(path.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
        }

        bool has_extension(const std::wstring& name)
        {
            const auto slash = name.find_last_of(L"\\/");
            const auto dot = name.find_last_of(L'.');
            return dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash);
        }

        std::wstring quote(const std::wstring& program, const std::wstring& arguments)
        {
            return std::format(L"\"{}\"{}", program, arguments.empty() ? L"" : std::format(L" {}", arguments));
        }

        std::pair<std::wstring, std::wstring> split_program(const std::wstring& input)
        {
            if (!input.empty() && input.front() == L'"')
            {
                const auto closing = input.find(L'"', 1);
                if (closing != std::wstring::npos)
                    return { input.substr(1, closing - 1), trim(input.substr(closing + 1)) };
                return { input.substr(1), {} };
            }
            const auto space = input.find(L' ');
            if (space == std::wstring::npos)
                return { input, {} };
            return { input.substr(0, space), trim(input.substr(space + 1)) };
        }

        std::wstring url_scheme(const std::wstring& input)
        {
            const auto separator = input.find(L"://");
            if (separator != std::wstring::npos && separator >= 2)
                return input.substr(0, separator);
            if (_wcsnicmp(input.c_str(), L"www.", 4) == 0)
                return L"http";
            return {};
        }

        std::wstring assoc_string(ASSOCSTR field, const std::wstring& subject)
        {
            for (DWORD size = 1024;; size *= 2)
            {
                if (size > 65536)
                    return {};
                std::wstring buffer(size, L'\0');
                if (FAILED(AssocQueryStringW(ASSOCF_NONE, field, subject.c_str(), L"open", buffer.data(), &size)))
                    return {};
                buffer.resize(size);
                while (!buffer.empty() && buffer.back() == L'\0')
                    buffer.pop_back();
                return buffer;
            }
        }

        // The registered open command for `subject` with %1 replaced by the quoted argument.
        std::wstring assoc_command(const std::wstring& subject, const std::wstring& argument)
        {
            std::wstring command = assoc_string(ASSOCSTR_COMMAND, subject);
            if (command.empty())
                return {};
            const auto placeholder = command.find(L"%1");
            if (placeholder == std::wstring::npos)
                return command;
            return command.substr(0, placeholder) + L"\"" + argument + L"\"" + command.substr(placeholder + 2);
        }

        std::wstring app_paths_lookup(std::wstring name)
        {
            if (!has_extension(name))
                name += L".exe";
            for (const HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE })
            {
                wchar_t value[1024];
                DWORD size = sizeof(value);
                const std::wstring key = std::format(
                    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\{}", name);
                if (RegGetValueW(root, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS)
                    continue;
                std::wstring path(value);
                if (path.starts_with(L'"') && path.find(L'"', 1) != std::wstring::npos)
                    path = path.substr(1, path.find(L'"', 1) - 1);
                if (!path.empty())
                    return path;
            }
            return {};
        }

        // Returns the final command line for the typed program name, or empty with `error` set.
        std::wstring resolve_program(const std::wstring& program, const std::wstring& arguments, std::wstring& error)
        {
            if (file_exists(program))
                return quote(program, arguments);
            const std::wstring withExe = has_extension(program) ? program : program + L".exe";
            if (file_exists(withExe))
                return quote(withExe, arguments);

            wchar_t found[MAX_PATH] = {};
            if (SearchPathW(nullptr, program.c_str(), has_extension(program) ? nullptr : L".exe", MAX_PATH, found, nullptr) > 0)
                return quote(found, arguments);

            std::wstring viaAppPaths = app_paths_lookup(program);
            if (!viaAppPaths.empty())
                return quote(viaAppPaths, arguments);

            if (has_extension(program))
            {
                std::wstring command = assoc_command(program, program);
                if (!command.empty())
                    return command;
            }

            error = std::format(L"Windows cannot find '{}'.", program);
            return {};
        }

        // Longest-prefix retry for unquoted paths with spaces: Run accepts
        // "C:\Program Files\Git\git-bash.exe" typed without quotes by trying each
        // space boundary as the program/argument split.
        std::wstring resolve_unquoted_prefixes(const std::wstring& input, std::wstring& error)
        {
            std::vector<size_t> ends;
            ends.push_back(input.size());
            for (auto space = input.find_last_of(L' '); space != std::wstring::npos && space > 0;
                space = input.find_last_of(L' ', space - 1))
                ends.push_back(space);

            for (size_t end : ends)
            {
                const std::wstring candidate = trim(input.substr(0, end));
                if (candidate.empty())
                    continue;
                const std::wstring arguments = end < input.size() ? trim(input.substr(end + 1)) : std::wstring();
                if (file_exists(candidate))
                    return quote(candidate, arguments);
                const std::wstring withExe = has_extension(candidate) ? candidate : candidate + L".exe";
                if (file_exists(withExe))
                    return quote(withExe, arguments);
            }
            error = std::format(L"Windows cannot find '{}'.", input);
            return {};
        }

        // The directory that holds the conhost-hosted shells, or empty.
        std::wstring system_directory()
        {
            wchar_t buffer[MAX_PATH] = {};
            const UINT length = GetSystemDirectoryW(buffer, MAX_PATH);
            if (length == 0 || length > MAX_PATH - 40)
                return {};
            return buffer;
        }
    }

    Runner::Runner(Sink& sink)
        : sink_(sink)
    {
    }

    Runner::Recipe Runner::Classify(std::wstring_view input) const
    {
        const std::wstring text(input);   // _wcsicmp wants null termination
        static constexpr struct
        {
            const wchar_t* name;
            Recipe recipe;
        } kShells[] = {
            { L"powershell", Recipe::kPowerShell5 },
            { L"powershell.exe", Recipe::kPowerShell5 },
            { L"pwsh", Recipe::kPowerShell7 },
            { L"pwsh.exe", Recipe::kPowerShell7 },
            { L"cmd", Recipe::kCmd },
            { L"cmd.exe", Recipe::kCmd },
        };
        for (const auto& shell : kShells)
        {
            if (_wcsicmp(text.c_str(), shell.name) == 0)
                return shell.recipe;
        }
        return url_scheme(text).empty() ? Recipe::kResolvedProgram : Recipe::kUrl;
    }

    Runner::LaunchResult Runner::launch_directory(const std::wstring& desktop, const std::wstring& path)
    {
        return start(desktop, std::format(L"explorer.exe \"{}\"", path));
    }

    Runner::LaunchResult Runner::launch_url(const std::wstring& desktop, const std::wstring& url)
    {
        const std::wstring handler = assoc_string(ASSOCSTR_EXECUTABLE, url_scheme(url));
        if (handler.empty())
        {
            report(desktop, std::format(L"No program is associated with '{}'.", url));
            return {};
        }
        return start(desktop, std::format(L"\"{}\" \"{}\"", handler, url));
    }

    Runner::LaunchResult Runner::launch_program(const std::wstring& desktop, const std::wstring& input)
    {
        auto [program, arguments] = split_program(input);
        std::wstring error;
        std::wstring command = resolve_program(program, arguments, error);
        if (command.empty() && input.find(L' ') != std::wstring::npos && input.front() != L'"')
            command = resolve_unquoted_prefixes(input, error);
        if (command.empty())
        {
            report(desktop, error);
            return {};
        }
        return start(desktop, command);
    }

    Runner::LaunchResult Runner::launch_powershell5(const std::wstring& desktop)
    {
        // TODO: when Windows Terminal is the default terminal, conhost hands the
        // console off to it and the desktop never gets pinned (README "Known
        // Problems"). Detecting that situation belongs here.
        const std::wstring directory = system_directory();
        if (directory.empty())
        {
            report(desktop, L"Could not locate the Windows PowerShell directory.");
            return {};
        }
        // -NoExit is Runner knowledge: a console that exits immediately is not a
        // usable state, and leaves a new desktop with nothing to hold it open.
        return start(desktop, std::format(L"\"{}\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoExit", directory));
    }

    Runner::LaunchResult Runner::launch_powershell7(const std::wstring& desktop)
    {
        std::wstring error;
        const std::wstring command = resolve_program(L"pwsh.exe", std::wstring{}, error);
        if (command.empty())
        {
            // TODO: distinguish a real pwsh.exe from an App Execution Alias — an
            // MSIX alias reports a stub pid and its window belongs elsewhere.
            report(desktop, error.empty() ? L"PowerShell 7 (pwsh.exe) was not found." : error);
            return {};
        }
        return start(desktop, std::format(L"{} -NoExit", command));
    }

    Runner::LaunchResult Runner::launch_cmd(const std::wstring& desktop)
    {
        const std::wstring directory = system_directory();
        if (directory.empty())
        {
            report(desktop, L"Could not locate cmd.exe.");
            return {};
        }
        // /K mirrors -NoExit: a console that exits immediately is not a usable state.
        return start(desktop, std::format(L"\"{}\\cmd.exe\" /K", directory));
    }

    // The single doorway: CreateProcessW with lpDesktop. A success return is no
    // arrival proof — onto a missing desktop it "succeeds" and the child dies
    // within a second, and packaged-app aliases return a stub pid while the
    // real window belongs to a different process — so arrival is judged by
    // snapshot-diff: any window that was not on the desktop before the launch.
    Runner::LaunchResult Runner::start(const std::wstring& desktop, const std::wstring& command)
    {
        log::Info(std::format(L"[launch] command '{}'", command));
        const std::vector<DWORD> before = DesktopWindowPids(desktop);
        STARTUPINFOW si{ .cb = sizeof(si) };
        si.lpDesktop = const_cast<LPWSTR>(desktop.c_str());
        std::wstring mutableCommand = command;
        wil::unique_process_information process;
        if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE,
                nullptr, nullptr, &si, &process))
        {
            log::Err(std::format(L"[launch] CreateProcessW failed for '{}'", command), GetLastError());
            report(desktop, std::format(L"'{}' could not be started (error {}).", command, GetLastError()));
            return {};
        }

        log::Info(std::format(L"[launch] pid {} on '{}'", process.dwProcessId, desktop));
        const bool arrived = ProbeNewWindow(desktop, before, kLandingProbeMs);
        if (!arrived)
        {
            log::Warn(std::format(L"[launch] no new window on '{}' within {} ms", desktop, kLandingProbeMs));
            report(desktop, std::format(L"'{}' started but did not show a window on the desktop.", command));
        }
        return LaunchResult{ process.dwProcessId, arrived };
    }

    void Runner::report(const std::wstring& desktop, const std::wstring& detail)
    {
        sink_.OnLaunchFailed(desktop, detail);
    }

    Runner::LaunchResult Runner::Launch(const std::wstring& desktop, const std::wstring& rawInput)
    {
        const std::wstring input = trim(rawInput);
        if (input.empty())
            return {};
        log::Info(std::format(L"[launch] '{}' on '{}'", input, desktop));

        // Whether the input names a folder is a filesystem property, not a
        // syntactic one, so it is decided here rather than inside Classify().
        const DWORD attributes = GetFileAttributesW(input.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))
            return launch_directory(desktop, input);

        switch (Classify(input))
        {
        case Recipe::kUrl:
            return launch_url(desktop, input);
        case Recipe::kPowerShell5:
            return launch_powershell5(desktop);
        case Recipe::kPowerShell7:
            return launch_powershell7(desktop);
        case Recipe::kCmd:
            return launch_cmd(desktop);
        case Recipe::kResolvedProgram:
            return launch_program(desktop, input);
        }
        return {};
    }
}
