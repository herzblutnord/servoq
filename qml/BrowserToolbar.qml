import QtQuick
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
        anchors.bottom: parent.bottom
        height: 1
        color: root.controller.bookmarks_bar_visible ? "transparent" : "#373a3f"
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 4
        anchors.topMargin: 2
        anchors.bottomMargin: 2
        spacing: 6

        RowLayout {
            Layout.preferredHeight: 36
            spacing: 2

            ChromeButton {
                text: "▥"
                fontSize: 15
            }

            Item { Layout.preferredWidth: 8 }

            ChromeButton {
                text: "‹"
                fontSize: 22
                enabled: root.controller.can_go_back
                onClicked: root.controller.goBack()
            }

            ChromeButton {
                text: "›"
                fontSize: 22
                enabled: root.controller.can_go_forward
                onClicked: root.controller.goForward()
            }

            ChromeButton {
                text: root.controller.loading ? "×" : "↻"
                fontSize: root.controller.loading ? 18 : 17
                onClicked: root.controller.reload()
            }
        }

        Item { Layout.preferredWidth: 26 }

        LocationBar {
            Layout.fillWidth: true
            Layout.preferredHeight: 32
            controller: root.controller
            onAccepted: function(url) { root.controller.loadUrl(url) }
        }

        Item { Layout.preferredWidth: 26 }

        MenuButton {
            Layout.preferredWidth: 36
            Layout.preferredHeight: 36
            onNewTabRequested: root.controller.newTab()
            onFindRequested: root.controller.showFindInPage()
        }

        ToolbarSeparator {
            Layout.preferredWidth: 1
            Layout.preferredHeight: 22
        }

        WindowControls {
            Layout.preferredHeight: 38
        }
    }
}
