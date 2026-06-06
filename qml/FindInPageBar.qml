import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    required property var controller

    implicitHeight: 42
    color: "#141618"
    border.width: 0

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 1
        color: "#373a3f"
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 5
        spacing: 6

        TextField {
            id: findField
            Layout.preferredWidth: 250
            Layout.minimumWidth: 50
            Layout.preferredHeight: 30
            color: "#eef1f6"
            selectedTextColor: "white"
            selectionColor: "#3d7eea"
            placeholderText: "Search"
            placeholderTextColor: "#8a929d"
            font.pixelSize: 13
            background: Rectangle {
                radius: 8
                color: "#1d1f23"
                border.width: 1
                border.color: findField.activeFocus ? "#3d7eea" : "#373a3f"
            }
            onAccepted: root.controller.status_text = "Find next placeholder: " + text
        }

        ChromeButton {
            Layout.preferredWidth: 30
            Layout.preferredHeight: 30
            radius: 7
            text: "⌃"
            fontSize: 15
            onClicked: root.controller.status_text = "Find previous placeholder"
        }

        ChromeButton {
            Layout.preferredWidth: 30
            Layout.preferredHeight: 30
            radius: 7
            text: "⌄"
            fontSize: 15
            onClicked: root.controller.status_text = "Find next placeholder"
        }

        CheckBox {
            id: matchCase
            text: "Match Case"
            checked: false
            indicator.width: 16
            indicator.height: 16
            contentItem: Text {
                text: matchCase.text
                color: "#9aa3b0"
                font.pixelSize: 13
                verticalAlignment: Text.AlignVCenter
                leftPadding: matchCase.indicator.width + matchCase.spacing
            }
        }

        Text {
            text: ""
            visible: text.length > 0
            color: "#9aa3b0"
            font.pixelSize: 13
            font.weight: Font.Bold
        }

        Item { Layout.fillWidth: true }

        ChromeButton {
            Layout.preferredWidth: 30
            Layout.preferredHeight: 30
            radius: 7
            text: "×"
            fontSize: 17
            onClicked: root.controller.hideFindInPage()
        }
    }

    onVisibleChanged: if (visible) findField.forceActiveFocus()
}
