import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property var controller

    Rectangle {
        anchors.fill: parent
        color: "#08090d"

        Rectangle {
            anchors.fill: parent
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#151b28" }
                GradientStop { position: 0.38; color: "#0b0f17" }
                GradientStop { position: 1.0; color: "#08090d" }
            }
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            TabStrip {
                Layout.fillWidth: true
                controller: root.controller
            }

            BrowserToolbar {
                Layout.fillWidth: true
                controller: root.controller
            }

            BookmarkBar {
                Layout.fillWidth: true
                visible: root.controller.bookmarks_bar_visible
                controller: root.controller
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                WebViewPlaceholder {
                    anchors.fill: parent
                    controller: root.controller
                }

                FindInPageBar {
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: 18
                    anchors.rightMargin: 22
                    width: Math.min(parent.width - 44, 520)
                    controller: root.controller
                    visible: root.controller.find_in_page_visible
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: root.controller.status_text.length > 0 ? 28 : 0
                visible: root.controller.status_text.length > 0
                color: "#0b0e14"
                border.color: "#1e2533"
                border.width: 1

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: 14
                    anchors.right: parent.right
                    anchors.rightMargin: 14
                    text: root.controller.status_text
                    color: "#9aa7ba"
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
            }
        }
    }
}
