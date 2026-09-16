#pragma once

#include <windows.h>

#include <QCoreApplication>
#include <QtNetwork/QLocalSocket>

#include <QString>

#include <string>

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

    // The whole framing is the terminator, so writing a token is two writes.
    // Flushed: the dock's Ready/Home are the two messages whose sender may
    // have nothing left to do (and no event loop left to run) after them.
    inline void sendToken(QLocalSocket* socket, const char* token)
    {
        socket->write(token);
        socket->write("\n");
        socket->flush();
    }
}

// The dock process (main.cpp --dock <desktop> <pipe>). The process's main
// thread starts on the target desktop via lpDesktop, so QApplication
// initializes there without any SetThreadDesktop. Runs until the manager
// sends Exit; HOME is a request ("home"), never self-destruction.
class Dock
{
public:
    // Hidden at birth: a fresh desktop is never auto-entered. `desktop`
    // must match the desktop this process was launched on. Runs the event
    // loop of the caller's one and only QApplication - a second one in this
    // process silently replaces qApp, and destroying the orphaned first was
    // the dock's exit AV.
    static int run(QCoreApplication& app, const QString& desktop, const QString& pipeName);

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
