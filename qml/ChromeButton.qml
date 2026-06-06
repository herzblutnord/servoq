import QtQuick
import QtQuick.Controls

Control {
    id: control

    property alias text: label.text
    property color foreground: enabled ? "#e8edf7" : "#6f7785"
    property color hoverColor: "#2a3140"
    property color pressColor: "#343d50"
    property color baseColor: "transparent"
    property int radius: 9

    signal clicked

    implicitWidth: Math.max(34, label.implicitWidth + 18)
    implicitHeight: 34
    enabled: true

    background: Rectangle {
        radius: control.radius
        color: mouse.pressed ? control.pressColor : mouse.containsMouse ? control.hoverColor : control.baseColor
        border.color: mouse.containsMouse ? "#3d4658" : "transparent"
        border.width: 1
    }

    contentItem: Text {
        id: label
        color: control.foreground
        font.pixelSize: 14
        font.weight: Font.DemiBold
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
