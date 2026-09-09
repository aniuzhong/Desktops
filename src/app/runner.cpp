#include "runner.h"

#include <format>
#include <utility>

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
    }

    void Runner::report(const std::wstring& detail)
    {
        log::Warn(std::format(L"[launch] {}", detail));
        MessageBoxW(nullptr, detail.c_str(), L"Run", MB_ICONWARNING | MB_TOPMOST | MB_TASKMODAL);
    }

    // The single doorway: CreateProcessW with lpDesktop. A success return is no
    // arrival proof — onto a missing desktop it "succeeds" and the child dies
    // within a second, and packaged-app aliases return a stub pid while the
    // real window belongs to a different process — so arrival is judged by
    // snapshot-diff: any window that was not on the desktop before the launch.
    void Runner::start(const std::wstring& desktop, const std::wstring& command)
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
            report(std::format(L"'{}' could not be started (error {}).", command, GetLastError()));
            return;
        }

        log::Info(std::format(L"[launch] pid {} on '{}'", process.dwProcessId, desktop));
        if (!ProbeNewWindow(desktop, before, kLandingProbeMs))
        {
            log::Warn(std::format(L"[launch] no new window on '{}' within {} ms", desktop, kLandingProbeMs));
            report(std::format(L"'{}' started but did not show a window on the desktop.", command));
        }
    }

    void Runner::Launch(const std::wstring& desktop, const std::wstring& rawInput)
    {
        const std::wstring input = trim(rawInput);
        if (input.empty())
            return;
        log::Info(std::format(L"[launch] '{}' on '{}'", input, desktop));

        const DWORD attributes = GetFileAttributesW(input.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            start(desktop, std::format(L"explorer.exe \"{}\"", input));
            return;
        }

        const std::wstring scheme = url_scheme(input);
        if (!scheme.empty())
        {
            const std::wstring handler = assoc_string(ASSOCSTR_EXECUTABLE, scheme);
            if (handler.empty())
            {
                report(std::format(L"No program is associated with '{}'.", input));
                return;
            }
            start(desktop, std::format(L"\"{}\" \"{}\"", handler, input));
            return;
        }

        auto [program, arguments] = split_program(input);
        std::wstring error;
        std::wstring command = resolve_program(program, arguments, error);
        if (command.empty() && input.find(L' ') != std::wstring::npos && input.front() != L'"')
            command = resolve_unquoted_prefixes(input, error);
        if (command.empty())
        {
            report(error);
            return;
        }
        start(desktop, command);
    }
}
