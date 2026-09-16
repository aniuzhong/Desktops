#pragma once

#include <windows.h>

#include <QString>
#include <QLoggingCategory>

#include <string>
#include <vector>

// One ASCII token per message, '\n'-terminated on the wire; each side
// buffers and splits on '\n'. The protocol lives with the dock: the dock
// defines it, the manager speaks it.
namespace protocol
{
    inline constexpr char Ready[] = "ready";
    inline constexpr char Home[] = "home";

    // Activate when the input desktop moved here, Park when it moved
    // away (hide, keep running), Exit to shut down.
    inline constexpr char Activate[] = "activate";
    inline constexpr char Park[] = "park";
    inline constexpr char Exit[] = "exit";
}

// The dock process (main.cpp --dock <desktop> <pipe>). The process's main
// thread starts on the target desktop via lpDesktop, so QApplication
// initializes there without any SetThreadDesktop. Runs until the manager
// sends Exit; HOME is a request ("home"), never self-destruction.
class Dock
{
public:
    // Hidden at birth: a fresh desktop is never auto-entered. `desktop`
    // must match the desktop this process was launched on.
    static int run(const QString& desktop, const QString& pipeName, int argc, char** argv);

    // Per-app cross-desktop launches. Cross-desktop process creation has
    // app-specific failure modes on this machine, so each function below
    // carries exactly one app's worth of launch knowledge - console or
    // GUI, arguments, and the path spelling that survives here; the
    // evidence sits with the definition. A/B testing an app means
    // changing that one function.
    static bool launchCMD(const std::wstring& desktop, const char* source);
    static bool launchPowershell5(const std::wstring& desktop, const char* source);
    static bool launchNotePad(const std::wstring& desktop, const char* source);

    // No app knowledge: verbatim path, caller supplies everything. The
    // shell-open fallback (associations) covers non-executables.
    static bool launch(const std::wstring& exe, const std::wstring& args,
                       const std::wstring& desktop, DWORD creationFlags, const char* source);

private:
    // Documents/URLs: ShellExecuteEx lands on the calling thread's
    // desktop (this process is attached); associations are the system's.
    static void shellOpen(const std::wstring& file);
};
