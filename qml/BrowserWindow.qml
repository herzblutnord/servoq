import QtQuick
import QtQuick.Layouts

Item {
    id: root

    required property var controller

    Rectangle {
        anchors.fill: parent
        color: "#141618"

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

            WebViewPlaceholder {
                Layout.fillWidth: true
                Layout.fillHeight: true
                controller: root.controller
            }

            FindInPageBar {
                Layout.fillWidth: true
                visible: root.controller.find_in_page_visible
                controller: root.controller
            }
        }

        Rectangle {
            id: statusBubble
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.bottomMargin: root.controller.find_in_page_visible ? 42 : 0
            width: Math.min(parent.width * 0.62, Math.max(180, statusText.implicitWidth + 10))
            height: 22
            visible: root.controller.status_text.length > 0
            color: "#1d1f23"
            border.width: 1
            border.color: "#373a3f"
            radius: 0

            Text {
                id: statusText
                anchors.fill: parent
                anchors.leftMargin: 4
                anchors.rightMargin: 4
                text: root.controller.status_text
                color: "#eef1f6"
                font.pixelSize: 12
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }
        }
    }
}
