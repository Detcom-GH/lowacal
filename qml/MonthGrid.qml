import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Rectangle {
    id: monthGrid
    color: Kirigami.Theme.backgroundColor

    // Kirigami's palette has no "separatorColor" role (separators are
    // drawn via frameContrast or the Separator component instead) - this
    // is what a border.color needs, so it's a hand-rolled low-opacity
    // version of textColor instead, same trick already used for the
    // "today" gradient below.
    readonly property color hairline: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.12)

    property date viewDate: new Date()
    property var events: []
    property int maxVisibleEvents: 5
    // 0 = День, 1 = Тиждень, 2 = Місяць - drives both which grid is shown
    // below and how the toolbar's title/nav arrows behave.
    property int viewMode: 2


    // Qt.locale().firstDayOfWeek and dayName() are locale-aware (e.g.
    // Monday-first with "Пн, Вт, Ср..." for uk_UA, Sunday-first with
    // "Sun, Mon..." for en_US) - deliberately not hardcoded to either,
    // so this follows whatever the system is actually set to.
    // User override from Settings (-1 = follow the system locale).
    readonly property int firstDayOfWeek: CalendarBridge.firstDayOfWeek >= 0 ? CalendarBridge.firstDayOfWeek : Qt.locale().firstDayOfWeek
    readonly property var dayHeaders: {
        const names = []
        for (let i = 0; i < 7; i++) {
            const day = (firstDayOfWeek + i) % 7
            names.push(Qt.locale().dayName(day, Locale.ShortFormat))
        }
        return names
    }

    function isSameDay(a, b) {
        return a.getFullYear() === b.getFullYear()
            && a.getMonth() === b.getMonth()
            && a.getDate() === b.getDate()
    }

    // Week bounds for the title in Тиждень mode - same "locale's first
    // day of week" logic as WeekDayGrid.qml's periodDays, kept separate
    // and self-contained here rather than reaching into that component.
    readonly property var weekBounds: {
        const dow = viewDate.getDay()
        const diff = (dow - firstDayOfWeek + 7) % 7
        const start = new Date(viewDate.getFullYear(), viewDate.getMonth(), viewDate.getDate() - diff)
        const end = new Date(start.getFullYear(), start.getMonth(), start.getDate() + 6)
        return { start: start, end: end }
    }

    // Month names come from the locale, not a hand-written list: the
    // locale knows grammatical case - Ukrainian needs "Липень 2026" as a
    // heading (standalone/nominative) but "17 липня 2026" inside a date
    // (genitive), English needs "July" in both.
    function capitalized(text) {
        return text.length > 0 ? text.charAt(0).toLocaleUpperCase() + text.slice(1) : text
    }

    readonly property string periodTitle: {
        const loc = Qt.locale()
        if (viewMode === 0)
            return viewDate.toLocaleDateString(loc, "d MMMM yyyy")
        if (viewMode === 1) {
            const s = weekBounds.start, e = weekBounds.end
            if (s.getMonth() === e.getMonth())
                return s.getDate() + "\u2013" + e.toLocaleDateString(loc, "d MMMM yyyy")
            if (s.getFullYear() === e.getFullYear())
                return s.toLocaleDateString(loc, "d MMMM") + " \u2013 " + e.toLocaleDateString(loc, "d MMMM yyyy")
            return s.toLocaleDateString(loc, "d MMMM yyyy") + " \u2013 " + e.toLocaleDateString(loc, "d MMMM yyyy")
        }
        return capitalized(loc.standaloneMonthName(viewDate.getMonth(), Locale.LongFormat)) + " " + viewDate.getFullYear()
    }

    function stepPeriod(direction) {
        if (viewMode === 0) {
            viewDate = new Date(viewDate.getFullYear(), viewDate.getMonth(), viewDate.getDate() + direction)
        } else if (viewMode === 1) {
            viewDate = new Date(viewDate.getFullYear(), viewDate.getMonth(), viewDate.getDate() + direction * 7)
        } else {
            viewDate = new Date(viewDate.getFullYear(), viewDate.getMonth() + direction, 1)
        }
    }

    // Builds a flat list of cells covering the visible month, padded with
    // the trailing days of the previous month and leading days of the
    // next so the grid always starts on a Sunday - a whole number of
    // weeks, 5 or 6 rows depending on the month, unlike a fixed 5-row
    // grid which silently clips months that need a 6th row.
    function buildCells() {
        const year = viewDate.getFullYear()
        const month = viewDate.getMonth()
        const firstOfMonth = new Date(year, month, 1)
        // Date.getDay() is always 0=Sunday per the JS spec, regardless of
        // locale - re-based here against firstDayOfWeek so the grid
        // actually starts on the locale's first day, not always Sunday.
        const jsWeekday = firstOfMonth.getDay()
        const startOffset = (jsWeekday - firstDayOfWeek + 7) % 7
        const daysInMonth = new Date(year, month + 1, 0).getDate()
        const totalCells = Math.ceil((startOffset + daysInMonth) / 7) * 7

        const cells = []
        for (let i = 0; i < totalCells; i++) {
            const date = new Date(year, month, 1 - startOffset + i)
            cells.push({ date: date, outside: date.getMonth() !== month })
        }
        return cells
    }

    property var cells: buildCells()
    onViewDateChanged: cells = buildCells()

    function reloadEvents() {
        const first = cells.length > 0 ? cells[0].date : viewDate
        const last = cells.length > 0 ? cells[cells.length - 1].date : viewDate
        const fromIso = new Date(first.getFullYear(), first.getMonth(), first.getDate()).toISOString()
        const toIso = new Date(last.getFullYear(), last.getMonth(), last.getDate() + 1).toISOString()
        try {
            events = JSON.parse(CalendarBridge.eventsJson(fromIso, toIso))
        } catch (e) {
            events = []
        }
    }
    onCellsChanged: reloadEvents()

    // Real signal (see calendarbridge.h) instead of a polling timer -
    // fires the instant a background sync or event creation finishes.
    Connections {
        target: CalendarBridge
        function onEventsChanged() { monthGrid.reloadEvents() }
    }

    function eventsForDay(date) {
        return monthGrid.events.filter(function(e) {
            return monthGrid.isSameDay(new Date(e.startsAt), date)
        })
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Kirigami.Units.gridUnit * 1.5
        spacing: Kirigami.Units.largeSpacing

        // --- toolbar ---
        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing

            Kirigami.Heading {
                level: 2
                text: monthGrid.periodTitle
            }

            RowLayout {
                spacing: 0
                Controls.ToolButton {
                    icon.name: "go-previous"
                    display: Controls.AbstractButton.IconOnly
                    onClicked: monthGrid.stepPeriod(-1)
                }
                Controls.ToolButton {
                    icon.name: "go-next"
                    display: Controls.AbstractButton.IconOnly
                    onClicked: monthGrid.stepPeriod(1)
                }
            }

            Controls.Button {
                text: qsTr("Today")
                flat: true
                onClicked: viewDate = new Date()
            }

            Item { Layout.fillWidth: true }

            Controls.TabBar {
                id: viewSwitch
                currentIndex: monthGrid.viewMode
                onCurrentIndexChanged: monthGrid.viewMode = currentIndex
                Controls.TabButton { text: qsTr("Day") }
                Controls.TabButton { text: qsTr("Week") }
                Controls.TabButton { text: qsTr("Month") }
            }
        }

        // --- grid: month view (existing) or day/week time-grid, whichever is active ---
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: monthGrid.viewMode === 2
            color: Kirigami.Theme.backgroundColor
            border.color: monthGrid.hairline
            border.width: 1
            radius: Kirigami.Units.cornerRadius

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 1
                spacing: 0

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Repeater {
                        model: monthGrid.dayHeaders
                        Controls.Label {
                            required property string modelData
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            horizontalAlignment: Text.AlignHCenter
                            text: modelData
                            opacity: 0.55
                            font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                            font.bold: true
                            padding: Kirigami.Units.smallSpacing
                        }
                    }
                }

                Kirigami.Separator { Layout.fillWidth: true }

                GridLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    columns: 7
                    rowSpacing: 0
                    columnSpacing: 0

                    Repeater {
                        model: monthGrid.cells

                        Rectangle {
                            id: cell
                            required property var modelData
                            readonly property bool isToday: monthGrid.isSameDay(modelData.date, new Date())

                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.preferredWidth: 1
                            Layout.preferredHeight: 1
                            border.color: monthGrid.hairline
                            border.width: 1

                            // "Сьогодні" заливає весь день м'яким градієнтом
                            // замість обведення самої цифри.
                            gradient: isToday ? todayGradient : null
                            color: isToday ? "transparent" : Kirigami.Theme.backgroundColor

                            Gradient {
                                id: todayGradient
                                orientation: Gradient.Vertical
                                GradientStop { position: 0.0; color: Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.22) }
                                GradientStop { position: 0.65; color: Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.03) }
                            }

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: Kirigami.Units.smallSpacing
                                spacing: 2

                                Controls.Label {
                                    text: cell.modelData.date.getDate()
                                    opacity: cell.modelData.outside ? 0.35 : 1.0
                                    color: cell.isToday ? Kirigami.Theme.highlightColor : Kirigami.Theme.textColor
                                    font.bold: cell.isToday
                                }

                                Repeater {
                                    model: {
                                        const dayEvents = monthGrid.eventsForDay(cell.modelData.date)
                                        return cell.modelData.outside ? [] : dayEvents.slice(0, monthGrid.maxVisibleEvents)
                                    }

                                    EventChip {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        eventData: modelData
                                        onClicked: (mouse, item) => eventCard.openFor(modelData, item)
                                    }
                                }

                                Controls.Label {
                                    readonly property int total: monthGrid.eventsForDay(cell.modelData.date).length
                                    visible: !cell.modelData.outside && total > monthGrid.maxVisibleEvents
                                    text: qsTr("+%1 more").arg(total - monthGrid.maxVisibleEvents)
                                    font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                                    opacity: 0.6
                                }

                                Item { Layout.fillHeight: true }
                            }
                        }
                    }
                }
            }
        }

        WeekDayGrid {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: monthGrid.viewMode !== 2
            viewDate: monthGrid.viewDate
            daysToShow: monthGrid.viewMode === 0 ? 1 : 7
        }
    }

    Controls.RoundButton {
        text: "+"
        font.pixelSize: 22
        width: 50; height: 50
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Kirigami.Units.gridUnit
        Kirigami.Theme.colorSet: Kirigami.Theme.Button
        highlighted: true
        Controls.ToolTip.visible: hovered
        Controls.ToolTip.text: qsTr("New event")
        onClicked: eventEditor.openFor(new Date())
    }

    EventEditor {
        id: eventEditor
    }

    EventCard {
        id: eventCard
        onEditRequested: (eventData) => eventEditor.openForEdit(eventData)
    }

    Component.onCompleted: reloadEvents()
}
