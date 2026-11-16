import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Controls.Popup {
    id: about
    // Width from the window overlay, NOT `parent`: these dialogs are declared
    // inside the 236 px sidebar, so parent.width squeezed them to a strip
    // (found by rendering the real UI).
    width: Math.min(380, Controls.Overlay.overlay ? Controls.Overlay.overlay.width - 40 : 380)
    modal: true
    focus: true
    closePolicy: Controls.Popup.CloseOnEscape | Controls.Popup.CloseOnPressOutside
    anchors.centerIn: Controls.Overlay.overlay

    readonly property color hairline: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.12)
    readonly property string repoUrl: "https://github.com/Detcom-GH/lowacal"

    background: Rectangle {
        color: Kirigami.Theme.backgroundColor
        radius: Kirigami.Units.cornerRadius
        border.color: about.hairline
        border.width: 1
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        AsciiDachshund {
            Layout.alignment: Qt.AlignHCenter
            pixelSize: 11
        }

        ColumnLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 0
            Kirigami.Heading {
                Layout.alignment: Qt.AlignHCenter
                level: 2
                text: "Lowacal" // brand name, never translated
            }
            Controls.Label {
                objectName: "aboutVersion"
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("Version %1").arg(Qt.application.version)
                opacity: 0.7
            }
        }

        Controls.Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: qsTr("A calendar for iCloud and CalDAV that keeps reminding you even with the window closed.")
        }

        Controls.Label {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("License: %1").arg("GPL-3.0-or-later")
            font.pixelSize: Kirigami.Theme.smallFont.pixelSize
            opacity: 0.7
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing
            Controls.Button {
                Layout.fillWidth: true
                text: qsTr("Source code")
                icon.name: "internet-services"
                onClicked: Qt.openUrlExternally(about.repoUrl)
            }
            Controls.Button {
                Layout.fillWidth: true
                text: qsTr("Report a bug")
                icon.name: "tools-report-bug"
                onClicked: Qt.openUrlExternally(about.repoUrl + "/issues")
            }
        }

        Controls.Button {
            Layout.alignment: Qt.AlignRight
            text: qsTr("Close")
            onClicked: about.close()
        }
    }
}
