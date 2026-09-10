#pragma once

#include <windows.h>

#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QWidget>
#include <QtNetwork/QLocalServer>
#include <QtNetwork/QLocalSocket>
#include <QLoggingCategory>

#include <map>
#include <string>

#include <wil/resource.h>

Q_DECLARE_LOGGING_CATEGORY(lcPanel)

// The Default-desktop panel. Owns one dock process per desktop and is
// the only call site of SwitchDesktop. Never blocks: creation and dock
// death arrive as socket events, so the event loop is always free to
// take the user home.
class MainWindow : public QWidget
{
    Q_OBJECT

public:
    explicit MainWindow(const QString& instanceTag, QWidget* parent = nullptr);

public slots:
    // Move input back to Default + recall the panel. The go-home
    // semantic: the Default button on a dock, or a second instance's
    // pipe message.
    void goHome();

protected:
    // The panel is only ever visible on Default, and every path back to
    // Default goes through here: rebuilding the list on show is the one
    // moment staleness could be observed.
    void showEvent(QShowEvent* event) override;

private slots:
    void onNew();          // create the desktop, spawn its dock
    void onSwitchTo();     // health-gated switch to the selection
    void refreshList();    // align with enumeration, rebuild the list

private:
    struct DockEntry
    {
        QLocalServer* server = nullptr;    // per-dock pipe (owned)
        QLocalSocket* socket = nullptr;    // set on connection (owned)
        wil::unique_hdesk creationPin;     // held until ready releases it
        QByteArray inbox;
        bool ready = false;                // handshake received
        bool parked = true;                // hidden (not the input desktop's dock)
        bool active = false;               // input desktop is here
    };

    // The health gate + the only SwitchDesktop call site. Refuses with
    // a panel message when the target dock is not Parked/Active.
    bool switchTo(const std::wstring& desktop);

    // CreateDesktopW; the desktop handle moves into the dock entry as
    // its creation pin and is released when the dock reports ready (the
    // dock process's own attachment is the pin from then on).
    void createDesktop(const std::wstring& name);

    // Listens on the dock's pipe, launches the dock process on the
    // desktop, and wires the connection handlers. With a creation pin,
    // the desktop must already exist; without one this is an adoption
    // of an already-running desktop.
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
    QPushButton* newButton_;
    QPushButton* switchButton_;

    std::map<std::wstring, DockEntry> docks_;
};
