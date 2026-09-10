#pragma once

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

// The dock process (main.cpp --dock <desktop> <pipe>): owns the four-
// button dock (HOME / Console / Explorer / Open - no text input) on its
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

private:
    // Targeted launch (CreateProcessW with lpDesktop), verified by the
    // arrival diff (per-launch snapshot: a window here that was not
    // here before - the launched process cannot handshake).
    static bool launchExecutable(const std::wstring& exe, const std::wstring& args,
        const std::wstring& desktop);

    // Documents/URLs: ShellExecuteEx lands on the calling thread's
    // desktop (this process is attached); associations are the system's.
    static void shellOpen(const std::wstring& file);
};
