import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Controls.Popup {
    id: card
    width: 296
    padding: 0
    modal: false
    focus: true
    closePolicy: Controls.Popup.CloseOnEscape | Controls.Popup.CloseOnPressOutside

    property var eventData: null
    signal editRequested(var eventData)

    // Same as MonthGrid.qml: Kirigami has no separatorColor palette role.
    readonly property color hairline: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.12)

    function openFor(data, anchorItem) {
        eventData = data
        // Anchor near the clicked event chip, clamped to stay on-screen -
        // the same "popover near the click, not a centred modal" choice
        // as the HTML mockup. Positioning is computed in the Overlay's
        // coordinate space (the whole window), not the local parent's:
        // inside WeekDayGrid the anchor lives in a scrolled Flickable,
        // where parent-relative math lands the popup off-screen.
        card.parent = Controls.Overlay.overlay
        const overlay = Controls.Overlay.overlay
        const pos = anchorItem.mapToItem(overlay, 0, anchorItem.height + 6)
        const cardHeight = implicitHeight > 0 ? implicitHeight : 240
        let x = pos.x
        let y = pos.y
        if (x + card.width > overlay.width - 16) x = overlay.width - card.width - 16
        if (y + cardHeight > overlay.height - 16) y = pos.y - anchorItem.height - cardHeight - 12
        card.x = Math.max(16, x)
        card.y = Math.max(16, y)
        card.open()
    }

    background: Rectangle {
        color: Kirigami.Theme.backgroundColor
        radius: Kirigami.Units.cornerRadius * 1.4
        border.color: card.hairline
        border.width: 1
        layer.enabled: true
        // subtle drop shadow via a second, blurred-looking rectangle would
        // need Qt5Compat.GraphicalEffects/Qt6's MultiEffect; left as a
        // one-line addition (`import QtQuick.Effects` + `MultiEffect`)
        // once this is running on your machine.
    }

    contentItem: RowLayout {
        spacing: 0

        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 4
            color: card.eventData && card.eventData.color ? card.eventData.color : Kirigami.Theme.highlightColor
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.largeSpacing
            spacing: Kirigami.Units.smallSpacing

            RowLayout {
                Layout.fillWidth: true
                Kirigami.Heading {
                    level: 5
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: card.eventData ? card.eventData.summary : ""
                }
                Controls.ToolButton {
                    icon.name: "window-close"
                    display: Controls.AbstractButton.IconOnly
                    implicitWidth: 22; implicitHeight: 22
                    onClicked: card.close()
                }
            }

            RowLayout {
                spacing: Kirigami.Units.smallSpacing
                Kirigami.Icon { source: "clock"; implicitWidth: 15; implicitHeight: 15 }
                Controls.Label {
                    text: card.eventData ? formatRange(card.eventData.startsAt, card.eventData.endsAt) : ""
                    opacity: 0.75
                    font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                }
            }

            RowLayout {
                spacing: Kirigami.Units.smallSpacing
                Rectangle {
                    width: 8; height: 8; radius: 4
                    color: card.eventData && card.eventData.color ? card.eventData.color : Kirigami.Theme.highlightColor
                }
                Controls.Label {
                    text: card.eventData ? card.eventData.calendarName : ""
                    opacity: 0.75
                    font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                    elide: Text.ElideRight
                }
            }

            RowLayout {
                spacing: Kirigami.Units.smallSpacing
                visible: card.eventData && card.eventData.location
                Kirigami.Icon { source: "mark-location"; implicitWidth: 15; implicitHeight: 15 }
                Controls.Label {
                    text: card.eventData ? (card.eventData.location || "") : ""
                    opacity: 0.75
                    font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }

            Kirigami.Separator {
                Layout.fillWidth: true
                visible: card.eventData && card.eventData.description
            }

            Controls.Label {
                Layout.fillWidth: true
                visible: card.eventData && card.eventData.description
                text: card.eventData ? (card.eventData.description || "") : ""
                wrapMode: Text.Wrap
                font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                opacity: 0.8
            }

            RowLayout {
                Layout.topMargin: Kirigami.Units.smallSpacing
                spacing: Kirigami.Units.smallSpacing
                Controls.Button {
                    Layout.fillWidth: true
                    text: qsTr("Edit")
                    icon.name: "document-edit"
                    onClicked: {
                        card.editRequested(card.eventData)
                        card.close()
                    }
                }
                Controls.Button {
                    icon.name: "edit-delete"
                    display: Controls.AbstractButton.IconOnly
                    onClicked: {
                        CalendarBridge.deleteEvent(card.eventData.calendarId, card.eventData.href)
                        card.close()
                    }
                }
            }
        }
    }

    function formatRange(startIso, endIso) {
        const start = new Date(startIso)
        const end = new Date(endIso)
        const pad = (n) => n.toString().padStart(2, "0")
        const t = (d) => pad(d.getHours()) + ":" + pad(d.getMinutes())
        return t(start) + "\u2013" + t(end) + ", " + start.toLocaleDateString(Qt.locale(), "d MMMM")
    }
}
