import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    required property var controller

    implicitHeight: 54
    radius: 16
    color: "#151c29"
    border.color: "#354154"
    border.width: 1

    RowLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        TextField {
            id: findField
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            color: "#eef3fb"
            selectedTextColor: "#ffffff"
            selectionColor: "#3f6fbd"
            placeholderText: "Find in page"
            placeholderTextColor: "#68758a"
            background: Rectangle {
                radius: 10
                color: "#0f141f"
                border.color: findField.activeFocus ? "#5b8def" : "#2a3344"
                border.width: 1
            }
            onAccepted: root.controller.status_text = "Find next placeholder: " + text
        }

        Text {
            text: "0/0"
            color: "#7f8ca2"
            font.pixelSize: 12
        }

        ChromeButton {
            text: "Prev"
            onClicked: root.controller.status_text = "Find previous placeholder"
        }

        ChromeButton {
            text: "Next"
            onClicked: root.controller.status_text = "Find next placeholder"
        }

        ChromeButton {
            text: "x"
            onClicked: root.controller.hideFindInPage()
        }
    }

    onVisibleChanged: if (visible) findField.forceActiveFocus()
}
