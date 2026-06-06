import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    required property var controller

    implicitHeight: 36
    color: "#101620"
    border.color: "#1d2635"
    border.width: 1

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        spacing: 8

        Text {
            text: "Bookmarks"
            color: "#7d8ca5"
            font.pixelSize: 11
            font.weight: Font.Bold
        }

        Repeater {
            model: ["Servo", "Rust", "Qt", "CXX-Qt"]

            delegate: ChromeButton {
                text: modelData
                implicitHeight: 26
                foreground: "#c8d2e3"
                hoverColor: "#202b3e"
                onClicked: root.controller.loadUrl("https://servo.org/")
            }
        }

        Item { Layout.fillWidth: true }

        Text {
            text: "placeholder bar"
            color: "#566177"
            font.pixelSize: 11
        }
    }
}
