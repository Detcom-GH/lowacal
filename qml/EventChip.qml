import QtQuick
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Rectangle {
    id: chip
    property var eventData
    signal clicked(var mouse, var item)

    // Same three demo colours used throughout the mockup/QML: iCloud blue,
    // Google coral, local violet. In the real data these come from
    // `db::CalendarRow.color`, populated from each calendar's own colour
    // during sync - this fallback map only matters before any sync has run.
    readonly property color chipColor: eventData && eventData.color ? eventData.color : "#3DAEE9"

    implicitHeight: label.implicitHeight + 5
    radius: 5
    color: chipColor

    Controls.Label {
        id: label
        anchors.fill: parent
        anchors.leftMargin: 5
        anchors.rightMargin: 5
        anchors.topMargin: 2
        anchors.bottomMargin: 2
        text: (chip.eventData ? formatTime(chip.eventData.startsAt) + "  " + chip.eventData.summary : "")
        color: "white"
        font.pixelSize: Kirigami.Theme.smallFont.pixelSize
        elide: Text.ElideRight
    }

    function formatTime(iso) {
        const d = new Date(iso)
        const hh = d.getHours().toString().padStart(2, "0")
        const mm = d.getMinutes().toString().padStart(2, "0")
        return hh + ":" + mm
    }

    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: (mouse) => chip.clicked(mouse, chip)
    }
}
