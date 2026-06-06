import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root

    required property var controller

    implicitHeight: 44
    color: "#141618"
    border.width: 0

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: "#373a3f"
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 4
        anchors.rightMargin: 4
        spacing: 0

        TabButton {
            Layout.preferredWidth: Math.max(128, Math.min(240, (root.width - 90) / 4))
            Layout.preferredHeight: 38
            controller: root.controller
            active: true
            onCloseRequested: root.controller.closeCurrentTab()
        }

        ChromeButton {
            Layout.preferredWidth: 30
            Layout.preferredHeight: 30
            radius: 16
            text: "+"
            fontSize: 18
            foreground: "#eef1f6"
            onClicked: root.controller.newTab()
        }

        Item { Layout.fillWidth: true }
    }
}
