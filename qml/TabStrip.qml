import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    required property var controller

    implicitHeight: 46
    color: "#111722"
    border.color: "#222b3a"
    border.width: 1

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        anchors.topMargin: 7
        spacing: 8

        Rectangle {
            Layout.preferredWidth: Math.min(280, Math.max(190, root.width * 0.24))
            Layout.fillHeight: true
            radius: 12
            color: "#202838"
            border.color: "#3a4659"
            border.width: 1

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 6
                spacing: 8

                Rectangle {
                    Layout.preferredWidth: 9
                    Layout.preferredHeight: 9
                    radius: 5
                    color: root.controller.loading ? "#75d0ff" : "#68d391"
                }

                Text {
                    Layout.fillWidth: true
                    text: root.controller.title.length > 0 ? root.controller.title : "New Tab"
                    color: "#f4f7fb"
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }

                ChromeButton {
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                    text: "x"
                    foreground: "#b8c2d4"
                    onClicked: root.controller.closeCurrentTab()
                }
            }
        }

        ChromeButton {
            Layout.preferredWidth: 34
            Layout.preferredHeight: 34
            text: "+"
            onClicked: root.controller.newTab()
        }

        Item { Layout.fillWidth: true }

        Text {
            text: "ServoQ"
            color: "#657187"
            font.pixelSize: 12
            font.letterSpacing: 1.4
        }
    }
}
