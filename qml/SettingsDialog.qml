import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Controls.Popup {
    id: settings
    width: 340
    modal: true
    focus: true
    closePolicy: Controls.Popup.CloseOnEscape | Controls.Popup.CloseOnPressOutside
    anchors.centerIn: Controls.Overlay.overlay

    signal aboutRequested()

    readonly property color hairline: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.12)

    // -1 in the stored value means "disabled"; the UI models that as an
    // unchecked checkbox rather than a magic sentinel the user has to know.
    function load() {
        const a1 = CalendarBridge.alert1Minutes()
        const a2 = CalendarBridge.alert2Minutes()
        alert1Check.checked = a1 >= 0
        alert1Spin.value = a1 >= 0 ? a1 : 10
        alert2Check.checked = a2 >= 0
        alert2Spin.value = a2 >= 0 ? a2 : 60
        const fdow = CalendarBridge.firstDayOfWeek
        weekStartCombo.currentIndex = fdow === 1 ? 1 : (fdow === 0 ? 2 : 0)
        loadedLanguageIndex = Math.max(0, languageCodes.indexOf(CalendarBridge.language))
        languageCombo.currentIndex = loadedLanguageIndex
        autostartCheck.checked = CalendarBridge.autostart
    }

    // Order matches languageCombo's model. Language names are written in
    // their own language on purpose (never translated), so people can
    // find theirs even when the UI is currently in a language they don't read.
    readonly property var languageCodes: ["", "en", "uk"]
    property int loadedLanguageIndex: 0

    function save() {
        CalendarBridge.setReminderMinutes(
            alert1Check.checked ? alert1Spin.value : -1,
            alert2Check.checked ? alert2Spin.value : -1
        )
        CalendarBridge.firstDayOfWeek = [-1, 1, 0][weekStartCombo.currentIndex]
        CalendarBridge.language = languageCodes[languageCombo.currentIndex]
        if (autostartCheck.checked !== CalendarBridge.autostart)
            CalendarBridge.setAutostart(autostartCheck.checked)
        settings.close()
    }

    onOpened: load()

    background: Rectangle {
        color: Kirigami.Theme.backgroundColor
        radius: Kirigami.Units.cornerRadius
        border.color: settings.hairline
        border.width: 1
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Kirigami.Heading {
            level: 4
            text: qsTr("Settings")
        }

        Controls.Label {
            text: qsTr("General")
            font.bold: true
            opacity: 0.8
        }

        RowLayout {
            spacing: Kirigami.Units.smallSpacing
            Controls.Label { text: qsTr("Language:") }
            Controls.ComboBox {
                id: languageCombo
                objectName: "languageCombo"
                Layout.fillWidth: true
                model: [qsTr("System default"), "English", "Українська"]
            }
        }

        Controls.CheckBox {
            id: autostartCheck
            objectName: "autostartCheck"
            Layout.fillWidth: true
            text: qsTr("Start automatically at login (in the tray)")
        }

        Controls.Label {
            visible: languageCombo.currentIndex !== settings.loadedLanguageIndex
            Layout.fillWidth: true
            text: qsTr("Changes apply after you restart the app.")
            wrapMode: Text.Wrap
            font.pixelSize: Kirigami.Theme.smallFont.pixelSize
            opacity: 0.7
        }

        RowLayout {
            spacing: Kirigami.Units.smallSpacing
            Controls.Label { text: qsTr("First day of the week:") }
            Controls.ComboBox {
                id: weekStartCombo
                objectName: "weekStartCombo"
                Layout.fillWidth: true
                model: [qsTr("System default"), qsTr("Monday"), qsTr("Sunday")]
            }
        }

        Controls.Label {
            text: qsTr("Reminders (for all events)")
            font.bold: true
            opacity: 0.8
        }

        RowLayout {
            spacing: Kirigami.Units.smallSpacing
            Controls.CheckBox {
                id: alert1Check
                text: qsTr("Reminder 1:")
            }
            Controls.SpinBox {
                id: alert1Spin
                enabled: alert1Check.checked
                from: 0
                to: 24 * 60
                stepSize: 5
                editable: true
            }
            Controls.Label { text: qsTr("min before") ; opacity: 0.7 }
            Item { Layout.fillWidth: true }
        }

        RowLayout {
            spacing: Kirigami.Units.smallSpacing
            Controls.CheckBox {
                id: alert2Check
                text: qsTr("Reminder 2:")
            }
            Controls.SpinBox {
                id: alert2Spin
                enabled: alert2Check.checked
                from: 0
                to: 24 * 60
                stepSize: 15
                editable: true
            }
            Controls.Label { text: qsTr("min before") ; opacity: 0.7 }
            Item { Layout.fillWidth: true }
        }

        Controls.Label {
            Layout.fillWidth: true
            text: qsTr("A notification arrives the given number of minutes before an event starts. With both reminders on you can, for example, be reminded an hour before and again 10 minutes before.")
            wrapMode: Text.Wrap
            font.pixelSize: Kirigami.Theme.smallFont.pixelSize
            opacity: 0.6
        }

        Kirigami.Separator { Layout.fillWidth: true }

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing
            Controls.Label {
                Layout.fillWidth: true
                text: "Lowacal " + Qt.application.version
                opacity: 0.7
            }
            Controls.Button {
                objectName: "aboutButton"
                flat: true
                text: qsTr("About")
                icon.name: "help-about"
                onClicked: {
                    settings.close()
                    settings.aboutRequested()
                }
            }
        }

        RowLayout {
            Layout.topMargin: Kirigami.Units.smallSpacing
            spacing: Kirigami.Units.smallSpacing
            Controls.Button {
                Layout.fillWidth: true
                text: qsTr("Cancel")
                onClicked: settings.close()
            }
            Controls.Button {
                Layout.fillWidth: true
                highlighted: true
                text: qsTr("Save")
                onClicked: settings.save()
            }
        }
    }
}
