pragma Singleton
import QtQuick
// TEST STUB ONLY - mirrors a subset of real Kirigami.Theme properties.
QtObject {
    property color backgroundColor: "#2a2e32"
    property color textColor: "#fcfcfc"
    property color highlightColor: "#3daee9"
    property color negativeTextColor: "#da4453"
    property color positiveTextColor: "#27ae60"
    property font smallFont: Qt.font({ pixelSize: 10 })
    property font fixedWidthFont: Qt.font({ family: "monospace" })
}
