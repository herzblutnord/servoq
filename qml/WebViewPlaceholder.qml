import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root

    required property var controller

    color: "#0d0f12"

    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.width: 0

        ColumnLayout {
            anchors.centerIn: parent
            width: Math.min(parent.width - 80, 620)
            spacing: 12

            Text {
                Layout.fillWidth: true
                text: "Servo WebView placeholder"
                color: "#eef1f6"
                font.pixelSize: 24
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
            }

            Text {
                Layout.fillWidth: true
                text: "Ladybird-style Qt chrome is represented in QML; the actual Servo rendering surface will replace this content area later."
                color: "#9aa3b0"
                font.pixelSize: 14
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                lineHeight: 1.2
            }

            Rectangle {
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: Math.min(520, parent.width)
                Layout.preferredHeight: 30
                radius: 8
                color: "#1d1f23"
                border.width: 1
                border.color: "#373a3f"

                Text {
                    anchors.fill: parent
                    anchors.leftMargin: 9
                    anchors.rightMargin: 9
                    text: root.controller.url
                    color: "#dce3ed"
                    font.pixelSize: 13
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }
            }
        }
    }
}
