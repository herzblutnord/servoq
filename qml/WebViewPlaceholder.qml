import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    required property var controller

    color: "#0a0d13"

    Rectangle {
        anchors.fill: parent
        anchors.margins: 18
        radius: 24
        color: "#0e1420"
        border.color: "#202a3b"
        border.width: 1

        Rectangle {
            anchors.fill: parent
            anchors.margins: 1
            radius: 23
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#151f30" }
                GradientStop { position: 0.55; color: "#0d121b" }
                GradientStop { position: 1.0; color: "#090c12" }
            }
        }

        ColumnLayout {
            anchors.centerIn: parent
            width: Math.min(parent.width - 80, 700)
            spacing: 18

            Rectangle {
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: 76
                Layout.preferredHeight: 76
                radius: 22
                color: "#1c2738"
                border.color: "#3b4a62"
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "SQ"
                    color: "#dce7f7"
                    font.pixelSize: 24
                    font.weight: Font.Black
                }
            }

            Text {
                Layout.fillWidth: true
                text: "Servo rendering surface will be inserted here"
                color: "#f4f7fb"
                font.pixelSize: 28
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }

            Text {
                Layout.fillWidth: true
                text: "The browser chrome is live QML. Navigation, tabs, bookmarks, find, and status are placeholder controller state until the Servo WebView integration lands."
                color: "#9ba8bb"
                font.pixelSize: 15
                lineHeight: 1.25
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 44
                radius: 14
                color: "#101824"
                border.color: "#273349"
                border.width: 1

                Text {
                    anchors.fill: parent
                    anchors.leftMargin: 16
                    anchors.rightMargin: 16
                    text: "Current URL: " + root.controller.url
                    color: "#c7d2e3"
                    font.pixelSize: 13
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }
            }
        }
    }
}
