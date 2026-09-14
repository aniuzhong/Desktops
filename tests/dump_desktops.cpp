//*********************************************************
//
//    dump_desktops - a diagnostic view of the workstation /
//    desktop / thread / process / window relations reachable
//    through wilx primitives alone. First consumer of the
//    growing wilx surface; what this dump cannot say (or can
//    only say through choreography) feeds the relation-model
//    design. Not a pass/fail test: always exits 0.
//
//    Console-ASCII on purpose: redirected output and old
//    conhost codepages must not mangle the view.
//
//*********************************************************
#include <windows.h>

#include <wilx/desktop.h>
#include <wilx/toolhelp.h>
#include <wilx/win32_helpers.h>

#include <wil/resource.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace
{
    constexpr size_t MaxSamplesPerGroup = 8;

    struct ProcessRecord
    {
        DWORD pid = 0;
        DWORD session = 0;
        std::wstring exeName;
    };

    struct ThreadRecord
    {
        DWORD tid = 0;
        DWORD pid = 0;
        std::optional<std::wstring> desktopName; // weak identity: bare name, station unknown
        HRESULT desktopError = S_OK;
    };

    struct WindowRecord
    {
        HWND hwnd = nullptr;
        std::wstring title;
        DWORD tid = 0;
        DWORD pid = 0;
    };

    struct DesktopRecord
    {
        std::wstring name;
        bool input = false;
        std::optional<std::vector<WindowRecord>> windows; // nullopt == desktop not openable
        DWORD windowsError = 0;
    };

    struct WinStaRecord
    {
        std::wstring name;
        bool own = false;
        bool opened = false;
        DWORD openError = 0;
        std::vector<DesktopRecord> desktops;
    };

    std::wstring SameLine(std::wstring text)
    {
        std::ranges::replace_if(text, [](wchar_t c) { return c < L' '; }, L'?');
        return text;
    }

    std::wstring ErrorCell(DWORD error)
    {
        // Failures are data here, never swallowed: the dump's whole point.
        return std::format(L"! error {}: {}", error, SameLine(wilx::TryGetWin32ErrorMessage(error)));
    }

    std::wstring ErrorCell(HRESULT error)
    {
        // Same, for the HRESULT cores: they carry nothing but a Win32 code.
        return std::format(L"! error {}: {}", HRESULT_CODE(error),
            SameLine(wilx::TryGetWin32ErrorMessage(HRESULT_CODE(error))));
    }

    std::wstring ExeNameOrGone(const std::map<DWORD, std::wstring>& exeByPid, DWORD pid)
    {
        const auto exe = exeByPid.find(pid);
        return exe == exeByPid.end() ? L"(gone)" : SameLine(exe->second);
    }

    std::vector<ProcessRecord> CaptureProcesses()
    {
        std::vector<ProcessRecord> processes;
        wilx::for_each_process([&](const PROCESSENTRY32W& entry) {
            ProcessRecord process;
            process.pid = entry.th32ProcessID;
            process.exeName = entry.szExeFile;
            ProcessIdToSessionId(process.pid, &process.session);
            processes.push_back(std::move(process));
            return true;
        });
        std::ranges::sort(processes, std::ranges::less(), &ProcessRecord::pid);
        return processes;
    }

    std::vector<ThreadRecord> CaptureThreadBindings(size_t& pseudoThreads)
    {
        pseudoThreads = 0;
        std::vector<ThreadRecord> threads;
        wilx::for_each_thread([&](const THREADENTRY32& entry) {
            // Toolhelp lists Idle-process pseudo entries with tid 0; they have
            // no desktop binding to query (GetThreadDesktop(0) -> NULL).
            if (0 == entry.th32ThreadID)
            {
                ++pseudoThreads;
                return true;
            }

            ThreadRecord thread;
            thread.tid = entry.th32ThreadID;
            thread.pid = entry.th32OwnerProcessID;

            std::wstring name;
            thread.desktopError = wilx::GetThreadDesktopNameNoThrow(thread.tid, name);
            if (SUCCEEDED(thread.desktopError))
                thread.desktopName = std::move(name);

            threads.push_back(std::move(thread));
            return true;
        });
        std::ranges::sort(threads, [](const ThreadRecord& a, const ThreadRecord& b) {
            return std::tie(a.pid, a.tid) < std::tie(b.pid, b.tid);
        });
        return threads;
    }

    std::vector<WinStaRecord> CaptureStations(const std::wstring& inputDesktopName)
    {
        const std::wstring ownStationName = wilx::TryGetUserObjectName(GetProcessWindowStation());

        std::vector<std::wstring> stationNames;
        wilx::for_each_window_station_nothrow([&](PCWSTR name) {
            stationNames.emplace_back(name);
            return true;
        });
        std::ranges::sort(stationNames, [](const std::wstring& a, const std::wstring& b) {
            return _wcsicmp(a.c_str(), b.c_str()) < 0;
        });

        std::vector<WinStaRecord> stations;
        for (const std::wstring& stationName : stationNames)
        {
            WinStaRecord& station = stations.emplace_back();
            station.name = stationName;
            station.own = (0 == _wcsicmp(stationName.c_str(), ownStationName.c_str()));

            wil::unique_hwinsta opened(OpenWindowStationW(stationName.c_str(), FALSE, WINSTA_ENUMDESKTOPS));
            if (!opened)
            {
                station.openError = GetLastError();
                continue;
            }
            station.opened = true;

            wilx::for_each_desktop_nothrow(opened.get(), [&](PCWSTR desktopName) {
                DesktopRecord& desktop = station.desktops.emplace_back();
                desktop.name = desktopName;
                // Input-desktop identity is weak (a bare name); only the own
                // station may claim it.
                desktop.input = station.own && !inputDesktopName.empty() &&
                    (0 == _wcsicmp(desktopName, inputDesktopName.c_str()));
                return true;
            });
            std::ranges::sort(station.desktops, std::ranges::less(), &DesktopRecord::name);

            // EnumDesktopsW works through the station handle alone, but
            // OpenDesktopW resolves desktop names inside the *process* window
            // station: the probe switches stations and restores on scope exit.
            // A windowed application must not do this while other threads keep
            // windows in the real station; this is a windowless console tool.
            HWINSTA previousStation = GetProcessWindowStation();
            auto restoreStation = wil::scope_exit([&] { SetProcessWindowStation(previousStation); });
            SetProcessWindowStation(opened.get());

            for (DesktopRecord& desktop : station.desktops)
            {
                wil::unique_hdesk desktopHandle(
                    OpenDesktopW(desktop.name.c_str(), 0, FALSE, DESKTOP_READOBJECTS));
                if (!desktopHandle)
                {
                    desktop.windowsError = GetLastError();
                    continue;
                }

                std::vector<WindowRecord> windows;
                wilx::for_each_desktop_window_nothrow(desktopHandle.get(), [&](HWND hwnd) {
                    WindowRecord window;
                    window.hwnd = hwnd;
                    window.title = wilx::TryGetWindowText(hwnd);
                    if (auto owner = wilx::TryGetWindowThreadProcessId(hwnd))
                    {
                        window.tid = owner->threadId;
                        window.pid = owner->processId;
                    }
                    windows.push_back(std::move(window));
                    return true;
                });
                std::ranges::sort(windows, std::ranges::less(), &WindowRecord::hwnd);
                desktop.windows = std::move(windows);
            }
        }
        return stations;
    }

    std::wstring RenderWindows(const std::vector<WindowRecord>& windows, DWORD selfPid)
    {
        if (windows.empty())
        {
            return L"      (no top-level windows)\n";
        }

        std::wstring out;
        const size_t samples = std::min(windows.size(), MaxSamplesPerGroup);
        for (size_t i = 0; i < samples; ++i)
        {
            const WindowRecord& window = windows[i];
            out += std::format(L"      window 0x{:08X} \"{}\" (tid {} -> pid {}{})\n",
                reinterpret_cast<uintptr_t>(window.hwnd), SameLine(window.title), window.tid, window.pid,
                window.pid == selfPid ? L" [self]" : L"");
        }
        if (windows.size() > samples)
            out += std::format(L"      ... and {} more\n", windows.size() - samples);
        return out;
    }

    std::wstring RenderStations(const std::vector<WinStaRecord>& stations, DWORD selfPid)
    {
        std::wstring out;
        for (const WinStaRecord& station : stations)
        {
            out += std::format(L"  {}{}\n", station.name, station.own ? L"  [own]" : L"");
            if (!station.opened)
            {
                out += std::format(L"    station not openable: {}\n", ErrorCell(station.openError));
                continue;
            }

            for (const DesktopRecord& desktop : station.desktops)
            {
                out += std::format(L"    {}{}\n", desktop.name, desktop.input ? L"  [input]" : L"");
                if (desktop.windows)
                    out += RenderWindows(*desktop.windows, selfPid);
                else
                    out += std::format(L"      windows not enumerable: {}\n", ErrorCell(desktop.windowsError));
            }
            if (station.desktops.empty())
            {
                // wilx's total fail-soft: this reads the same whether the
                // station has no desktops or the enumeration was denied.
                out += L"    (no desktops returned)\n";
            }
        }
        return out;
    }

    std::wstring RenderThreads(const std::vector<ThreadRecord>& threads, size_t pseudoThreads,
        const std::map<DWORD, std::wstring>& exeByPid, DWORD selfTid)
    {
        std::map<std::wstring, std::vector<const ThreadRecord*>> byName;
        std::map<HRESULT, std::vector<const ThreadRecord*>> byError;
        for (const ThreadRecord& thread : threads)
        {
            if (thread.desktopName)
                byName[*thread.desktopName].push_back(&thread);
            else
                byError[thread.desktopError].push_back(&thread);
        }

        std::wstring out = std::format(L"[threads -> desktop]  {} thread(s) probed, {} pseudo (tid 0)\n",
        threads.size(), pseudoThreads);
        for (const auto& [name, group] : byName)
        {
            out += std::format(L"  \"{}\" <- {} thread(s)\n", SameLine(name), group.size());
            const size_t samples = std::min(group.size(), MaxSamplesPerGroup);
            for (size_t i = 0; i < samples; ++i)
            {
                const ThreadRecord& thread = *group[i];
                out += std::format(L"    tid {} -> pid {} ({}){}\n", thread.tid, thread.pid,
                    ExeNameOrGone(exeByPid, thread.pid), thread.tid == selfTid ? L" [self]" : L"");
            }
            if (group.size() > samples)
                out += std::format(L"    ... and {} more\n", group.size() - samples);
        }
        for (const auto& [error, group] : byError)
            out += std::format(L"  {} <- {} thread(s)\n", ErrorCell(error), group.size());
        return out;
    }

    std::wstring RenderProcesses(const std::vector<ProcessRecord>& processes, DWORD selfPid)
    {
        size_t nameWidth = 0;
        for (const ProcessRecord& process : processes)
            nameWidth = std::max(nameWidth, process.exeName.size());

        std::wstring out = std::format(L"[processes]  {} process(es)\n", processes.size());
        for (const ProcessRecord& process : processes)
        {
            out += std::format(L"  pid {:>6}  session {:<2}  {:<{}}{}\n", process.pid, process.session,
                SameLine(process.exeName), nameWidth, process.pid == selfPid ? L" [self]" : L"");
        }
        return out;
    }

    std::wstring RenderSignals(const std::vector<WinStaRecord>& stations,
        const std::vector<ThreadRecord>& threads, const std::map<DWORD, std::wstring>& exeByPid)
    {
        std::map<std::wstring, std::vector<std::wstring>> stationsByDesktopName;
        for (const WinStaRecord& station : stations)
            for (const DesktopRecord& desktop : station.desktops)
                stationsByDesktopName[desktop.name].push_back(station.name);

        std::map<DWORD, const ThreadRecord*> threadByTid;
        for (const ThreadRecord& thread : threads)
            threadByTid.emplace(thread.tid, &thread);

        std::wstring out;

        // Weak identity means a bare desktop name can resolve to several
        // stations; the dump surfaces that instead of hiding it.
        for (const auto& [name, stationNames] : stationsByDesktopName)
        {
            if (stationNames.size() > 1)
            {
                std::wstring joined;
                for (const std::wstring& stationName : stationNames)
                    joined += std::format(L"\"{}\" ", stationName);
                out += std::format(L"! desktop name \"{}\" ambiguous across stations: {}\n", SameLine(name), joined);
            }
        }

        std::set<std::wstring> knownDesktopNames;
        for (const auto& [name, stationNames] : stationsByDesktopName)
            knownDesktopNames.insert(name);

        size_t dangling = 0;
        for (const ThreadRecord& thread : threads)
        {
            if (thread.desktopName && !knownDesktopNames.contains(*thread.desktopName))
            {
                if (++dangling <= MaxSamplesPerGroup)
                    out += std::format(L"! thread {} (pid {}, {}) bound to a desktop name no enumeration shows: \"{}\"\n",
                        thread.tid, thread.pid, ExeNameOrGone(exeByPid, thread.pid), SameLine(*thread.desktopName));
            }
        }
        if (dangling > MaxSamplesPerGroup)
            out += std::format(L"  ... and {} more such threads\n", dangling - MaxSamplesPerGroup);

        // Cross-check the two channels that can disagree: a window must live
        // on the desktop its owning thread is bound to.
        const std::vector<WindowRecord> noWindows;
        size_t mismatches = 0;
        for (const WinStaRecord& station : stations)
            for (const DesktopRecord& desktop : station.desktops)
                for (const WindowRecord& window : (desktop.windows ? *desktop.windows : noWindows))
                {
                    const auto owner = threadByTid.find(window.tid);
                    if (owner == threadByTid.end() || !owner->second->desktopName)
                        continue;
                    if (0 != _wcsicmp(owner->second->desktopName->c_str(), desktop.name.c_str()))
                    {
                        if (++mismatches <= MaxSamplesPerGroup)
                            out += std::format(
                                L"! window 0x{:08X} sits on \"{}\" but its thread {} binds to \"{}\" (pid {}, {})\n",
                                reinterpret_cast<uintptr_t>(window.hwnd), SameLine(desktop.name), window.tid,
                                SameLine(*owner->second->desktopName), window.pid, ExeNameOrGone(exeByPid, window.pid));
                    }
                }
        if (mismatches > MaxSamplesPerGroup)
            out += std::format(L"  ... and {} more such windows\n", mismatches - MaxSamplesPerGroup);

        if (out.empty())
            out = L"  (none)\n";
        return L"[signals]\n" + out;
    }
}

int main()
{
    SetConsoleOutputCP(CP_UTF8);

    const DWORD selfPid = GetCurrentProcessId();
    const DWORD selfTid = GetCurrentThreadId();

    DWORD selfSession = 0;
    ProcessIdToSessionId(selfPid, &selfSession);

    std::wstring inputDesktopName;
    const HRESULT inputError = wilx::GetInputDesktopNameNoThrow(inputDesktopName);

    const auto processes = CaptureProcesses();
    size_t pseudoThreads = 0;
    const auto threads = CaptureThreadBindings(pseudoThreads);
    const auto stations = CaptureStations(inputDesktopName);

    std::map<DWORD, std::wstring> exeByPid;
    for (const ProcessRecord& process : processes)
        exeByPid.emplace(process.pid, process.exeName);

    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
    localtime_s(&local, &now);
    wchar_t stamp[32] = {};
    std::wcsftime(stamp, ARRAYSIZE(stamp), L"%Y-%m-%d %H:%M:%S", &local);

    std::wstring dump;
    dump += std::format(L"== wilx desktop dump @ {} ==\n", stamp);
    dump += std::format(L"self: pid {} (session {}), tid {}\n", selfPid, selfSession, selfTid);
    if (!inputDesktopName.empty())
        dump += std::format(L"input desktop: \"{}\"\n", SameLine(inputDesktopName));
    else
        dump += std::format(L"input desktop unavailable: {}\n", ErrorCell(inputError));

    dump += L"\n[stations -> desktops -> windows]\n";
    dump += RenderStations(stations, selfPid);
    dump += L"\n" + RenderThreads(threads, pseudoThreads, exeByPid, selfTid);
    dump += L"\n" + RenderProcesses(processes, selfPid);
    dump += L"\n" + RenderSignals(stations, threads, exeByPid);

    const std::string utf8 = wilx::TryGetUtf8String(dump);
    std::fputs(utf8.c_str(), stdout);
    return 0;
}
