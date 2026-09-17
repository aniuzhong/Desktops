#include <windows.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMessageBox>
#include <QMutex>
#include <QStandardPaths>
#include <QString>

#include <string>

#include "dock.h"
#include "mainwindow.h"
#include "wilx/desktop.h"

namespace
{
    QString g_logPath;
    QMutex g_logMutex;

    void logHandler(QtMsgType type, const QMessageLogContext& context, const QString& message)
    {
        const QMutexLocker lock(&g_logMutex);
        QFile file(g_logPath);
        if (file.size() > 1024 * 1024)
        {
            QFile::remove(g_logPath + ".1");
            QFile::rename(g_logPath, g_logPath + ".1");
        }
        if (!file.open(QIODevice::Append | QIODevice::Text))
            return;
        file.write(qFormatLogMessage(type, context, message).toUtf8());
        file.putChar('\n');
    }

    void installLogging()
    {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/logs";
        QDir().mkpath(dir);
        g_logPath = dir + "/desktops.log";
        // The manager and every dock share this file: P/T is what tells them
        // apart. Qt's own placeholders cover the rest; %{category} is kept
        // conditional so plain qWarning() lines stay uncluttered.
        qSetMessagePattern("[%{time yyyy-MM-dd hh:mm:ss.zzz}] [P%{pid} T%{threadid}] "
            "[%{type}] %{if-category}%{category}: %{endif}%{message}");
        qInstallMessageHandler(logHandler);
    }
}  // namespace

int main(int argc, char* argv[])
{
    // Light theme regardless of the system's dark-mode preference.
    qputenv("QT_QPA_PLATFORM", "windows:darkmode=0");

    // This is the process's only QApplication: a second one silently
    // replaces qApp and leaves the first to be destroyed against
    // already-torn-down Qt state - that was the dock's exit AV.
    QApplication app(argc, argv);
    app.setApplicationName("Desktops");

    // The command line is how the manager hands a dock its identity, and
    // that identity is Unicode: arguments() reads the wide command line
    // (GetCommandLine), not main's argv, which the CRT has already converted
    // through the process ANSI code page (1252 on this machine) - that turns
    // a desktop named 桌面1 into "??1".
    const QStringList arguments = QCoreApplication::arguments();

    if (arguments.size() >= 4 && arguments.at(1) == QStringLiteral("--dock"))
    {
        installLogging();
        return Dock::run(app, arguments.at(2), arguments.at(3));
    }

    // No organization name: it would insert a level into AppLocalDataLocation.
    // Without this the panel's window has no icon at all (WM_GETICON returns
    // null for both sizes) and the taskbar shows the generic one, whatever the
    // exe's resource says.
    app.setWindowIcon(QIcon(QStringLiteral(":/resources/desktops.ico")));
    installLogging();

    // Read once: a process's session is fixed at creation and no API moves a
    // running one (WTSGetActiveConsoleSessionId would follow fast user
    // switching and is deliberately not used). Must stay after installLogging
    // so a failure here still reaches desktops.log.
    DWORD session = 0;
    const HRESULT hr = wilx::GetCurrentSessionIdNoThrow(session);
    if (FAILED(hr))
    {
        qFatal("cannot resolve my session: 0x%08X", static_cast<unsigned>(hr));
    }
    const QString instanceTag = QString::number(session);
    // Session-scoped: fast user switching must not make two sessions recall
    // each other.
    const QString instancePipe = QString("Desktops-instance-%1").arg(instanceTag);

    {
        QLocalSocket probe;
        probe.connectToServer(instancePipe);
        if (probe.waitForConnected(300))
        {
            protocol::sendToken(&probe, protocol::Home);
            probe.waitForBytesWritten(500);
            return 0;
        }
    }

    // The panel lives on Default only: launched from elsewhere is a refusal,
    // never a relocation.
    const std::wstring threadDesktop = wilx::TryGetThreadDesktopName();
    if (_wcsicmp(threadDesktop.c_str(), L"Default") != 0)
    {
        qWarning("launched on '%ls', refusing", threadDesktop.c_str());
        QMessageBox::information(nullptr, "Desktops",
            "Desktops runs on the Default desktop.\n"
            "Switch back to Default and start it there.");
        return 0;
    }

    // No removeServer(): a no-op on Windows, where two servers may share one
    // pipe name - the probe above is the only single-instance gate.
    QLocalServer instanceServer;
    if (!instanceServer.listen(instancePipe))
    {
        qCritical("instance pipe listen failed: %s", instanceServer.errorString().toUtf8().constData());
        return -1;
    }

    MainWindow window(instanceTag);
    window.show();

    QObject::connect(&instanceServer, &QLocalServer::newConnection, [&] {
        if (QLocalSocket* connection = instanceServer.nextPendingConnection())
        {
            QObject::connect(connection, &QLocalSocket::readyRead, [connection, &window] {
                if (QString::fromUtf8(connection->readAll()).contains(protocol::Home))
                    window.goHome();
                connection->disconnectFromServer();
            });
        }
    });

    const int managerResult = app.exec();
    // The dock's ExitProcess came off with the fix for its double
    // QApplication; the manager's teardown has not been re-checked since, so
    // it still skips its own.
    ::ExitProcess(static_cast<UINT>(managerResult));
}
