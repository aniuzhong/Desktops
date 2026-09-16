#pragma once

#include <windows.h>

#include <QtWidgets/QListWidget>
#include <QtWidgets/QWidget>
#include <QtNetwork/QLocalServer>
#include <QtNetwork/QLocalSocket>
#include <QLoggingCategory>

#include <map>
#include <string>

#include <wil/resource.h>

Q_DECLARE_LOGGING_CATEGORY(lcPanel)

// The Default-desktop panel: the only call site of SwitchDesktop. Never
// blocks: creation and dock death arrive as socket events, so the event
// loop is always free to take the user home.
class MainWindow : public QWidget
{
    Q_OBJECT

public:
    explicit MainWindow(const QString& instanceTag, QWidget* parent = nullptr);

public slots:
    // The go-home semantic: the Default button on a dock, or a second
    // instance's pipe message.
    void goHome();

protected:
    // The panel is only ever visible on Default, and every path back to
    // Default goes through here: rebuilding the list on show is the one
    // moment staleness could be observed.
    void showEvent(QShowEvent* event) override;

private slots:
    void onNew();
    void onSwitchTo();
    void refreshList();

private:
    struct DockEntry
    {
        QLocalServer* server = nullptr;    // owned
        QLocalSocket* socket = nullptr;    // owned, null until the dock connects
        wil::unique_hdesk creationPin;     // held until ready releases it
        bool ready = false;                // handshake received
        bool active = false;               // input desktop is here
    };

    // The health gate: refuses when the dock has not reported ready or
    // its pipe is down.
    bool switchTo(const QString& desktop);

    // The desktop handle moves into the dock entry as its creation pin
    // and is released when the dock reports ready (the dock process's
    // own attachment is the pin from then on).
    void createDesktop(const QString& name);

    // With a creation pin the desktop must already exist; without one
    // this is an adoption of an already-running desktop.
    bool spawnDock(const std::wstring& desktop, wil::unique_hdesk creationPin);

    void onDockConnection(const std::wstring& desktop);
    void onDockSocketReadyRead(const std::wstring& desktop);
    void onDockDisconnected(const std::wstring& desktop);
    void onDockReady(const std::wstring& desktop);

    DockEntry* find(const std::wstring& desktop);
    void dropDock(const std::wstring& desktop);
    QString selectedDesktop() const;

    QString instanceTag_;
    QListWidget* desktopList_;

    std::map<std::wstring, DockEntry> docks_;
};
