import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Rectangle {
    id: sidebar
    color: Kirigami.Theme.backgroundColor

    property var accounts: []
    property var calendars: []
    property string pendingRemoveId: ""
    property string pendingRemoveLabel: ""

    function reload() {
        try {
            accounts = JSON.parse(CalendarBridge.accountsJson())
        } catch (e) {
            accounts = []
        }
        try {
            calendars = JSON.parse(CalendarBridge.calendarsJson())
        } catch (e) {
            calendars = []
        }
    }

    function calendarsFor(accountId) {
        return sidebar.calendars.filter(function(c) { return c.accountId === accountId })
    }

    property string successText: ""
    function showStatus(message) {
        successText = message
        statusTimer.restart()
    }

    Connections {
        target: CalendarBridge
        function onEventsChanged() { sidebar.reload() }
        function onAccountsChanged() { sidebar.reload() }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Kirigami.Units.largeSpacing
        spacing: Kirigami.Units.largeSpacing

        RowLayout {
            spacing: Kirigami.Units.smallSpacing
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                AsciiDachshund {
                    Layout.fillWidth: true // font shrinks to fit the sidebar, tail included
                    pixelSize: 8
                }
                Kirigami.Heading {
                    text: "Lowacal" // brand name, never translated
                    level: 4
                }
            }
            Item { Layout.fillWidth: true }
        }

        Controls.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            ColumnLayout {
                width: sidebar.width - 2 * Kirigami.Units.largeSpacing
                spacing: Kirigami.Units.largeSpacing

                Repeater {
                    model: sidebar.accounts

                    ColumnLayout {
                        id: accountBlock
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 0

                            Controls.Label {
                                Layout.fillWidth: true
                                text: accountBlock.modelData.label.toUpperCase()
                                font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                                font.bold: true
                                font.letterSpacing: 1
                                opacity: 0.6
                                elide: Text.ElideRight
                                Controls.ToolTip.visible: accountHover.hovered
                                Controls.ToolTip.text: accountBlock.modelData.username
                                HoverHandler { id: accountHover }
                            }

                            Controls.ToolButton {
                                icon.name: "overflow-menu"
                                display: Controls.AbstractButton.IconOnly
                                implicitWidth: Kirigami.Units.iconSizes.medium
                                implicitHeight: Kirigami.Units.iconSizes.medium
                                onClicked: accountMenu.popup()
                                Controls.ToolTip.visible: hovered
                                Controls.ToolTip.text: qsTr("Account actions")

                                Controls.Menu {
                                    id: accountMenu
                                    Controls.MenuItem {
                                        text: qsTr("Sync now")
                                        icon.name: "view-refresh"
                                        onTriggered: CalendarBridge.syncNow()
                                    }
                                    Controls.MenuItem {
                                        text: qsTr("Remove account…")
                                        icon.name: "edit-delete"
                                        onTriggered: {
                                            sidebar.pendingRemoveId = accountBlock.modelData.id
                                            sidebar.pendingRemoveLabel = accountBlock.modelData.label
                                            removeDialog.open()
                                        }
                                    }
                                }
                            }
                        }

                        Repeater {
                            model: sidebar.calendarsFor(accountBlock.modelData.id)

                            RowLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: Kirigami.Units.smallSpacing

                                Rectangle {
                                    width: 9; height: 9; radius: 4.5
                                    color: modelData.color
                                    opacity: modelData.visible ? 1.0 : 0.35
                                }

                                Controls.Label {
                                    text: modelData.displayName
                                    Layout.fillWidth: true
                                    opacity: modelData.visible ? 1.0 : 0.5
                                    elide: Text.ElideRight
                                }

                                Controls.CheckBox {
                                    checked: modelData.visible
                                    onToggled: CalendarBridge.setCalendarVisible(modelData.id, checked)
                                }
                            }
                        }

                        // Account saved, first sync still running.
                        Controls.Label {
                            visible: sidebar.calendarsFor(accountBlock.modelData.id).length === 0
                            text: CalendarBridge.syncing ? qsTr("Loading calendars…") : qsTr("No calendars found")
                            opacity: 0.6
                            font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                        }
                    }
                }

                Controls.Button {
                    visible: sidebar.accounts.length > 0
                    Layout.fillWidth: true
                    flat: true
                    text: qsTr("Add account")
                    icon.name: "list-add"
                    onClicked: accountDialog.open()
                }

                Kirigami.PlaceholderMessage {
                    Layout.fillWidth: true
                    visible: sidebar.accounts.length === 0
                    text: qsTr("No calendars yet")
                    explanation: qsTr("Connect iCloud, Nextcloud or another CalDAV server")
                    helpfulAction: Kirigami.Action {
                        text: qsTr("Add calendar")
                        icon.name: "list-add"
                        onTriggered: accountDialog.open()
                    }
                }
            }
        }

        // Errors persist until resolved; success messages fade after a few seconds.
        Controls.Label {
            Layout.fillWidth: true
            visible: text.length > 0
            text: sidebar.successText.length > 0 ? sidebar.successText : CalendarBridge.lastError
            color: sidebar.successText.length > 0 ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.negativeTextColor
            wrapMode: Text.Wrap
            font.pixelSize: Kirigami.Theme.smallFont.pixelSize
        }

        Timer {
            id: statusTimer
            interval: 5000
            onTriggered: sidebar.successText = ""
        }

        Kirigami.Separator { Layout.fillWidth: true }

        RowLayout {
            spacing: Kirigami.Units.smallSpacing

            Rectangle {
                width: 30; height: 30; radius: 15
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: Kirigami.Theme.highlightColor }
                    GradientStop { position: 1.0; color: "#8577D1" }
                }
            }

            Item { Layout.fillWidth: true }

            Controls.BusyIndicator {
                implicitWidth: 18; implicitHeight: 18
                running: CalendarBridge.syncing
                visible: running
            }

            Controls.ToolButton {
                icon.name: "view-refresh"
                display: Controls.AbstractButton.IconOnly
                onClicked: CalendarBridge.syncNow()
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.text: qsTr("Sync now")
            }

            Controls.ToolButton {
                icon.name: "configure"
                display: Controls.AbstractButton.IconOnly
                onClicked: settingsDialog.open()
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.text: qsTr("Settings")
            }
        }
    }

    SettingsDialog {
        id: settingsDialog
        objectName: "settingsDialog"
        onAboutRequested: aboutDialog.open()
    }

    AboutDialog {
        id: aboutDialog
        objectName: "aboutDialog"
    }

    AccountDialog {
        id: accountDialog
        objectName: "accountDialog"
        onSucceeded: (message) => sidebar.showStatus(message)
    }

    Controls.Dialog {
        id: removeDialog
        anchors.centerIn: Controls.Overlay.overlay
        modal: true
        title: qsTr("Remove account “%1”?").arg(sidebar.pendingRemoveLabel)
        standardButtons: Controls.Dialog.Ok | Controls.Dialog.Cancel
        onAccepted: {
            CalendarBridge.removeAccount(sidebar.pendingRemoveId)
            sidebar.pendingRemoveId = ""
        }

        Controls.Label {
            width: 320
            wrapMode: Text.Wrap
            text: qsTr("Its calendars will disappear from this app. Nothing is deleted on the server: everything stays on your iPhone and in other apps.")
        }
    }

    Component.onCompleted: reload()
}
