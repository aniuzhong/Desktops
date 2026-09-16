#include "dock.h"

#include <windows.h>
#include <shellapi.h>

#include <QAbstractButton>
#include <QApplication>
#include <QFile>
#include <QFileDialog>
#include <QGuiApplication>
#include <QScreen>
#include <QHBoxLayout>
#include <QImage>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QAction>
#include <QMenu>
#include <QPixmap>
#include <QString>
#include <QToolButton>
#include <QSvgRenderer>
#include <QPainter>

#include <algorithm>
#include <format>
#include <map>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <wil/resource.h>

#include "wallpaper.h"
#include "wilx/desktop.h"
#include "wilx/toolhelp.h"

Q_LOGGING_CATEGORY(lcDock, "desktops.dock")

namespace
{
    constexpr wchar_t kPowershellSuffix[] = L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    constexpr wchar_t kCmdSuffix[] = L"\\cmd.exe";
    constexpr wchar_t kNotepadSuffix[] = L"\\notepad.exe";
    constexpr wchar_t kConhostSuffix[] = L"\\conhost.exe";

    // The fixed height keeps the layout and the positioning maths on one
    // number; kIconSize is 1:1 with the shell's large icon (SHGFI_LARGEICON),
    // so icons are never upscaled.
    constexpr int kBarHeight = 48;
    constexpr int kButtonSize = 40;
    constexpr int kIconSize = 32;

    std::wstring systemDirectory()
    {
        std::wstring path(MAX_PATH, L'\0');
        path.resize(::GetSystemDirectoryW(path.data(), MAX_PATH));
        return path;
    }

    std::wstring forwardSlashed(std::wstring path)
    {
        std::replace(path.begin(), path.end(), wchar_t(92), wchar_t(47));
        return path;
    }

    std::vector<DWORD> desktopWindowPids(HDESK desktop)
    {
        std::vector<DWORD> pids;
        wilx::for_each_desktop_window_nothrow(desktop, [&](HWND window) {
            DWORD pid = 0;
            ::GetWindowThreadProcessId(window, &pid);
            if (pid)
                pids.push_back(pid);
        });
        return pids;
    }

    std::vector<DWORD> desktopWindowPids(const std::wstring& name)
    {
        wil::unique_hdesk handle(::OpenDesktopW(name.c_str(), 0, FALSE, DESKTOP_READOBJECTS));
        return handle ? desktopWindowPids(handle.get()) : std::vector<DWORD> {};
    }

    // Toolhelp parent of pid, or 0 when unanswerable (already reaped, or
    // the snapshot failed).
    DWORD parentPid(DWORD pid)
    {
        DWORD parent = 0;
        wilx::for_each_process([&](const PROCESSENTRY32W& entry) {
            if (entry.th32ProcessID == pid)
            {
                parent = entry.th32ParentProcessID;
                return false;
            }
            return true;
        });
        return parent;
    }

    // Disposition of a launch that never produced a window: still running
    // or the exit code, the only trace left on a desktop no one can see.
    std::string processExitState(DWORD pid)
    {
        wil::unique_handle process(
            ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        if (!process)
            return "already exited";
        DWORD code = 0;
        if (!::GetExitCodeProcess(process.get(), &code))
            return "exit code unavailable";
        if (code == STILL_ACTIVE)
            return "still running";
        return std::format("exited with code {}", code);
    }

    std::wstring windowTitle(HWND window)
    {
        // SendMessageTimeout, not GetWindowText: a cross-process WM_GETTEXT
        // that hangs would hang the dock's UI thread with it.
        std::wstring title(512, L'\0');
        DWORD_PTR copied = 0;
        if (!::SendMessageTimeoutW(window, WM_GETTEXT, title.size(),
                reinterpret_cast<LPARAM>(title.data()), SMTO_ABORTIFHUNG | SMTO_BLOCK, 150, &copied))
        {
            return {};
        }
        title.resize(copied);
        return title;
    }

    std::wstring processImagePath(DWORD pid)
    {
        wil::unique_handle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        if (!process)
            return {};
        wchar_t path[MAX_PATH] = {};
        DWORD size = MAX_PATH;
        return ::QueryFullProcessImageNameW(process.get(), 0, path, &size)
            ? std::wstring(path, size)
            : std::wstring {};
    }

    // A window the user minimized and would expect to find again: iconic but
    // still visible (minimizing clears neither WS_VISIBLE nor the
    // enumeration), not a tool window, unowned or explicitly asking for a
    // taskbar button, and not one of ours.
    bool isRestorableWindow(HWND window, DWORD pid)
    {
        if (pid == ::GetCurrentProcessId() || !::IsIconic(window) || !::IsWindowVisible(window))
            return false;
        const LONG_PTR extended = ::GetWindowLongPtrW(window, GWL_EXSTYLE);
        if ((extended & WS_EX_TOOLWINDOW) && !(extended & WS_EX_APPWINDOW))
            return false;
        if (::GetWindow(window, GW_OWNER) && !(extended & WS_EX_APPWINDOW))
            return false;
        return true;
    }

    // Arrival proof for launches: the launched process cannot handshake,
    // so the verdict is a window whose pid was not here before and that
    // the launch owns - the process itself, or its console host (a console
    // window belongs to conhost.exe, whose parent is the launched
    // process). Strict ownership keeps overlapping launches from claiming
    // each other's windows.
    bool newWindowArrived(HDESK desktop, const std::vector<DWORD>& before,
        DWORD launchedPid, unsigned timeoutMs)
    {
        const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
        for (;;)
        {
            for (DWORD pid : desktopWindowPids(desktop))
            {
                if (std::find(before.begin(), before.end(), pid) != before.end())
                    continue;
                if (pid == launchedPid || parentPid(pid) == launchedPid)
                    return true;
            }
            if (::GetTickCount64() >= deadline)
                return false;
            ::Sleep(100);
        }
    }

    bool newWindowArrived(const std::wstring& name, const std::vector<DWORD>& before,
        DWORD launchedPid, unsigned timeoutMs)
    {
        wil::unique_hdesk handle(::OpenDesktopW(name.c_str(), 0, FALSE, DESKTOP_READOBJECTS));
        return handle && newWindowArrived(handle.get(), before, launchedPid, timeoutMs);
    }

    // HICON -> QIcon without QtWinExtras (dropped in Qt 6): pull the
    // 32bpp color bitmap via GetDIBits and wrap it in a QPixmap. `icon` is
    // never touched - the caller owns it and destroys it exactly once
    // (DestroyIcon twice does not crash, it only fails with
    // ERROR_INVALID_CURSOR_HANDLE, but the handle value may already have been
    // reused, which would destroy somebody else's icon).
    QIcon iconFromHicon(HICON icon)
    {
        ICONINFO info{};
        if (!::GetIconInfo(icon, &info))
            return {};
        // GetIconInfo owns both bitmaps it hands back; only the colour one
        // is read, the mask is carried here for its lifetime alone.
        const wil::unique_hbitmap color(info.hbmColor);
        const wil::unique_hbitmap mask(info.hbmMask);
        QImage image;
        if (color)
        {
            BITMAP bitmap{};
            if (::GetObjectW(color.get(), sizeof(bitmap), &bitmap) != 0)
            {
                BITMAPINFOHEADER header{};
                header.biSize = sizeof(header);
                header.biWidth = bitmap.bmWidth;
                header.biHeight = -bitmap.bmHeight;   // top-down
                header.biPlanes = 1;
                header.biBitCount = 32;
                header.biCompression = BI_RGB;
                image = QImage(bitmap.bmWidth, bitmap.bmHeight, QImage::Format_ARGB32);
                HDC dc = ::CreateCompatibleDC(nullptr);
                ::GetDIBits(dc, color.get(), 0, static_cast<UINT>(bitmap.bmHeight),
                    image.bits(), reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS);
                ::DeleteDC(dc);
            }
        }
        if (image.isNull())
            return {};
        return QPixmap::fromImage(image);
    }

    QIcon executableIcon(const std::wstring& executable)
    {
        // The file must exist for its OWN icon (no SHGFI_USEFILEATTRIBUTES,
        // which yields the generic exe icon).
        SHFILEINFOW info{};
        if (::SHGetFileInfoW(executable.c_str(), 0, &info, sizeof(info),
                SHGFI_ICON | SHGFI_LARGEICON)
            == 0)
            return {};
        // SHGetFileInfoW owns the icon it hands back.
        const wil::unique_hicon owned(info.hIcon);
        return iconFromHicon(owned.get());
    }

    // For icons with no executable file to name: the Win+R "Run" icon,
    // e.g. imageres.dll,100.
    QIcon resourceIcon(const std::wstring& path, int resourceId)
    {
        // Not `large`/`small`: the SDK's MIDL headers #define small as char.
        // ExtractIconExW owns both icons it hands back, and may fill either
        // even when it fails, so both are taken into RAII before any return.
        wil::unique_hicon largeIcon;
        wil::unique_hicon smallIcon;
        if (::ExtractIconExW(path.c_str(), -resourceId, largeIcon.addressof(),
                smallIcon.addressof(), 1) <= 0 || !largeIcon)
        {
            return {};
        }
        return iconFromHicon(largeIcon.get());
    }

    // The app's own SVG (embedded via desktops.qrc), rendered at 3x for
    // crisp scaling down to the 32px button icon.
    QIcon appSvgIcon()
    {
        QSvgRenderer renderer(QStringLiteral(":/resources/desktops.svg"));
        if (!renderer.isValid())
            return {};
        QImage image(96, 96, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        renderer.render(&painter, QRectF(0, 0, 96, 96));
        if (image.isNull())
            return {};
        return QPixmap::fromImage(image);
    }

    // resources/dock.qss carries the rationale behind the colours.
    QString dockStyleSheet()
    {
        QFile file(QStringLiteral(":/resources/dock.qss"));
        if (!file.open(QIODevice::ReadOnly))
        {
            qCWarning(lcDock, "dock style sheet resource failed to open");
            return {};
        }
        return QString::fromUtf8(file.readAll());
    }

    struct MinimizedWindow
    {
        HWND window = nullptr;
        QString name;
        QString detail;
        QIcon icon;
    };

    // The title, unless it is the console host spelling its own path out or
    // missing altogether - then the executable's own name says more.
    QString minimizedWindowName(HWND window, const std::wstring& image)
    {
        const QString title = QString::fromStdWString(windowTitle(window));
        if (!title.trimmed().isEmpty()
            && title.compare(QString::fromStdWString(image), Qt::CaseInsensitive) != 0)
        {
            return title;
        }
        return QString::fromStdWString(image.substr(image.find_last_of(L"\\/") + 1));
    }

    QIcon iconForExecutable(const std::wstring& image)
    {
        // At most once per executable: the menu is rebuilt on every
        // right-click and SHGetFileInfoW is not free.
        static std::map<std::wstring, QIcon> cache;
        const auto it = cache.find(image);
        if (it != cache.end())
            return it->second;
        const QIcon icon = executableIcon(image);
        cache.emplace(image, icon);
        return icon;
    }

    std::vector<MinimizedWindow> collectMinimizedWindows(HDESK desktop)
    {
        std::vector<MinimizedWindow> found;
        wilx::for_each_desktop_window_nothrow(desktop, [&](HWND window) {
            DWORD pid = 0;
            ::GetWindowThreadProcessId(window, &pid);
            if (!isRestorableWindow(window, pid))
                return true;
            const std::wstring image = processImagePath(pid);
            MinimizedWindow entry;
            entry.window = window;
            entry.name = minimizedWindowName(window, image);
            entry.detail = QString("%1 (pid %2)")
                .arg(QString::fromStdWString(image), QString::number(pid));
            entry.icon = iconForExecutable(image);
            found.push_back(std::move(entry));
            return true;
        });
        return found;
    }

    // A minimized window has no shell to represent it on a desktop like this
    // one, so the bar lists them on right-click rather than wearing them:
    // buttons for them pushed the centred launchers off centre. Only the
    // noticing is ours - putting a window back is ShowWindow +
    // SetForegroundWindow, and the focus hand-off works from here because
    // the click that reached us is the last input event.
    void showMinimizedMenu(HDESK desktop, const QPoint& position)
    {
        QMenu menu;
        const std::vector<MinimizedWindow> windows = collectMinimizedWindows(desktop);
        if (windows.empty())
            menu.addAction("No minimized windows")->setEnabled(false);
        for (const MinimizedWindow& entry : windows)
        {
            QAction* action = menu.addAction(entry.icon, entry.name);
            action->setToolTip(entry.detail);
            QObject::connect(action, &QAction::triggered, [window = entry.window] {
                if (!::IsWindow(window))   // gone while the menu was open
                    return;
                ::ShowWindow(window, SW_RESTORE);
                if (!::SetForegroundWindow(window))
                    qCWarning(lcDock, "restore: SetForegroundWindow(hwnd=%p) failed (%lu)",
                        window, ::GetLastError());
            });
        }
        menu.exec(position);
    }

    // A full-width bar docked to the bottom edge. This desktop has no
    // shell, so this bar is the taskbar. Created hidden: a fresh desktop
    // is never auto-entered.
    //
    // Qt::Window, deliberately not Qt::Tool. Tool windows are owned, and
    // Windows hides an owned tool window when the owner goes away (closing
    // a launched app took the bar with it); they are also excluded from
    // Qt's quit-on-last-window-closed count, so closing the Run dialog
    // quit the whole dock process. Keeping out of Alt+Tab - the only thing
    // Tool would buy - is moot here: Alt+Tab is a shell feature and these
    // desktops have no shell.
    //
    // DoesNotAcceptFocus alone: showing the bar never moves the keyboard.
    //
    // StaysOnTop: the bar is a taskbar (Shell_TrayWnd is topmost too), and
    // the topmost band is per-desktop, so this cannot leak anywhere else.
    // Without it the bar was once found buried under the full-screen
    // wallpaper - alive, vis=Y, right rect, no owner - after a launched
    // console window closed and the window manager rearranged the
    // desktop's z-order; with every window NoActivate, nothing re-raised
    // the bar.
    QWidget* composeDock(const QString& desktop, const std::function<void()>& onDefault,
        const std::function<void()>& onPowerShell, const std::function<void()>& onCmd,
        const std::function<void()>& onNotepad,
        const std::function<void()>& onRun)
    {
        auto* dock = new QWidget;
        dock->setWindowTitle("Desktops - " + desktop);
        dock->setWindowFlags(Qt::FramelessWindowHint | Qt::Window |
            Qt::WindowDoesNotAcceptFocus | Qt::WindowStaysOnTopHint);
        dock->setFixedHeight(kBarHeight);
        dock->setStyleSheet(dockStyleSheet());

        // Stretch either side keeps the icons centred in the full-width
        // bar; the bar itself supplies the padding, so no margins.
        auto* row = new QHBoxLayout(dock);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
        row->addStretch();

        // Labels are hover tooltips, not permanent captions (taskbar
        // style).
        const auto addButton = [&](QIcon icon, const QString& label,
                                   const std::function<void()>& handler) {
            auto* button = new QToolButton(dock);
            if (!icon.isNull())
                button->setIcon(std::move(icon));
            button->setIconSize(QSize(kIconSize, kIconSize));
            button->setFixedSize(kButtonSize, kButtonSize);
            button->setToolTip(label);
            row->addWidget(button);
            QObject::connect(button, &QAbstractButton::clicked, handler);
        };
        addButton(appSvgIcon(), "Default", onDefault);
        addButton(executableIcon(systemDirectory() + kPowershellSuffix), "PowerShell", onPowerShell);
        addButton(executableIcon(systemDirectory() + kCmdSuffix), "CMD", onCmd);
        addButton(executableIcon(systemDirectory() + kNotepadSuffix), "NotePad", onNotepad);
        addButton(resourceIcon(systemDirectory() + L"\\imageres.dll", 100), "Run", onRun);
        row->addStretch();
        return dock;
    }

    // Full width of the primary screen, flush against the bottom edge.
    // geometry(), not availableGeometry(): with no shell nothing is
    // reserved, and this bar is what would be reserved.
    void positionAlongBottom(QWidget* dock)
    {
        const QRect screen = QGuiApplication::primaryScreen()->geometry();
        dock->setGeometry(screen.x(), screen.bottom() + 1 - dock->height(), screen.width(),
            dock->height());
    }
}  // namespace

int Dock::run(QCoreApplication& app, const QString& desktop, const QString& pipeName)
{
    // The process was launched with lpDesktop=<desktop>: this main thread is
    // already attached, so Qt initialized on the target desktop without any
    // SetThreadDesktop.
    //
    // This process lives and dies by the manager's pipe (protocol::Exit),
    // not by its windows: a dock can legitimately have no visible window
    // for long stretches, and the Run dialog closing is not a reason to
    // quit. Without this, Qt's default quit-on-last-window-closed ends
    // the process the moment the last counted window closes - which took
    // the bar away mid-launch.
    QApplication::setQuitOnLastWindowClosed(false);

    const std::wstring desktopWide = desktop.toStdWString();
    const wil::unique_hdesk desktopPin(
        ::OpenDesktopW(desktopWide.c_str(), 0, FALSE, GENERIC_ALL));
    if (!desktopPin)
    {
        qCCritical(lcDock, "OpenDesktopW('%s') failed (%lu)",
            desktop.toUtf8().constData(), ::GetLastError());
        return 3;
    }

    QLocalSocket socket;
    QWidget* wallpaper = nullptr;
    QWidget* dock = nullptr;

    // Launches run on detached workers: CreateProcessW onto a desktop can
    // block for a long while and the dock's UI thread must never freeze
    // behind it. Workers touch only their own copies and the thread-safe
    // log. Known-app buttons carry no launch knowledge here - their
    // launchXxx function is the knowledge; the Run dialog goes through
    // the app-free launch with the shell-open fallback.
    using KnownLaunch = bool (*)(const std::wstring& desktop, const char* source);
    const auto launchDetached = [desktopWide](KnownLaunch launch, const char* source) {
        std::thread([desktopWide, source, launch] {
            launch(desktopWide, source);
        }).detach();
    };
    const auto launchDetachedUnknown = [desktopWide](const std::wstring& exe,
        const std::wstring& args, DWORD creationFlags, const char* source) {
        std::thread([desktopWide, exe, args, creationFlags, source] {
            if (!launch(exe, args, desktopWide, creationFlags, source))
                shellOpen(exe);   // not an executable: associations take over
        }).detach();
    };
    const auto onDefault = [&] {
        qCInfo(lcDock, "HOME pressed - requesting input back to Default");
        protocol::sendToken(&socket, protocol::Home);
        if (dock)
            dock->hide();   // park immediately; the manager moves input
    };
    const auto onPowerShell = [&] {
        launchDetached(Dock::launchPowershell5, "btn:PowerShell");
    };
    const auto onCmd = [&] {
        launchDetached(Dock::launchCMD, "btn:CMD");
    };
    const auto onNotepad = [&] {
        launchDetached(Dock::launchNotePad, "btn:NotePad");
    };
    const auto onRun = [&] {
        const QString pick = QFileDialog::getOpenFileName(
            dock, QString(), QString(), "All files (*.*)");
        if (pick.isEmpty())
            return;
        launchDetachedUnknown(pick.toStdWString(), L"", 0, "btn:Run-open");
    };

    // Wallpaper first. Unlike the dock it is shown at birth and never
    // hidden: it is only ever background, so it needs no park/activate
    // handling. The explicit sink makes that permanent - desktop z-order
    // rearranges itself when launched app windows come and go (the
    // wallpaper was once found above the dock bar; see composeDock).
    wallpaper = composeWallpaper();
    if (wallpaper)
    {
        wallpaper->show();
        wallpaper->lower();
    }

    dock = composeDock(desktop, onDefault, onPowerShell, onCmd, onNotepad, onRun);
    positionAlongBottom(dock);
    // Collected on demand, so there is nothing to poll: the menu is only
    // ever built while the user is looking at it.
    dock->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(dock, &QWidget::customContextMenuRequested, [&](const QPoint& position) {
        showMinimizedMenu(desktopPin.get(), dock->mapToGlobal(position));
    });

    socket.connectToServer(pipeName);
    if (!socket.waitForConnected(5000))
    {
        qCCritical(lcDock, "manager pipe connect failed: %s",
            socket.errorString().toUtf8().constData());
        delete wallpaper;
        delete dock;
        return 2;
    }
    protocol::sendToken(&socket, protocol::Ready);

    QObject::connect(&socket, &QLocalSocket::readyRead, [&] {
        while (socket.canReadLine())
        {
            const QByteArray line = socket.readLine().trimmed();
            if (line == protocol::Activate && dock)
            {
                // No activateWindow(): the bar must not take focus, so
                // arriving on a desktop leaves the keyboard where it was.
                dock->show();
                dock->raise();
            }
            else if (line == protocol::Park && dock)
            {
                dock->hide();
            }
            else if (line == protocol::Exit)
            {
                app.quit();
            }
        }
    });
    // The manager never drops the connection on purpose: if the pipe
    // dies the dock is an orphan with no exit path, so it quits.
    QObject::connect(&socket, &QLocalSocket::disconnected, [&] {
        qCInfo(lcDock, "manager pipe disconnected (%s) - quitting",
            socket.errorString().toUtf8().constData());
        app.quit();
    });

    const int code = app.exec();
    delete wallpaper;
    delete dock;
    return code;
}

bool Dock::launchCMD(const std::wstring& desktop, const char* source)
{
    // Console app, hosted through an explicit conhost: with Windows
    // Terminal as the default terminal (HKCU\Console\%%Startup) a bare
    // cmd.exe has its console delegated to the packaged
    // WindowsTerminal.exe, which creates the window on the Default
    // desktop and ignores lpDesktop - reproduced here, the launch's
    // window showed up in EnumDesktopWindows(Default), owned by
    // WindowsTerminal. An explicitly launched conhost bypasses the
    // delegation and creates its console window on the client's desktop.
    // The path must stay backslash-spelled: a forward-slash command line
    // makes cmd.exe exit at once (code 1) and windowless - seen on the
    // 10:09 and 10:31 sessions and reproduced on the Default desktop, so
    // plain cmd behavior, not a desktop effect.
    return launch(systemDirectory() + kConhostSuffix,
        L" \"" + systemDirectory() + kCmdSuffix + L"\"", desktop,
        CREATE_NEW_CONSOLE, source);
}

bool Dock::launchPowershell5(const std::wstring& desktop, const char* source)
{
    // Console app, same explicit-conhost hosting as launchCMD (with
    // Windows Terminal as the default terminal the window would land on
    // the Default desktop). -NoExit keeps the window up. Launches under
    // either spelling - backslash kept as the one with the longer track
    // record (10:57, 11:40 sessions).
    return launch(systemDirectory() + kConhostSuffix,
        L" \"" + systemDirectory() + kPowershellSuffix + L"\" -NoExit", desktop,
        CREATE_NEW_CONSOLE, source);
}

bool Dock::launchNotePad(const std::wstring& desktop, const char* source)
{
    // GUI app: no console flags (a console it never attaches to makes
    // CreateProcessW block for ~30s on a shell-less desktop).
    // Path MUST be forward-slash-spelled on this machine: Huorong's
    // behavior engine (when running) blocks the backslash spelling -
    // NtCreateUserProcess never returns, every dock thread gets
    // suspended from outside, and the dock is silently terminated
    // ~35-76s later (10:57, 11:59, 12:04 wedges; 12:26 clean with
    // Huorong off). The forward-slash command line does not match the
    // engine's pattern and sails through.
    return launch(forwardSlashed(systemDirectory() + kNotepadSuffix), L"",
        desktop, 0, source);
}

bool Dock::launch(const std::wstring& exe, const std::wstring& args,
    const std::wstring& desktop, DWORD creationFlags, const char* source)
{
    // The app-free core: the path arrives exactly as the caller chose to
    // spell it, and `creationFlags` is caller knowledge too.
    const std::vector<DWORD> before = desktopWindowPids(desktop);
    // Writable command-line buffer: CreateProcessW may rewrite it.
    std::wstring command = L"\"" + exe + L"\"" + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.lpDesktop = const_cast<LPWSTR>(desktop.c_str());
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
        creationFlags, nullptr, nullptr, &si, &pi);
    if (!ok)
    {
        qCWarning(lcDock, "[%s] CreateProcessW('%s') failed (%lu)",
            source, QString::fromStdWString(exe).toUtf8().constData(), ::GetLastError());
        return false;
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    const bool arrived = newWindowArrived(desktop, before, pi.dwProcessId, 5000);
    if (!arrived)
        qCWarning(lcDock, "[%s] no window arrived on the desktop within 5s (pid=%lu %s)",
            source, pi.dwProcessId, processExitState(pi.dwProcessId).c_str());
    return true;
}

void Dock::shellOpen(const std::wstring& file)
{
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.lpFile = file.c_str();
    sei.nShow = SW_SHOWNORMAL;
    // The dock's main thread is attached to the desktop, so
    // ShellExecuteEx lands the new process there; associations are the
    // system's job.
    if (!::ShellExecuteExW(&sei))
        qCWarning(lcDock, "ShellExecuteExW failed (%lu)", ::GetLastError());
}
