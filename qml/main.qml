import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// CalendarBridge is registered as a QQmlContext property in main.cpp, so
// it's used directly below with no import - unlike the earlier cxx-qt
// version, which needed its own QML module.
//
// NOTE ON THE TRAY: it's plain QSystemTrayIcon in src/traycontroller.cpp,
// connected directly to this window's show()/raise()/requestActivate()
// in main.cpp. No polling flag needed here (compare the git history of
// this file if you're curious what that looked like) - the tray runs on
// the GUI thread, same as this window, so the connection is direct and
// instant.
//
// NOTE ON KIRIGAMI: written against Kirigami's documented API but not
// run through a compiler in the sandbox this was scaffolded in (KF6
// Kirigami isn't packaged for Ubuntu 24.04, only the older Qt5/KF5
// Kirigami2 - see the README). The C++ core (everything under src/) *was*
// compiled and linked for real. If `qmllint`/first run flags a property
// name here, it's most likely a small drift between Kirigami versions.

Kirigami.ApplicationWindow {
    id: root

    title: "Lowacal" // brand name, never translated
    // false when launched at login (--hidden): live in the tray until opened
    visible: !lowacalStartHidden
    width: 1180
    height: 760
    minimumWidth: 760
    minimumHeight: 480

    // --- This is the whole point: closing the window hides it instead of
    // quitting the process, the same way Telegram/WhatsApp desktop clients
    // do. Not "minimize" (which would still leave a taskbar entry / show
    // up in Alt-Tab) - a real hide, with the tray icon as the only way
    // back in until you explicitly choose "Вийти". ---
    onClosing: (close) => {
        close.accepted = false
        root.visible = false
    }

    pageStack.initialPage: Kirigami.Page {
        title: ""
        padding: 0
        // Kirigami.Page normally reserves space for its own header/title
        // bar; we don't want a second title under the OS/compositor one,
        // so it's left empty. Fine-tuning `globalToolBarStyle` here is a
        // one-line adjustment once you see it rendered for real.

        RowLayout {
            anchors.fill: parent
            spacing: 0

            Sidebar {
                Layout.preferredWidth: 236
                Layout.fillHeight: true
            }

            Kirigami.Separator {
                Layout.fillHeight: true
            }

            MonthGrid {
                Layout.fillWidth: true
                Layout.fillHeight: true
            }
        }
    }

    Component.onCompleted: CalendarBridge.syncNow()
}
