import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root

    required property var controller

    implicitHeight: 34
    color: "#141618"
    border.width: 0

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 4
        anchors.rightMargin: 4
        spacing: 3

        Repeater {
            model: [
                { title: "Servo", icon: "◉", url: "https://servo.org/" },
                { title: "Rust", icon: "◉", url: "https://www.rust-lang.org/" },
                { title: "Qt", icon: "◉", url: "https://www.qt.io/" },
                { title: "CXX-Qt", icon: "▾", url: "https://kdab.github.io/cxx-qt/" }
            ]

            delegate: Rectangle {
                id: bookmarkButton
                Layout.preferredWidth: Math.min(150, Math.max(58, bookmarkText.implicitWidth + 31))
                Layout.preferredHeight: 24
                radius: 7
                color: bookmarkMouse.pressed ? "#313439" : bookmarkMouse.containsMouse ? "#26292d" : "transparent"
                border.width: 1
                border.color: bookmarkMouse.containsMouse || bookmarkMouse.pressed ? "#373a3f" : "transparent"

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 7
                    anchors.rightMargin: 7
                    spacing: 6

                    Text {
                        text: modelData.icon
                        color: "#cbd3dd"
                        font.pixelSize: 10
                    }

                    Text {
                        id: bookmarkText
                        Layout.fillWidth: true
                        text: modelData.title
                        color: "#eef1f6"
                        font.pixelSize: 13
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                MouseArea {
                    id: bookmarkMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: root.controller.loadUrl(modelData.url)
                }
            }
        }

        Item { Layout.fillWidth: true }
    }
}
