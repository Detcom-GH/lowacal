import QtQuick
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

// The Lowacal dachshund, drawn with characters - in-app branding instead of
// the picture logo (the picture stays as the system app icon). Art: data/dachshund.txt.
// Uses the user's own monospace font from Plasma settings. With a width set
// (e.g. Layout.fillWidth in the sidebar) the font shrinks to fit, so the
// tail never gets clipped; without one it's drawn at pixelSize.
Controls.Label {
    objectName: "asciiDachshund"
    property real pixelSize: 12

    text: "                         __\n"
        + "  ,                    ,\" e`--o\n"
        + " ((                   (  | __,'\n"
        + "  \\\\~----------------' \\_;/\n"
        + "  (                      /\n"
        + "  /) ._______________.  )\n"
        + " (( (               (( (\n"
        + "  ``-'               ``-'"
    textFormat: Text.PlainText
    wrapMode: Text.NoWrap
    font.family: Kirigami.Theme.fixedWidthFont.family
    font.pixelSize: pixelSize
    fontSizeMode: Text.HorizontalFit
    minimumPixelSize: 6
    color: Kirigami.Theme.textColor
    opacity: 0.85
    Accessible.name: "Lowacal"
}
