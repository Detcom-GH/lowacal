import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
ColumnLayout {
    property alias text: title.text
    property string explanation
    property alias icon: btn.icon
    property Action helpfulAction
    Label { id: title }
    Button { id: btn; objectName: "placeholderAction"; action: helpfulAction }
}
