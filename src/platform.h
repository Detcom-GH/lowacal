#pragma once

#include <QObject>
#include <QString>
#include <functional>

// Desktop-integration bits that behave differently inside and outside a
// Flatpak sandbox. Everything here runs on the GUI thread.

namespace Autostart {

// True inside Flatpak (or when LOWACAL_FORCE_PORTAL=1, used by tests to
// exercise the portal path against a fake portal).
bool usesPortal();

// Where the autostart entry lives when NOT sandboxed:
// ~/.config/autostart/<app-id>.desktop
QString desktopFilePath();

// Current state as far as this app can know it. Outside Flatpak that's
// the autostart file itself; inside Flatpak the portal owns the real
// state and can't be queried, so the last confirmed answer is remembered.
bool isEnabled();

// Turns launch-at-login on or off. The app is started with --hidden, so
// it comes up in the tray without opening a window.
//
// Inside Flatpak this asks the Background portal (a sandboxed app cannot
// write ~/.config/autostart itself); some desktops show the user a
// confirmation first, hence the callback. `enabledNow` is the state the
// system actually ended up in.
void setEnabled(bool enabled, const QString &reason, QObject *context,
                std::function<void(bool ok, bool enabledNow, const QString &error)> callback);

} // namespace Autostart

// One running copy at a time. With launch-at-login the app usually sits
// in the tray already, so clicking it in the app menu must bring up that
// copy's window - not start a second process with a second tray icon and
// a second sync loop on the same database.
//
// Mechanism: the first process owns the D-Bus name <app-id> (which a
// Flatpak app may always own). A later process that can't get the name
// asks the owner to show its window, then exits.
class SingleInstance : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "lowacal.SingleInstance")

public:
    explicit SingleInstance(QObject *parent = nullptr);

    // True: this process is the primary instance, carry on starting up.
    // False: another instance is running; if `activateExisting`, it has
    // been asked to show its window. Either way, this process should exit.
    // With no session bus at all there is nothing to coordinate through,
    // so this returns true rather than refusing to start.
    bool claim(bool activateExisting);

public slots:
    Q_SCRIPTABLE void activate();

signals:
    void activateRequested();
};
