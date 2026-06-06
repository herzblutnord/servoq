import QtQuick
import QtQuick.Controls

Control {
    id: control

    property alias text: label.text
    property color foreground: enabled ? "#eef1f6" : "#9aa3b0"
    property color hoverColor: "#26292d"
    property color pressColor: "#313439"
    property color borderColor: "#373a3f"
    property color baseColor: "transparent"
    property int radius: 17
    property int fontSize: 14
    property bool destructive: false

    signal clicked

    implicitWidth: 36
    implicitHeight: 36
    opacity: enabled ? 1.0 : 0.55

    background: Rectangle {
        radius: control.radius
        color: {
            if (mouse.pressed && control.destructive)
                return "#c42b1c"
            if (mouse.containsMouse && control.destructive)
                return "#c42b1c"
            if (mouse.pressed)
                return control.pressColor
            if (mouse.containsMouse)
                return control.hoverColor
            return control.baseColor
        }
        border.width: control.destructive && mouse.containsMouse ? 0 : 1
        border.color: mouse.containsMouse || mouse.pressed ? control.borderColor : "transparent"
    }

    contentItem: Text {
        id: label
        color: control.destructive && mouse.containsMouse ? "white" : control.foreground
        font.pixelSize: control.fontSize
        font.weight: Font.Medium
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: control.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        enabled: control.enabled
        onClicked: control.clicked()
    }
}
