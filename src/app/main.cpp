#include <windows.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
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
    QString sessionTag()
    {
        DWORD session = 0;
        ::ProcessIdToSessionId(::GetCurrentProcessId(), &session);
        return QString::number(session);
    }

    QString instancePipe()
    {
        // Session-scoped: fast user switching must not make two sessions
        // recall each other.
        return QString("Desktops-instance-%1").arg(sessionTag());
    }

    QString g_logPath;
    QMutex g_logMutex;

    const char* logLevelName(QtMsgType type)
    {
        switch (type)
        {
        case QtDebugMsg: return "debug";
        case QtInfoMsg: return "info";
        case QtWarningMsg: return "warn";
        default: return "error";   // critical + fatal
        }
    }

    void logHandler(QtMsgType type, const QMessageLogContext&, const QString& message)
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
        file.write(QString("[%1] [P%2 T%3] [%4] %5\n")
            .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz"))
            .arg(::GetCurrentProcessId())
            .arg(::GetCurrentThreadId())
            .arg(logLevelName(type), message)
            .toUtf8());
    }

    void installLogging()
    {
        const QString dir = QStandardPaths::writableLocation(
            QStandardPaths::AppLocalDataLocation) + "/logs";
        QDir().mkpath(dir);
        g_logPath = dir + "/desktops.log";
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

    app.setOrganizationName(QString());
    // Without this the panel's window has no icon at all (WM_GETICON returns
    // null for both sizes) and the taskbar shows the generic one, whatever the
    // exe's resource says.
    app.setWindowIcon(QIcon(QStringLiteral(":/resources/desktops.ico")));
    installLogging();

    {
        QLocalSocket probe;
        probe.connectToServer(instancePipe());
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

    QLocalServer::removeServer(instancePipe());
    QLocalServer instanceServer;
    if (!instanceServer.listen(instancePipe()))
    {
        qCritical("instance pipe listen failed: %s",
            instanceServer.errorString().toUtf8().constData());
        return -1;
    }

    MainWindow window(sessionTag());
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
