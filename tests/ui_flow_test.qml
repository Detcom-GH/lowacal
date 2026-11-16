import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../qml"

// Mirrors qml/main.qml's layout: the sidebar at its real 236 px width.
// (An earlier version stretched it over the whole window, which hid a bug
// where dialogs declared inside the sidebar took the sidebar's width.)
ApplicationWindow {
    width: 1000
    height: 760
    visible: true

    RowLayout {
        anchors.fill: parent
        spacing: 0
        Sidebar {
            objectName: "sidebar"
            Layout.preferredWidth: 236
            Layout.fillHeight: true
        }
        Item { Layout.fillWidth: true; Layout.fillHeight: true }
    }
}
