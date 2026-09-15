#include "mainwindow.h"

#include <windows.h>

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMessageBox>
#include <QString>
#include <QVBoxLayout>

#include <algorithm>
#include <string>
#include <vector>

#include <wil/resource.h>

#include "dock.h"
#include "wilx/desktop.h"

Q_LOGGING_CATEGORY(lcPanel, "desktops.panel")

namespace
{
    const QString kDefaultDesktop = QStringLiteral("Default");

    // Desktops that are never ours to manage.
    const QStringList kReservedDesktops = {kDefaultDesktop, QStringLiteral("Winlogon"),
        QStringLiteral("Disconnect")};

    void send(QLocalSocket* socket, const char* token)
    {
        socket->write(token);
        socket->write("\n");
    }

    QStringList listExtraDesktops()
    {
        QStringList names;
        wilx::for_each_desktop_nothrow([&](PCWSTR rawName) {
            const QString name = QString::fromWCharArray(rawName);
            if (kReservedDesktops.contains(name, Qt::CaseInsensitive))
                return true;
            // Probe before listing: a name that cannot be opened is on
            // its way out (or belongs to someone else's sandbox).
            wil::unique_hdesk probe(::OpenDesktopW(reinterpret_cast<LPCWSTR>(name.utf16()), 0,
                FALSE, DESKTOP_SWITCHDESKTOP | DESKTOP_READOBJECTS));
            if (probe)
                names.append(name);
            return true;
        });
        names.sort(Qt::CaseInsensitive);
        return names;
    }

    bool switchInputTo(const QString& name)
    {
        const std::wstring wide = name.toStdWString();
        qCInfo(lcPanel, "moving input desktop to '%s'", name.toUtf8().constData());
        wil::unique_hdesk desktop(
            ::OpenDesktopW(wide.c_str(), 0, FALSE, DESKTOP_SWITCHDESKTOP));
        if (!desktop)
        {
            const DWORD error = ::GetLastError();
            qCWarning(lcPanel, "OpenDesktopW('%s') failed (%lu)",
                name.toUtf8().constData(), error);
            return false;
        }
        if (!::SwitchDesktop(desktop.get()))
        {
            const DWORD error = ::GetLastError();
            qCWarning(lcPanel, "SwitchDesktop('%s') failed (%lu)",
                name.toUtf8().constData(), error);
            return false;
        }
        return true;
    }

    bool isValidDesktopName(const QString& name)
    {
        if (name.trimmed().isEmpty())
            return false;
        // Characters that would break pipe names or command lines.
        static const QString forbidden = QStringLiteral("\\/\":*?<>|");
        for (const QChar ch : name)
        {
            if (ch.unicode() == 0 || forbidden.contains(ch))
                return false;
        }
        return true;
    }
}

MainWindow::MainWindow(const QString& instanceTag, QWidget* parent)
    : QWidget(parent)
    , instanceTag_(instanceTag)
{
    setWindowTitle("Desktops");
    setFixedSize(320, 240);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);

    desktopList_ = new QListWidget(this);
    layout->addWidget(desktopList_, 1);

    auto* row = new QHBoxLayout;
    newButton_ = new QPushButton("&New", this);
    switchButton_ = new QPushButton("&Switch To", this);
    row->addWidget(newButton_);
    row->addWidget(switchButton_);
    row->addStretch(1);
    layout->addLayout(row);

    connect(newButton_, &QPushButton::clicked, this, &MainWindow::onNew);
    connect(switchButton_, &QPushButton::clicked, this, &MainWindow::onSwitchTo);
    connect(desktopList_, &QListWidget::itemDoubleClicked,
        this, [this](QListWidgetItem*) { onSwitchTo(); });

    refreshList();
}

void MainWindow::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refreshList();   // the list is only ever seen right after this
}

void MainWindow::goHome()
{
    if (!switchInputTo(kDefaultDesktop))
    {
        qCWarning(lcPanel, "goHome: switch to Default failed");
        return;
    }
    for (auto& [name, entry] : docks_)
    {
        if (entry.active && entry.socket && entry.socket->state() == QLocalSocket::ConnectedState)
        {
            send(entry.socket, protocol::Park);
            entry.active = false;
            entry.parked = true;
        }
    }
    show();
    raise();
    activateWindow();
}

void MainWindow::onNew()
{
    bool accepted = false;
    const QString name = QInputDialog::getText(this, "New Desktop", "Desktop name:",
        QLineEdit::Normal, QString(), &accepted).trimmed();
    if (!accepted || name.isEmpty())
        return;
    // Desktop names are case-insensitive in Win32: creating "probecase" when
    // "ProbeCase" exists reopens the same desktop instead of failing, so the
    // duplicate check must be too - a case-sensitive lookup here would give one
    // desktop two docks (two bars, two wallpapers).
    if (!isValidDesktopName(name)
        || kReservedDesktops.contains(name, Qt::CaseInsensitive)
        || listExtraDesktops().contains(name, Qt::CaseInsensitive))
    {
        QMessageBox::warning(this, "New Desktop",
            QString("A desktop named '%1' cannot be used.").arg(name));
        return;
    }
    createDesktop(name);
}

void MainWindow::onSwitchTo()
{
    const QString name = selectedDesktop();
    if (!name.isEmpty())
        switchTo(name);
}

bool MainWindow::switchTo(const QString& desktop)
{
    if (0 == desktop.compare(kDefaultDesktop, Qt::CaseInsensitive))
    {
        goHome();
        return true;
    }
    DockEntry* entry = find(desktop.toStdWString());
    // The health gate: exactly one precondition, read from the state the
    // dock itself reported. No probing, no waiting.
    if (!entry || !entry->ready
        || (!entry->socket || entry->socket->state() != QLocalSocket::ConnectedState))
    {
        QMessageBox::warning(this, "Desktops",
            QString("Could not attach to desktop '%1': its dock is not ready.")
                .arg(desktop));
        return false;
    }
    if (!switchInputTo(desktop))
    {
        QMessageBox::warning(this, "Desktops",
            QString("Switching to '%1' did not take effect.").arg(desktop));
        return false;
    }
    send(entry->socket, protocol::Activate);
    entry->active = true;
    entry->parked = false;
    return true;
}

void MainWindow::createDesktop(const QString& name)
{
    qCInfo(lcPanel, "creating desktop '%s'", name.toUtf8().constData());
    const std::wstring wide = name.toStdWString();
    wil::unique_hdesk created(
        ::CreateDesktopW(wide.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr));
    if (!created)
    {
        const DWORD error = ::GetLastError();
        qCWarning(lcPanel, "CreateDesktopW('%s') failed (%lu)",
            name.toUtf8().constData(), error);
        QMessageBox::warning(this, "Desktops",
            QString("Could not create desktop '%1' (error %2).").arg(name).arg(error));
        return;
    }
    spawnDock(wide, std::move(created));
}

bool MainWindow::spawnDock(const std::wstring& desktop, wil::unique_hdesk creationPin)
{
    const QString pipe = QString("%1-dock-%2-%3")
        .arg(instanceTag_, QString::number(::GetCurrentProcessId()),
            QString::fromStdWString(desktop));

    auto entry = DockEntry{};
    entry.server = new QLocalServer(this);
    QLocalServer::removeServer(pipe);
    if (!entry.server->listen(pipe))
    {
        qCWarning(lcPanel, "listen('%s') failed: %s", pipe.toUtf8().constData(),
            entry.server->errorString().toUtf8().constData());
        delete entry.server;
        QMessageBox::warning(this, "Desktops",
            QString("Could not start the dock for desktop '%1'.")
                .arg(QString::fromStdWString(desktop)));
        return false;
    }
    entry.creationPin = std::move(creationPin);
    auto [it, inserted] = docks_.emplace(desktop, std::move(entry));
    if (!inserted)
    {
        qCWarning(lcPanel, "dock for '%s' already exists",
            QString::fromStdWString(desktop).toUtf8().constData());
        delete it->second.server;
        return false;
    }

    QObject::connect(it->second.server, &QLocalServer::newConnection, this,
        [this, desktop] { onDockConnection(desktop); });

    wchar_t self[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    // writable: CreateProcessW may rewrite the command line
    std::wstring command = L"\"" + std::wstring(self) + L"\" --dock " +
        desktop + L" \"" + pipe.toStdWString() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.lpDesktop = const_cast<LPWSTR>(desktop.c_str());
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        const DWORD error = ::GetLastError();
        qCWarning(lcPanel, "dock launch failed (%lu)", error);
        dropDock(desktop);
        QMessageBox::warning(this, "Desktops",
            QString("Could not start the dock for desktop '%1' (error %2).")
                .arg(QString::fromStdWString(desktop))
                .arg(error));
        return false;
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    // The dock process itself is the desktop pin once attached; the
    // creation pin is released on ready. Until then the desktop cannot
    // be switched into (the health gate refuses).
    qCInfo(lcPanel, "dock spawned for '%s'", QString::fromStdWString(desktop).toUtf8().constData());
    refreshList();
    return true;
}

void MainWindow::onDockConnection(const std::wstring& desktop)
{
    DockEntry* entry = find(desktop);
    if (!entry || !entry->server || !entry->server->hasPendingConnections())
        return;
    entry->socket = entry->server->nextPendingConnection();
    entry->socket->setParent(this);
    connect(entry->socket, &QLocalSocket::readyRead, this,
        [this, desktop] { onDockSocketReadyRead(desktop); });
    connect(entry->socket, &QLocalSocket::disconnected, this,
        [this, desktop] { onDockDisconnected(desktop); });
}

void MainWindow::onDockSocketReadyRead(const std::wstring& desktop)
{
    DockEntry* entry = find(desktop);
    if (!entry || !entry->socket)
        return;
    while (entry->socket->canReadLine())
    {
        const QByteArray line = entry->socket->readLine().trimmed();
        if (line == protocol::Ready)
            onDockReady(desktop);
        else if (line == protocol::Home)
            goHome();
    }
}

void MainWindow::onDockReady(const std::wstring& desktop)
{
    DockEntry* entry = find(desktop);
    if (!entry)
        return;
    entry->ready = true;
    entry->creationPin.reset();   // the dock process is the pin now
    qCInfo(lcPanel, "dock for '%s' is ready", QString::fromStdWString(desktop).toUtf8().constData());
    refreshList();
}

void MainWindow::onDockDisconnected(const std::wstring& desktop)
{
    DockEntry* entry = find(desktop);
    const bool wasReady = entry && entry->ready;
    qCWarning(lcPanel, "dock for '%s' exited (was ready: %d)",
        QString::fromStdWString(desktop).toUtf8().constData(), wasReady ? 1 : 0);
    dropDock(desktop);
    refreshList();
    if (wasReady)
        QMessageBox::warning(this, "Desktops",
            QString("The dock for desktop '%1' exited; the desktop is gone.")
                .arg(QString::fromStdWString(desktop)));
}

void MainWindow::refreshList()
{
    const QStringList extras = listExtraDesktops();

    // dropDock erases from docks_, so the drop list is collected first.
    std::vector<std::wstring> toDrop;
    for (auto& [name, entry] : docks_)
    {
        if (!extras.contains(QString::fromStdWString(name), Qt::CaseInsensitive))
            toDrop.push_back(name);
    }
    for (const std::wstring& name : toDrop)
        dropDock(name);
    for (const QString& extra : extras)
        if (!find(extra.toStdWString()))
            spawnDock(extra.toStdWString(), nullptr);

    desktopList_->clear();
    desktopList_->addItem(kDefaultDesktop);
    desktopList_->addItems(extras);
}

MainWindow::DockEntry* MainWindow::find(const std::wstring& desktop)
{
    const auto it = docks_.find(desktop);
    return it == docks_.end() ? nullptr : &it->second;
}

void MainWindow::dropDock(const std::wstring& desktop)
{
    const auto it = docks_.find(desktop);
    if (it == docks_.end())
        return;
    auto& entry = it->second;
    if (entry.socket && entry.socket->state() == QLocalSocket::ConnectedState)
    {
        send(entry.socket, protocol::Exit);
        entry.socket->flush();
    }
    if (entry.socket)
        entry.socket->deleteLater();
    if (entry.server)
        entry.server->deleteLater();
    docks_.erase(it);
}

QString MainWindow::selectedDesktop() const
{
    return desktopList_->currentItem() ? desktopList_->currentItem()->text() : QString();
}
