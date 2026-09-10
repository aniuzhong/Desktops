// Desktops entry point. Two modes of the same exe:
//   Desktops.exe                     - manager: panel + docking authority
//   Desktops.exe --dock <name> <pipe> - per-desktop dock process
#include <windows.h>

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMessageBox>
#include <QMutex>
#include <QStandardPaths>
#include <QString>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include "dock.h"
#include "mainwindow.h"
#include "wilx/win32_helpers.h"

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
        // Session-scoped, like the old Local\ mutex: fast user switching
        // must not make two sessions recall each other.
        return QString("Desktops-instance-%1").arg(sessionTag());
    }

    // The Qt message handler: a rotating log file under
    // %LOCALAPPDATA%/Desktops/logs, shared by both process modes.
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
        QMutexLocker lock(&g_logMutex);
        QFile file(g_logPath);
        if (file.exists() && file.size() > 1024 * 1024)
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

    // A dock dying on a non-Default desktop has no console and leaves a
    // WER report only if Windows feels like archiving one, so the crash
    // has to land in our own log too.
    LONG __stdcall logUnhandledException(EXCEPTION_POINTERS* info)
    {
        const DWORD code = info && info->ExceptionRecord
            ? info->ExceptionRecord->ExceptionCode
            : 0;
        const quintptr at = info && info->ExceptionRecord
            ? reinterpret_cast<quintptr>(info->ExceptionRecord->ExceptionAddress)
            : 0;
        qCritical("unhandled exception 0x%s at 0x%s",
            QString::number(code, 16).toUtf8().constData(),
            QString::number(static_cast<qulonglong>(at), 16).toUtf8().constData());
        return EXCEPTION_EXECUTE_HANDLER;   // WER still gets its say
    }

    void installLogging()
    {
        const QString dir = QStandardPaths::writableLocation(
            QStandardPaths::AppLocalDataLocation) + "/logs";
        QDir().mkpath(dir);
        g_logPath = dir + "/desktops.log";
        qInstallMessageHandler(logHandler);
        ::SetUnhandledExceptionFilter(logUnhandledException);
        std::set_terminate([] {
            qCritical("std::terminate: unhandled C++ exception");
            std::abort();
        });
    }
}  // namespace

int main(int argc, char* argv[])
{
    // Light theme regardless of the system's dark-mode preference.
    qputenv("QT_QPA_PLATFORM", "windows:darkmode=0");

    if (argc >= 4 && std::strcmp(argv[1], "--dock") == 0)
    {
        QApplication app(argc, argv);
        app.setApplicationName("Desktops");
        installLogging();
        const std::wstring threadDesktop = wilx::TryGetThreadDesktopName();
        qInfo("dock mode: desktop='%s' pipe='%s' pid=%lu mainTid=%lu threadDesktop='%s'",
            argv[2], argv[3], ::GetCurrentProcessId(), ::GetCurrentThreadId(),
            QString::fromWCharArray(threadDesktop.c_str()).toUtf8().constData());
        const int dockResult = Dock::run(QString::fromLocal8Bit(argv[2]),
            QString::fromLocal8Bit(argv[3]), argc, argv);
        // Last line before static destruction: an AV after this one is
        // not in Dock::run at all.
        qInfo("Dock::run returned %d", dockResult);
        // End-of-life child: skip QApplication/static teardown, which AVs
        // here in the static-Qt build (observed on every dock exit). The
        // log file is unbuffered, so everything is already on disk.
        ::ExitProcess(static_cast<UINT>(dockResult));
    }

    QApplication app(argc, argv);
    app.setApplicationName("Desktops");
    app.setOrganizationName(QString());
    installLogging();
    {
        const std::wstring threadDesktop = wilx::TryGetThreadDesktopName();
        qInfo("manager starting: pid=%lu mainTid=%lu session=%s threadDesktop='%s' instancePipe='%s'",
            ::GetCurrentProcessId(), ::GetCurrentThreadId(), sessionTag().toUtf8().constData(),
            QString::fromWCharArray(threadDesktop.c_str()).toUtf8().constData(),
            instancePipe().toUtf8().constData());
        for (int i = 0; i < argc; ++i)
            qInfo("manager arg[%d]='%s'", i, argv[i]);
    }

    // Single instance: an existing manager receives "home" and recalls the
    // user; this launch exits.
    {
        QLocalSocket probe;
        probe.connectToServer(instancePipe());
        if (probe.waitForConnected(300))
        {
            probe.write(protocol::Home);
            probe.write("\n");
            probe.flush();
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

    // Second instance = go-home recall.
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
    qInfo("manager exiting with %d", managerResult);
    // Same teardown rationale as the dock branch.
    ::ExitProcess(static_cast<UINT>(managerResult));
}
