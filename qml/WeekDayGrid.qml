import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

// Shared by both "День" and "Тиждень": an hourly time grid with events
// positioned by their actual start time/duration. daysToShow controls
// which mode this is - 1 for Day, 7 for Week.
//
// Layout note: the grid deliberately avoids nested RowLayout/fillWidth
// for the day columns - an earlier version did that and the columns
// collapsed toward the right edge (fillWidth items competing across a
// ScrollView boundary). Explicit x/width arithmetic on a plain Item is
// dumber but fully predictable.
Rectangle {
    id: root
    color: Kirigami.Theme.backgroundColor

    property date viewDate: new Date()
    property int daysToShow: 7
    property var events: []

    readonly property color hairline: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.12)
    // User override from Settings (-1 = follow the system locale).
    readonly property int firstDayOfWeek: CalendarBridge.firstDayOfWeek >= 0 ? CalendarBridge.firstDayOfWeek : Qt.locale().firstDayOfWeek
    readonly property int hourHeight: 56
    readonly property int timeColWidth: 52
    readonly property real dayColWidth: (width - timeColWidth) / daysToShow

    function isSameDay(a, b) {
        return a.getFullYear() === b.getFullYear() && a.getMonth() === b.getMonth() && a.getDate() === b.getDate()
    }

    readonly property var periodDays: {
        const days = []
        if (daysToShow === 1) {
            days.push(new Date(viewDate.getFullYear(), viewDate.getMonth(), viewDate.getDate()))
            return days
        }
        const dow = viewDate.getDay()
        const diff = (dow - firstDayOfWeek + 7) % 7
        const start = new Date(viewDate.getFullYear(), viewDate.getMonth(), viewDate.getDate() - diff)
        for (let i = 0; i < daysToShow; i++) {
            days.push(new Date(start.getFullYear(), start.getMonth(), start.getDate() + i))
        }
        return days
    }

    function reloadEvents() {
        if (periodDays.length === 0) return
        const first = periodDays[0]
        const last = periodDays[periodDays.length - 1]
        const fromIso = new Date(first.getFullYear(), first.getMonth(), first.getDate()).toISOString()
        const toIso = new Date(last.getFullYear(), last.getMonth(), last.getDate() + 1).toISOString()
        try {
            events = JSON.parse(CalendarBridge.eventsJson(fromIso, toIso))
        } catch (e) {
            events = []
        }
    }
    onPeriodDaysChanged: reloadEvents()

    Connections {
        target: CalendarBridge
        function onEventsChanged() { root.reloadEvents() }
    }

    function timedEventsForDay(date) {
        return root.events.filter(function(e) { return !e.allDay && root.isSameDay(new Date(e.startsAt), date) })
    }
    function allDayEventsForDay(date) {
        return root.events.filter(function(e) { return e.allDay && root.isSameDay(new Date(e.startsAt), date) })
    }

    // Assigns each event a lane (column index within the day) and the
    // lane count of its overlap cluster, so overlapping events split the
    // width side-by-side instead of the later one painting over the
    // earlier. Greedy first-free-lane assignment - the standard approach
    // for calendar day views.
    function layoutDayEvents(date) {
        const evs = timedEventsForDay(date).map(function(e) {
            const s = new Date(e.startsAt)
            const en = new Date(e.endsAt)
            const startMin = s.getHours() * 60 + s.getMinutes()
            // visual floor of ~25 min so tiny events stay clickable and
            // overlap detection matches what's actually drawn
            const endMin = Math.max(startMin + 25, en.getHours() * 60 + en.getMinutes())
            return { data: e, startMin: startMin, endMin: endMin, lane: 0, laneCount: 1 }
        }).sort(function(a, b) { return a.startMin - b.startMin || a.endMin - b.endMin })

        let cluster = []
        let clusterEnd = -1
        function closeCluster() {
            if (cluster.length === 0) return
            const laneEnds = []
            for (const item of cluster) {
                let lane = 0
                while (lane < laneEnds.length && laneEnds[lane] > item.startMin) lane++
                item.lane = lane
                laneEnds[lane] = item.endMin
            }
            for (const item of cluster) item.laneCount = laneEnds.length
            cluster = []
            clusterEnd = -1
        }

        for (const item of evs) {
            if (cluster.length > 0 && item.startMin >= clusterEnd) closeCluster()
            cluster.push(item)
            clusterEnd = Math.max(clusterEnd, item.endMin)
        }
        closeCluster()
        return evs
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // --- day-of-week headers, x-aligned with the day columns below ---
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: headerRow.implicitHeight + Kirigami.Units.smallSpacing

            Row {
                id: headerRow
                x: root.timeColWidth
                Repeater {
                    model: root.periodDays
                    delegate: Column {
                        required property date modelData
                        readonly property bool isToday: root.isSameDay(modelData, new Date())
                        width: root.dayColWidth
                        Controls.Label {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: Qt.locale().dayName(modelData.getDay(), Locale.ShortFormat)
                            opacity: 0.6
                            font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                        }
                        Controls.Label {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: modelData.getDate()
                            font.bold: isToday
                            color: isToday ? Kirigami.Theme.highlightColor : Kirigami.Theme.textColor
                        }
                    }
                }
            }
        }

        // --- all-day strip (only when something's actually there) ---
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? allDayRow.implicitHeight + 4 : 0
            visible: root.periodDays.some(d => root.allDayEventsForDay(d).length > 0)

            Row {
                id: allDayRow
                x: root.timeColWidth
                Repeater {
                    model: root.periodDays
                    delegate: Column {
                        required property date modelData
                        width: root.dayColWidth
                        padding: 2
                        spacing: 2
                        Repeater {
                            model: root.allDayEventsForDay(modelData)
                            delegate: EventChip {
                                required property var modelData
                                width: root.dayColWidth - 4
                                eventData: modelData
                                onClicked: (mouse, item) => eventCard.openFor(modelData, item)
                            }
                        }
                    }
                }
            }
        }

        Kirigami.Separator { Layout.fillWidth: true }

        Controls.ScrollView {
            id: scroller
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            // Both explicit: setting only contentWidth turns off Qt's
            // automatic content-size derivation altogether, which silently
            // killed vertical scrolling (contentHeight stayed 0).
            contentWidth: availableWidth
            contentHeight: 24 * root.hourHeight

            Item {
                width: scroller.availableWidth
                height: 24 * root.hourHeight

                // hour labels
                Repeater {
                    model: 24
                    delegate: Controls.Label {
                        required property int index
                        x: 0
                        y: index * root.hourHeight - (index > 0 ? 7 : 0)
                        width: root.timeColWidth - 8
                        horizontalAlignment: Text.AlignRight
                        visible: index > 0
                        text: index.toString().padStart(2, "0") + ":00"
                        font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                        opacity: 0.5
                    }
                }

                // horizontal hour lines, spanning all the day columns
                Repeater {
                    model: 23
                    delegate: Rectangle {
                        required property int index
                        x: root.timeColWidth
                        y: (index + 1) * root.hourHeight
                        width: parent.width - root.timeColWidth
                        height: 1
                        color: root.hairline
                    }
                }

                // day columns with their events
                Repeater {
                    model: root.periodDays
                    delegate: Rectangle {
                        id: dayColumn
                        required property int index
                        required property date modelData
                        readonly property bool isToday: root.isSameDay(modelData, new Date())

                        x: root.timeColWidth + index * root.dayColWidth
                        y: 0
                        width: root.dayColWidth
                        height: 24 * root.hourHeight
                        border.color: root.hairline
                        border.width: 1
                        color: isToday ? Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.06)
                                       : "transparent"

                        Repeater {
                            model: root.layoutDayEvents(dayColumn.modelData)
                            delegate: Rectangle {
                                id: chip
                                required property var modelData
                                readonly property var ev: modelData.data
                                readonly property real laneWidth: (dayColumn.width - 4) / modelData.laneCount

                                x: 2 + modelData.lane * laneWidth
                                width: laneWidth - (modelData.laneCount > 1 ? 2 : 0)
                                y: (modelData.startMin / 60) * root.hourHeight
                                height: ((modelData.endMin - modelData.startMin) / 60) * root.hourHeight - 2
                                radius: 5
                                color: ev.color && ev.color.length > 0 ? ev.color : Kirigami.Theme.highlightColor

                                Controls.Label {
                                    anchors.fill: parent
                                    anchors.margins: 4
                                    verticalAlignment: Text.AlignTop
                                    text: chip.ev.summary
                                    color: "white"
                                    font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                                    elide: Text.ElideRight
                                    wrapMode: Text.NoWrap
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: (mouse) => eventCard.openFor(chip.ev, chip)
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    EventCard {
        id: eventCard
        onEditRequested: (eventData) => weekDayEditor.openForEdit(eventData)
    }

    EventEditor {
        id: weekDayEditor
    }

    // Scroll to a sane starting position (08:00-ish) instead of midnight,
    // once the view has its real size.
    Component.onCompleted: {
        reloadEvents()
        Qt.callLater(function() {
            if (scroller.contentItem && scroller.contentItem.contentY !== undefined) {
                scroller.contentItem.contentY = 7.5 * root.hourHeight
            }
        })
    }
}
