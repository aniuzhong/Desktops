#pragma once

#include <windows.h>

#include <QString>
#include <QLoggingCategory>

#include <string>
#include <vector>

// Line-based QLocalSocket protocol between the manager (panel process) and
// a dock process. One ASCII token per message, '\n'-terminated on the wire;
// each side buffers and splits on '\n'. The protocol lives with the dock:
// the dock defines it, the manager speaks it.
namespace protocol
{
    // dock -> manager
    inline constexpr char Ready[] = "ready";    // dock attached; dock widget exists
    inline constexpr char Home[] = "home";      // user pressed HOME on the dock

    // manager -> dock
    inline constexpr char Activate[] = "activate";  // input desktop moved here: show + raise
    inline constexpr char Park[] = "park";          // input desktop moved away: hide, keep running
    inline constexpr char Exit[] = "exit";          // shut down
}

// The dock process (main.cpp --dock <desktop> <pipe>): owns the six-
// button dock (Default / PowerShell / CMD / NotePad / Explorer / Run -
// no text input) on its
// desktop and every launch performed from it. The process's main thread
// starts on the target desktop via lpDesktop, so QApplication
// initializes there without any SetThreadDesktop. Runs until the
// manager sends Exit; HOME is a request ("home"), never self-destruction.
//
// Communication is line-based over QLocalSocket (protocol above):
//   dock -> manager: Ready, Home
//   manager -> dock: Activate (show + raise), Park (hide), Exit
class Dock
{
public:
    // Composes the dock (hidden at birth: a fresh desktop is never
    // auto-entered), connects to the manager's per-dock socket, sends
    // Ready, and runs the event loop until Exit. `desktop` must match
    // the desktop this process was launched on.
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
    static bool launchExplorer(const std::wstring& desktop, const char* source);

    // No app knowledge: verbatim path, caller supplies everything. The
    // shell-open fallback (associations) covers non-executables.
    static bool launch(const std::wstring& exe, const std::wstring& args,
                       const std::wstring& desktop, DWORD creationFlags, const char* source);

private:
    // Documents/URLs: ShellExecuteEx lands on the calling thread's
    // desktop (this process is attached); associations are the system's.
    static void shellOpen(const std::wstring& file);
};
