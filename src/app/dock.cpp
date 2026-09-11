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
#include <QPixmap>
#include <QString>
#include <QToolButton>
#include <QSvgRenderer>
#include <QPainter>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <wil/resource.h>

#include "wilx/desktop_windows.h"
#include "wilx/toolhelp.h"
#include "wilx/win32_helpers.h"

Q_LOGGING_CATEGORY(lcDock, "desktops.dock")

namespace
{
    constexpr wchar_t kPowershellSuffix[] = L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    constexpr wchar_t kExplorerSuffix[] = L"\\explorer.exe";
    constexpr wchar_t kCmdSuffix[] = L"\\cmd.exe";
    constexpr wchar_t kNotepadSuffix[] = L"\\notepad.exe";

    QString q(const std::wstring& text)
    {
        return QString::fromWCharArray(text.c_str(), static_cast<qsizetype>(text.size()));
    }

    std::wstring stdW(const QString& text)
    {
        return std::wstring(reinterpret_cast<const wchar_t*>(text.utf16()),
            static_cast<size_t>(text.size()));
    }

    std::wstring systemDirectory()
    {
        std::wstring path(MAX_PATH, L'\0');
        path.resize(::GetSystemDirectoryW(path.data(), MAX_PATH));
        return path;
    }

    std::wstring windowsDirectory()
    {
        std::wstring path(MAX_PATH, L'\0');
        path.resize(::GetWindowsDirectoryW(path.data(), MAX_PATH));
        return path;
    }

    std::wstring forwardSlashed(std::wstring path)
    {
        std::replace(path.begin(), path.end(), wchar_t(92), wchar_t(47));
        return path;
    }

    // PIDs owning top-level windows on the desktop (arrival diff input).
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
        char text[48];
        std::snprintf(text, sizeof(text), "exited with code %lu", code);
        return text;
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
    // 32bpp color bitmap via GetDIBits and wrap it in a QPixmap.
    // Ownership stays with the caller (DestroyIcon after wrapping).
    QIcon iconFromHicon(HICON icon)
    {
        ICONINFO info{};
        if (!::GetIconInfo(icon, &info))
            return {};
        QImage image;
        if (info.hbmColor)
        {
            BITMAP bitmap{};
            if (::GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap) != 0)
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
                ::GetDIBits(dc, info.hbmColor, 0, static_cast<UINT>(bitmap.bmHeight),
                    image.bits(), reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS);
                ::DeleteDC(dc);
            }
        }
        if (info.hbmColor)
            ::DeleteObject(info.hbmColor);
        if (info.hbmMask)
            ::DeleteObject(info.hbmMask);
        ::DestroyIcon(icon);
        if (image.isNull())
            return {};
        return QPixmap::fromImage(image);
    }

    QIcon fromHIconHandle(HICON icon)
    {
        QIcon result = iconFromHicon(icon);
        if (icon)
            ::DestroyIcon(icon);
        return result;
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
        return fromHIconHandle(info.hIcon);
    }

    // An icon baked into a DLL (shell32, imageres, ...) by resource id.
    // Used for icons that don't come from an executable we can name a
    // file for: the Win+R "Run" icon, e.g. imageres.dll,100.
    QIcon resourceIcon(const std::wstring& path, int resourceId)
    {
        // Not `large`/`small`: the SDK's MIDL headers #define small as char.
        HICON largeIcon = nullptr;
        HICON smallIcon = nullptr;
        const int extracted = ::ExtractIconExW(path.c_str(), -resourceId, &largeIcon,
            &smallIcon, 1);
        if (extracted <= 0 || !largeIcon)
        {
            if (largeIcon)
                ::DestroyIcon(largeIcon);
            if (smallIcon)
                ::DestroyIcon(smallIcon);
            return {};
        }
        QIcon icon = iconFromHicon(largeIcon);   // destroys largeIcon
        if (smallIcon)
            ::DestroyIcon(smallIcon);
        return icon;
    }

    QIcon folderIcon()
    {
        // SHGetStockIconInfo(SIID_FOLDER) failed on this machine; a real
        // directory through SHGetFileInfo is the proven path.
        SHFILEINFOW info{};
        if (::SHGetFileInfoW(L"C:\\Windows", 0, &info, sizeof(info),
                SHGFI_ICON | SHGFI_LARGEICON)
            == 0)
            return {};
        return fromHIconHandle(info.hIcon);
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

    // Splits complete '\n' lines out of the socket buffer, leaving any
    // remainder for the next readyRead.
    std::vector<QByteArray> takeLines(QByteArray& buffer)
    {
        std::vector<QByteArray> lines;
        for (int newline = buffer.indexOf('\n'); newline >= 0; newline = buffer.indexOf('\n'))
        {
            lines.push_back(buffer.left(newline));
            buffer.remove(0, newline + 1);
        }
        return lines;
    }

    // The dock's look lives in resources/dock.qss, compiled into the exe
    // by desktops.qrc; the file carries the rationale behind the colours.
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

    // Created hidden: a fresh desktop is never auto-entered.
    QWidget* composeDock(const QString& desktop, const std::function<void()>& onDefault,
        const std::function<void()>& onPowerShell, const std::function<void()>& onCmd,
        const std::function<void()>& onNotepad, const std::function<void()>& onExplorer,
        const std::function<void()>& onRun)
    {
        auto* dock = new QWidget;
        dock->setWindowTitle("Desktops - " + desktop);
        dock->setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
        dock->setStyleSheet(dockStyleSheet());

        auto* row = new QHBoxLayout(dock);
        row->setContentsMargins(20, 16, 20, 16);
        row->setSpacing(20);

        // Labels are hover tooltips, not permanent captions (macOS dock
        // style).
        const auto addButton = [&](QIcon icon, const QString& label,
                                   const std::function<void()>& handler) {
            auto* button = new QToolButton(dock);
            if (!icon.isNull())
                button->setIcon(std::move(icon));
            button->setIconSize(QSize(40, 40));
            button->setToolTip(label);
            row->addWidget(button);
            QObject::connect(button, &QAbstractButton::clicked, handler);
        };
        addButton(appSvgIcon(), "Default", onDefault);
        addButton(executableIcon(systemDirectory() + kPowershellSuffix), "PowerShell",
            onPowerShell);
        addButton(executableIcon(systemDirectory() + kCmdSuffix), "CMD", onCmd);
        addButton(executableIcon(systemDirectory() + kNotepadSuffix), "NotePad", onNotepad);
        addButton(executableIcon(windowsDirectory() + kExplorerSuffix), "Explorer",
            onExplorer);
        addButton(resourceIcon(systemDirectory() + L"\\imageres.dll", 100), "Run", onRun);
        return dock;
    }

    // Bottom-center of the primary screen, small margin above the edge.
    void positionAtBottomCenter(QWidget* dock)
    {
        dock->adjustSize();
        const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
        dock->move(available.x() + (available.width() - dock->width()) / 2,
            available.bottom() - dock->height() - 12);
    }
}  // namespace

int Dock::run(const QString& desktop, const QString& pipeName, int argc, char** argv)
{
    // The process was launched with lpDesktop=<desktop>: this main
    // thread is already attached, so QApplication initializes on the
    // target desktop without any SetThreadDesktop.
    //
    // Disable IME/TSF BEFORE Qt initializes: text services on non-default
    // desktops are half-broken (README Known Problems; they spawn a TSF
    // thread + Cicero windows per desktop and correlate with the heap
    // corruption in WER). Typing keeps working; only composition is lost,
    // which never worked here anyway.
    ::ImmDisableIME(0);

    QApplication app(argc, argv);

    const std::wstring desktopWide = stdW(desktop);
    qCInfo(lcDock,
        "dock starting: desktop='%s' pipe='%s' pid=%lu mainTid=%lu threadDesktop='%s' (verifies the lpDesktop attach)",
        q(desktopWide).toUtf8().constData(), pipeName.toUtf8().constData(),
        ::GetCurrentProcessId(),
        ::GetCurrentThreadId(),
        q(wilx::TryGetThreadDesktopName()).toUtf8().constData());
    const wil::unique_hdesk desktopPin(
        ::OpenDesktopW(desktopWide.c_str(), 0, FALSE, GENERIC_ALL));
    if (!desktopPin)
    {
        qCCritical(lcDock, "OpenDesktopW('%s') failed (%lu)",
            q(desktopWide).toUtf8().constData(), ::GetLastError());
        return 3;
    }

    QLocalSocket socket;
    QByteArray inbox;
    QWidget* dock = nullptr;

    const auto send = [&socket](const char* token) {
        socket.write(token);
        socket.write("\n", 1);
        socket.flush();
    };
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
        send(protocol::Home);
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
    const auto onExplorer = [&] {
        launchDetached(Dock::launchExplorer, "btn:Explorer");
    };
    const auto onRun = [&] {
        const QString pick = QFileDialog::getOpenFileName(
            dock, QString(), QString(), "All files (*.*)");
        if (pick.isEmpty())
            return;
        launchDetachedUnknown(stdW(pick), L"", 0, "btn:Run-open");
    };

    dock = composeDock(desktop, onDefault, onPowerShell, onCmd, onNotepad, onExplorer,
        onRun);
    positionAtBottomCenter(dock);

    socket.connectToServer(pipeName);
    if (!socket.waitForConnected(5000))
    {
        qCCritical(lcDock, "manager pipe connect failed: %s",
            socket.errorString().toUtf8().constData());
        delete dock;
        return 2;
    }
    send(protocol::Ready);
    qCInfo(lcDock, "ready reported to manager");

    QObject::connect(&socket, &QLocalSocket::readyRead, [&] {
        inbox += socket.readAll();
        for (const QByteArray& line : takeLines(inbox))
        {
            if (line == protocol::Activate && dock)
            {
                dock->show();
                dock->raise();
                dock->activateWindow();
            }
            else if (line == protocol::Park && dock)
            {
                dock->hide();
            }
            else if (line == protocol::Exit)
            {
                qCInfo(lcDock, "exit requested by manager");
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
    qCInfo(lcDock, "dock for '%s' exiting with code %d",
        q(desktopWide).toUtf8().constData(), code);
    delete dock;
    return code;
}

bool Dock::launchCMD(const std::wstring& desktop, const char* source)
{
    // Console app. The path must stay backslash-spelled: a forward-slash
    // command line makes cmd.exe exit at once (code 1) and windowless -
    // seen on the 10:09 and 10:31 sessions and reproduced on the Default
    // desktop, so plain cmd behavior, not a desktop effect.
    return launch(systemDirectory() + kCmdSuffix, L"", desktop,
        CREATE_NEW_CONSOLE, source);
}

bool Dock::launchPowershell5(const std::wstring& desktop, const char* source)
{
    // Console app; -NoExit keeps the window up. Launches under either
    // spelling - backslash kept as the one with the longer track record
    // (10:57, 11:40 sessions).
    return launch(systemDirectory() + kPowershellSuffix, L" -NoExit", desktop,
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

bool Dock::launchExplorer(const std::wstring& desktop, const char* source)
{
    // GUI app, no console flags; forward slash like NotePad - explorer
    // has never launched any other way from here.
    return launch(forwardSlashed(windowsDirectory() + kExplorerSuffix), L"",
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
    qCInfo(lcDock,
        "[%s] CreateProcessW: exe='%s' args='%s' cmd='%s' lpDesktop='%s' flags=0x%lx "
        "callerPid=%lu callerTid=%lu",
        source, q(exe).toUtf8().constData(), q(args).toUtf8().constData(),
        q(command).toUtf8().constData(), q(desktop).toUtf8().constData(),
        creationFlags, ::GetCurrentProcessId(), ::GetCurrentThreadId());
    const ULONGLONG createStartedAt = ::GetTickCount64();
    const BOOL ok = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
        creationFlags, nullptr, nullptr, &si, &pi);
    const unsigned long long createTook = ::GetTickCount64() - createStartedAt;
    if (!ok)
    {
        qCWarning(lcDock, "[%s] CreateProcessW('%s') failed (%lu)",
            source, q(exe).toUtf8().constData(), ::GetLastError());
        return false;
    }
    qCInfo(lcDock, "[%s] launched pid=%lu in %llums", source, pi.dwProcessId, createTook);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    const bool arrived = newWindowArrived(desktop, before, pi.dwProcessId, 5000);
    if (arrived)
        qCInfo(lcDock, "[%s] arrival ok", source);
    else
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
