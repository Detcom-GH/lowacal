import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Controls.Popup {
    id: editor
    width: 320
    modal: true
    focus: true
    closePolicy: Controls.Popup.CloseOnEscape

    property var calendars: []
    // When set, the form is editing this existing event (href/uid kept)
    // rather than creating a new one - drives both the title and which
    // bridge call submit() makes.
    property var editingEvent: null

    readonly property color hairline: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.12)

    // Opens the form pre-filled for a given day - defaults to 10:00,
    // 1 hour long, since guessing a "reasonable" time beats leaving it
    // blank and it's just a starting point the user overtypes anyway.
    function openFor(targetDate) {
        editingEvent = null
        try {
            calendars = JSON.parse(CalendarBridge.calendarsJson()).filter(c => c.visible)
        } catch (e) {
            calendars = []
        }
        titleField.text = ""
        dateField.text = Qt.formatDate(targetDate, "yyyy-MM-dd")
        timeField.text = "10:00"
        durationSpin.value = 60
        locationField.text = ""
        descArea.text = ""
        calendarCombo.currentIndex = 0
        errorLabel.text = ""
        editor.open()
        titleField.forceActiveFocus()
    }

    function openForEdit(eventData) {
        editingEvent = eventData
        try {
            calendars = JSON.parse(CalendarBridge.calendarsJson()).filter(c => c.visible)
        } catch (e) {
            calendars = []
        }
        const start = new Date(eventData.startsAt)
        const end = new Date(eventData.endsAt)
        titleField.text = eventData.summary
        dateField.text = Qt.formatDate(start, "yyyy-MM-dd")
        timeField.text = Qt.formatTime(start, "HH:mm")
        durationSpin.value = Math.max(5, Math.round((end.getTime() - start.getTime()) / 60000))
        locationField.text = eventData.location || ""
        descArea.text = eventData.description || ""
        // Lock the calendar picker to the event's own calendar - moving an
        // event between calendars is a delete+create on the CalDAV level,
        // deliberately out of scope for this form.
        for (let i = 0; i < calendars.length; i++) {
            if (calendars[i].id === eventData.calendarId) {
                calendarCombo.currentIndex = i
                break
            }
        }
        errorLabel.text = ""
        editor.open()
        titleField.forceActiveFocus()
    }

    background: Rectangle {
        color: Kirigami.Theme.backgroundColor
        radius: Kirigami.Units.cornerRadius
        border.color: editor.hairline
        border.width: 1
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        Kirigami.Heading {
            level: 5
            text: editor.editingEvent ? qsTr("Edit event") : qsTr("New event")
        }

        Controls.TextField {
            id: titleField
            Layout.fillWidth: true
            placeholderText: qsTr("Title")
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            Controls.TextField {
                id: dateField
                Layout.fillWidth: true
                placeholderText: qsTr("YYYY-MM-DD")
                validator: RegularExpressionValidator { regularExpression: /\d{4}-\d{2}-\d{2}/ }
            }
            Controls.TextField {
                id: timeField
                Layout.preferredWidth: 70
                placeholderText: qsTr("HH:MM")
                validator: RegularExpressionValidator { regularExpression: /\d{1,2}:\d{2}/ }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Controls.Label {
                text: qsTr("Duration, min:")
                Layout.fillWidth: true
            }
            Controls.SpinBox {
                id: durationSpin
                from: 5
                to: 24 * 60
                stepSize: 15
                value: 60
                editable: true
            }
        }

        Controls.ComboBox {
            id: calendarCombo
            Layout.fillWidth: true
            model: editor.calendars
            textRole: "displayName"
            valueRole: "id"
            // Moving an event between calendars is delete+create at the
            // CalDAV level - out of scope for the edit form, so lock it.
            enabled: !editor.editingEvent
        }

        Controls.TextField {
            id: locationField
            Layout.fillWidth: true
            placeholderText: qsTr("Location (optional)")
        }

        Controls.TextArea {
            id: descArea
            Layout.fillWidth: true
            Layout.preferredHeight: 56
            placeholderText: qsTr("Notes (optional)")
            wrapMode: Text.Wrap
        }

        Controls.Label {
            id: emptyCalendarsNotice
            Layout.fillWidth: true
            visible: editor.calendars.length === 0
            text: qsTr("No calendars available yet. Add an account first.")
            color: Kirigami.Theme.negativeTextColor
            wrapMode: Text.Wrap
            font.pixelSize: Kirigami.Theme.smallFont.pixelSize
        }

        Controls.Label {
            id: errorLabel
            Layout.fillWidth: true
            visible: text.length > 0
            color: Kirigami.Theme.negativeTextColor
            wrapMode: Text.Wrap
            font.pixelSize: Kirigami.Theme.smallFont.pixelSize
        }

        RowLayout {
            Layout.topMargin: Kirigami.Units.smallSpacing
            spacing: Kirigami.Units.smallSpacing

            Controls.Button {
                Layout.fillWidth: true
                text: qsTr("Cancel")
                onClicked: editor.close()
            }
            Controls.Button {
                Layout.fillWidth: true
                highlighted: true
                text: editor.editingEvent ? qsTr("Save") : qsTr("Create")
                enabled: titleField.text.trim().length > 0 && editor.calendars.length > 0
                onClicked: editor.submit()
            }
        }
    }

    function submit() {
        const dateParts = dateField.text.split("-").map(Number)
        const timeParts = timeField.text.split(":").map(Number)
        if (dateParts.length !== 3 || timeParts.length !== 2 || dateParts.some(isNaN) || timeParts.some(isNaN)) {
            errorLabel.text = qsTr("Check the date and time format.")
            return
        }
        const start = new Date(dateParts[0], dateParts[1] - 1, dateParts[2], timeParts[0], timeParts[1])
        if (isNaN(start.getTime())) {
            errorLabel.text = qsTr("Invalid date.")
            return
        }
        const end = new Date(start.getTime() + durationSpin.value * 60000)

        // Fire-and-forget: this queues the request onto SyncWorker's
        // thread and returns immediately. If the upload itself later
        // fails (bad credentials, offline...), that surfaces through
        // CalendarBridge.lastError, same as a failed background sync -
        // not here, since we don't know the outcome yet at this point.
        if (editor.editingEvent) {
            CalendarBridge.updateEvent(
                editor.editingEvent.calendarId,
                editor.editingEvent.href,
                editor.editingEvent.uid,
                titleField.text.trim(),
                start.toISOString(),
                end.toISOString(),
                locationField.text.trim(),
                descArea.text.trim()
            )
        } else {
            CalendarBridge.createEvent(
                calendarCombo.currentValue,
                titleField.text.trim(),
                start.toISOString(),
                end.toISOString(),
                locationField.text.trim(),
                descArea.text.trim()
            )
        }
        editor.close()
    }
}
