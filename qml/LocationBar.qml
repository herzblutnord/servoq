import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    required property var controller
    property alias text: addressField.text

    signal accepted(string url)

    implicitHeight: 32
    radius: 16
    color: addressField.activeFocus ? "#151719" : "#121416"
    border.width: 1
    border.color: addressField.activeFocus ? "#475f91" : locationMouse.containsMouse ? "#212428" : "#212326"

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 6
        spacing: 6

        Rectangle {
            Layout.preferredWidth: secureText.visible ? secureText.implicitWidth + 14 : 22
            Layout.preferredHeight: 22
            radius: 10
            color: secureText.visible ? "#2b1f1f" : "transparent"
            border.width: secureText.visible ? 1 : 0
            border.color: "#573a38"

            Text {
                id: secureText
                anchors.centerIn: parent
                visible: false
                text: "Not Secure"
                color: "#e08e88"
                font.pixelSize: 11
                font.weight: Font.Medium
            }

            Text {
                anchors.centerIn: parent
                visible: !secureText.visible
                text: "◉"
                color: "#aab3bf"
                font.pixelSize: 10
            }
        }

        TextField {
            id: addressField
            Layout.fillWidth: true
            text: root.controller.url
            selectByMouse: true
            color: "#f0f3f8"
            selectedTextColor: "white"
            selectionColor: "#3d7eea"
            placeholderText: "Search or enter address"
            placeholderTextColor: "#777f89"
            font.pixelSize: 14
            verticalAlignment: TextInput.AlignVCenter
            background: Item {}
            leftPadding: 0
            rightPadding: 0
            onAccepted: root.accepted(text)
        }

        Rectangle {
            Layout.preferredHeight: 22
            Layout.preferredWidth: zoomLabel.implicitWidth + 14
            radius: 10
            color: "#191d20"
            border.width: 1
            border.color: "#2e3237"
            visible: false

            Text {
                id: zoomLabel
                anchors.centerIn: parent
                text: "100%"
                color: "#9aa3b0"
                font.pixelSize: 11
                font.weight: Font.Medium
            }
        }

        ChromeButton {
            Layout.preferredWidth: 24
            Layout.preferredHeight: 23
            radius: 10
            text: "☆"
            fontSize: 15
            borderColor: "transparent"
            hoverColor: "#25282c"
            pressColor: "#30343a"
            onClicked: root.controller.toggleBookmark()
        }
    }

    MouseArea {
        id: locationMouse
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
    }
}
