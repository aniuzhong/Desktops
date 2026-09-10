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
#include "wilx/desktops.h"
#include "wilx/desktop_windows.h"

Q_LOGGING_CATEGORY(lcPanel, "desktops.panel")

namespace
{
    inline constexpr wchar_t kDefaultDesktop[] = L"Default";

    QString q(const std::wstring& text)
    {
        return QString::fromWCharArray(text.c_str(), static_cast<qsizetype>(text.size()));
    }

    std::wstring stdW(const QString& text)
    {
        return std::wstring(reinterpret_cast<const wchar_t*>(text.utf16()),
            static_cast<size_t>(text.size()));
    }

    // Desktops of the process's window station minus Default/Winlogon/
    // Disconnect, sorted case-insensitively. A desktop is listed only
    // when it opens with DESKTOP_SWITCHDESKTOP | DESKTOP_READOBJECTS.
    std::vector<std::wstring> listExtraDesktops()
    {
        std::vector<std::wstring> names;
        wilx::for_each_desktop_nothrow([&](PCWSTR rawName) {
            std::wstring name(rawName);
            if (_wcsicmp(name.c_str(), kDefaultDesktop) == 0
                || _wcsicmp(name.c_str(), L"Winlogon") == 0
                || _wcsicmp(name.c_str(), L"Disconnect") == 0)
                return true;
            // Probe before listing: a name that cannot be opened is on
            // its way out (or belongs to someone else's sandbox).
            wil::unique_hdesk probe(::OpenDesktopW(name.c_str(), 0, FALSE,
                DESKTOP_SWITCHDESKTOP | DESKTOP_READOBJECTS));
            if (probe)
                names.push_back(std::move(name));
            return true;
        });
        std::sort(names.begin(), names.end(),
            [](const std::wstring& a, const std::wstring& b) {
                return _wcsicmp(a.c_str(), b.c_str()) < 0;
            });
        return names;
    }

    // OpenDesktopW(DESKTOP_SWITCHDESKTOP) + SwitchDesktop.
    bool switchInputTo(const std::wstring& name)
    {
        qCInfo(lcPanel, "moving input desktop to '%s'", q(name).toUtf8().constData());
        wil::unique_hdesk desktop(
            ::OpenDesktopW(name.c_str(), 0, FALSE, DESKTOP_SWITCHDESKTOP));
        if (!desktop)
        {
            const DWORD error = ::GetLastError();
            qCWarning(lcPanel, "OpenDesktopW('%s') failed (%lu)",
                q(name).toUtf8().constData(), error);
            return false;
        }
        if (!::SwitchDesktop(desktop.get()))
        {
            const DWORD error = ::GetLastError();
            qCWarning(lcPanel, "SwitchDesktop('%s') failed (%lu)",
                q(name).toUtf8().constData(), error);
            return false;
        }
        return true;
    }

    bool isValidDesktopName(const QString& name)
    {
        if (name.trimmed().isEmpty())
            return false;
        // Characters that would break pipe names or command lines.
        static const std::wstring forbidden = L"\\/\":*?<>|";
        for (const QChar ch : name)
        {
            const wchar_t wide = static_cast<wchar_t>(ch.unicode());
            if (wide == 0 || forbidden.find(wide) != std::wstring::npos)
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
            entry.socket->write(protocol::Park);
            entry.socket->write("\n");
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
    if (!isValidDesktopName(name)
        || _wcsicmp(stdW(name).c_str(), kDefaultDesktop) == 0
        || docks_.find(stdW(name)) != docks_.end())
    {
        QMessageBox::warning(this, "New Desktop",
            QString("A desktop named '%1' cannot be used.").arg(name));
        return;
    }
    createDesktop(stdW(name));
}

void MainWindow::onSwitchTo()
{
    const QString name = selectedDesktop();
    if (!name.isEmpty())
        switchTo(stdW(name));
}

bool MainWindow::switchTo(const std::wstring& desktop)
{
    if (_wcsicmp(desktop.c_str(), kDefaultDesktop) == 0)
    {
        goHome();
        return true;
    }
    DockEntry* entry = find(desktop);
    // The health gate: exactly one precondition, read from the state the
    // dock itself reported. No probing, no waiting.
    if (!entry || !entry->ready
        || (!entry->socket || entry->socket->state() != QLocalSocket::ConnectedState))
    {
        QMessageBox::warning(this, "Desktops",
            QString("Could not attach to desktop '%1': its dock is not ready.")
                .arg(q(desktop)));
        return false;
    }
    if (!switchInputTo(desktop))
    {
        QMessageBox::warning(this, "Desktops",
            QString("Switching to '%1' did not take effect.").arg(q(desktop)));
        return false;
    }
    entry->socket->write(protocol::Activate);
    entry->socket->write("\n");
    entry->active = true;
    entry->parked = false;
    return true;
}

void MainWindow::createDesktop(const std::wstring& name)
{
    qCInfo(lcPanel, "creating desktop '%s'", q(name).toUtf8().constData());
    wil::unique_hdesk created(
        ::CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr));
    if (!created)
    {
        const DWORD error = ::GetLastError();
        qCWarning(lcPanel, "CreateDesktopW('%s') failed (%lu)",
            q(name).toUtf8().constData(), error);
        QMessageBox::warning(this, "Desktops",
            QString("Could not create desktop '%1' (error %2).").arg(q(name)).arg(error));
        return;
    }
    spawnDock(name, std::move(created));
}

bool MainWindow::spawnDock(const std::wstring& desktop, wil::unique_hdesk creationPin)
{
    const QString pipe = QString("%1-dock-%2-%3")
        .arg(instanceTag_, QString::number(::GetCurrentProcessId()), q(desktop));

    auto entry = DockEntry{};
    entry.server = new QLocalServer(this);
    QLocalServer::removeServer(pipe);
    if (!entry.server->listen(pipe))
    {
        qCWarning(lcPanel, "listen('%s') failed: %s", pipe.toUtf8().constData(),
            entry.server->errorString().toUtf8().constData());
        delete entry.server;
        QMessageBox::warning(this, "Desktops",
            QString("Could not start the dock for desktop '%1'.").arg(q(desktop)));
        return false;
    }
    entry.creationPin = std::move(creationPin);
    auto [it, inserted] = docks_.emplace(desktop, std::move(entry));
    if (!inserted)
    {
        qCWarning(lcPanel, "dock for '%s' already exists", q(desktop).toUtf8().constData());
        delete it->second.server;
        return false;
    }

    QObject::connect(it->second.server, &QLocalServer::newConnection, this,
        [this, desktop] { onDockConnection(desktop); });

    wchar_t self[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    // writable: CreateProcessW may rewrite the command line
    std::wstring command = L"\"" + std::wstring(self) + L"\" --dock " +
        desktop + L" \"" + stdW(pipe) + L"\"";
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
                .arg(q(desktop))
                .arg(error));
        return false;
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    // The dock process itself is the desktop pin once attached; the
    // creation pin is released on ready. Until then the desktop cannot
    // be switched into (the health gate refuses).
    qCInfo(lcPanel, "dock spawned for '%s'", q(desktop).toUtf8().constData());
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
    entry->inbox += entry->socket->readAll();
    for (int newline = entry->inbox.indexOf('\n'); newline >= 0;
         newline = entry->inbox.indexOf('\n'))
    {
        const QByteArray line = entry->inbox.left(newline);
        entry->inbox.remove(0, newline + 1);
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
    qCInfo(lcPanel, "dock for '%s' is ready", q(desktop).toUtf8().constData());
    refreshList();
}

void MainWindow::onDockDisconnected(const std::wstring& desktop)
{
    DockEntry* entry = find(desktop);
    const bool wasReady = entry && entry->ready;
    qCWarning(lcPanel, "dock for '%s' exited (was ready: %d)",
        q(desktop).toUtf8().constData(), wasReady ? 1 : 0);
    dropDock(desktop);
    refreshList();
    if (wasReady)
        QMessageBox::warning(this, "Desktops",
            QString("The dock for desktop '%1' exited; the desktop is gone.")
                .arg(q(desktop)));
}

void MainWindow::refreshList()
{
    const std::vector<std::wstring> extras = listExtraDesktops();

    // Align: drop registry entries whose desktop no longer exists, adopt
    // extras that have no dock yet. dropDock erases from docks_, so the
    // drop list is collected first.
    std::vector<std::wstring> toDrop;
    for (auto& [name, entry] : docks_)
    {
        const bool exists = std::find_if(extras.begin(), extras.end(),
            [&](const std::wstring& candidate) {
                return _wcsicmp(candidate.c_str(), name.c_str()) == 0;
            }) != extras.end();
        if (!exists)
            toDrop.push_back(name);
    }
    for (const std::wstring& name : toDrop)
        dropDock(name);
    for (const std::wstring& extra : extras)
        if (!find(extra))
            spawnDock(extra, nullptr);

    desktopList_->clear();
    desktopList_->addItem(QString::fromWCharArray(kDefaultDesktop));
    for (const std::wstring& extra : extras)
        desktopList_->addItem(QString::fromWCharArray(extra.c_str()));
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
        entry.socket->write(protocol::Exit);
        entry.socket->write("\n");
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
