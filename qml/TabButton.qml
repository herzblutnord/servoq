import QtQuick
import QtQuick.Layouts

Item {
    id: root

    required property var controller
    property bool active: true
    property bool hovered: mouse.containsMouse
    property int minTabWidth: 128
    property int maxTabWidth: 240

    signal closeRequested

    implicitWidth: Math.max(minTabWidth, Math.min(maxTabWidth, 220))
    implicitHeight: 38

    Rectangle {
        id: shadow2
        anchors.fill: tabCard
        anchors.topMargin: 2
        radius: 9
        color: root.active ? "#32000000" : "transparent"
        visible: root.active
    }

    Rectangle {
        id: shadow1
        anchors.fill: tabCard
        anchors.topMargin: 1
        radius: 9
        color: root.active ? "#70000000" : "transparent"
        visible: root.active
    }

    Rectangle {
        id: tabCard
        anchors.fill: parent
        anchors.leftMargin: 4
        anchors.rightMargin: 4
        anchors.topMargin: 3
        anchors.bottomMargin: 3
        radius: 9
        border.width: root.active ? 1 : 0
        border.color: "#42ffffff"
        color: root.active ? "transparent" : root.hovered ? "#18ffffff" : "transparent"

        gradient: root.active ? activeGradient : null
    }

    Gradient {
        id: activeGradient
        GradientStop { position: 0.0; color: "#48494b" }
        GradientStop { position: 1.0; color: "#434546" }
    }

    RowLayout {
        anchors.fill: tabCard
        anchors.leftMargin: 8
        anchors.rightMargin: 7
        spacing: 8

        Text {
            Layout.preferredWidth: 16
            Layout.preferredHeight: 16
            text: "◉"
            color: root.active ? "#dce3ed" : "#c8d0da"
            opacity: root.active ? 1.0 : root.hovered ? 0.92 : 0.86
            font.pixelSize: 10
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        Text {
            Layout.fillWidth: true
            text: root.controller.title.length > 0 ? root.controller.title : "New Tab"
            color: root.active ? "#eef1f6" : "#dfe4ec"
            opacity: root.active ? 1.0 : root.hovered ? 0.94 : 0.88
            font.pixelSize: 13
            font.weight: root.active ? Font.DemiBold : Font.Normal
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }

        ChromeButton {
            Layout.preferredWidth: 22
            Layout.preferredHeight: 22
            radius: 11
            text: "×"
            fontSize: 16
            foreground: "#eef1f6"
            hoverColor: "#26292d"
            pressColor: "#313439"
            borderColor: "#4b5056"
            visible: root.active || root.hovered
            onClicked: root.closeRequested()
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
    }
}
