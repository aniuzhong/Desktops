#include "dock.h"

#include <windows.h>
#include <shellapi.h>

#include <QAbstractButton>
#include <QApplication>
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
#include <functional>
#include <string>
#include <vector>

#include <wil/resource.h>

#include "wilx/desktop_windows.h"

Q_LOGGING_CATEGORY(lcDock, "desktops.dock")

namespace
{
    constexpr wchar_t kPowershellSuffix[] = L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    constexpr wchar_t kExplorerSuffix[] = L"\\explorer.exe";

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

    // Arrival proof for launches: the launched process cannot handshake,
    // so a window whose pid was not here before is the only verdict.
    bool newWindowArrived(HDESK desktop, const std::vector<DWORD>& before, unsigned timeoutMs)
    {
        const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
        for (;;)
        {
            const auto pids = desktopWindowPids(desktop);
            const bool arrived = std::any_of(pids.begin(), pids.end(), [&](DWORD pid) {
                return std::find(before.begin(), before.end(), pid) == before.end();
            });
            if (arrived || ::GetTickCount64() >= deadline)
                return arrived;
            ::Sleep(100);
        }
    }

    bool newWindowArrived(const std::wstring& name, const std::vector<DWORD>& before,
        unsigned timeoutMs)
    {
        wil::unique_hdesk handle(::OpenDesktopW(name.c_str(), 0, FALSE, DESKTOP_READOBJECTS));
        return handle && newWindowArrived(handle.get(), before, timeoutMs);
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

    QIcon stockIcon(SHSTOCKICONID id)
    {
        SHSTOCKICONINFO info{};
        if (FAILED(::SHGetStockIconInfo(id, SHGSI_ICON | SHGSI_LARGEICON, &info)))
            return {};
        return fromHIconHandle(info.hIcon);
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

    // Light theme, macOS-dock-shaped: a translucent rounded tray pinned
    // to the bottom-center of the screen, icon-only buttons with hover
    // tooltips. Win10 has no automatic corner rounding, so the tray is
    // frameless + translucent with a rounded stylesheet; on non-default
    // desktops DWM does not composite and the translucent base degrades
    // to the stylesheet background. Created hidden: a fresh desktop is
    // never auto-entered.
    QWidget* composeDock(const QString& desktop, const std::function<void()>& onDefault,
        const std::function<void()>& onConsole, const std::function<void()>& onExplorer,
        const std::function<void()>& onOpen)
    {
        auto* dock = new QWidget;
        dock->setWindowTitle("Desktops - " + desktop);
        dock->setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
        dock->setAttribute(Qt::WA_TranslucentBackground);
        dock->setStyleSheet(
            "QWidget { background: rgba(243, 243, 243, 216);"
            "  border-radius: 18px; }"
            "QToolButton { background: #ffffff; border: 1px solid #d9d9d9;"
            "  border-radius: 12px; min-width: 72px; min-height: 72px; }"
            "QToolButton:hover { background: #e8f0fe; border-color: #9ab8e8; }"
            "QToolButton:pressed { background: #d7e3fa; }");

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
        addButton(executableIcon(systemDirectory() + kPowershellSuffix), "Console",
            onConsole);
        addButton(executableIcon(windowsDirectory() + kExplorerSuffix), "Explorer",
            onExplorer);
        addButton(folderIcon(), "Open", onOpen);
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
    QApplication app(argc, argv);

    const std::wstring desktopWide = stdW(desktop);
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
    const auto onDefault = [&] {
        qCInfo(lcDock, "HOME pressed - requesting input back to Default");
        send(protocol::Home);
        if (dock)
            dock->hide();   // park immediately; the manager moves input
    };
    const auto onConsole = [&] { launchExecutable(systemDirectory() + kPowershellSuffix,
        L" -NoExit", desktopWide); };
    const auto onExplorer = [&] {
        launchExecutable(windowsDirectory() + kExplorerSuffix, L"", desktopWide);
    };
    const auto onOpen = [&] {
        const QString pick = QFileDialog::getOpenFileName(
            dock, QString(), QString(), "All files (*.*)");
        if (pick.isEmpty())
            return;
        const std::wstring file = stdW(pick);
        if (!launchExecutable(file, L"", desktopWide))
            shellOpen(file);   // documents: associations are the system's
    };

    dock = composeDock(desktop, onDefault, onConsole, onExplorer, onOpen);
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
    QObject::connect(&socket, &QLocalSocket::disconnected, [&] { app.quit(); });

    const int code = app.exec();
    delete dock;
    return code;
}

bool Dock::launchExecutable(const std::wstring& exe, const std::wstring& args,
    const std::wstring& desktop)
{
    // Per-launch snapshot: a window here that was not here before.
    const std::vector<DWORD> before = desktopWindowPids(desktop);
    // Writable command-line buffer: CreateProcessW may rewrite it.
    std::wstring command = L"\"" + exe + L"\"" + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.lpDesktop = const_cast<LPWSTR>(desktop.c_str());
    PROCESS_INFORMATION pi{};
    const bool ok = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
        CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi);
    if (ok)
    {
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        if (newWindowArrived(desktop, before, 5000))
            qCInfo(lcDock, "launch arrived: %ls", exe.c_str());
        else
            qCWarning(lcDock, "launched but no window appeared on the desktop within 5s");
    }
    else
    {
        qCWarning(lcDock, "CreateProcessW('%s') failed (%lu)",
            q(exe).toUtf8().constData(), ::GetLastError());
    }
    return ok;
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
