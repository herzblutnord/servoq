import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    required property var controller

    implicitHeight: 58
    color: "#0f141f"
    border.color: "#202838"
    border.width: 1

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 8

        ChromeButton {
            text: "<"
            enabled: root.controller.can_go_back
            onClicked: root.controller.goBack()
        }

        ChromeButton {
            text: ">"
            enabled: root.controller.can_go_forward
            onClicked: root.controller.goForward()
        }

        ChromeButton {
            text: "Reload"
            onClicked: root.controller.reload()
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            radius: 14
            color: addressBar.activeFocus ? "#151c2a" : "#111824"
            border.color: addressBar.activeFocus ? "#5b8def" : "#2a3344"
            border.width: 1

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 8
                spacing: 8

                Text {
                    text: "URL"
                    color: "#748197"
                    font.pixelSize: 11
                    font.weight: Font.Bold
                }

                TextField {
                    id: addressBar
                    Layout.fillWidth: true
                    text: root.controller.url
                    selectByMouse: true
                    color: "#eef3fb"
                    selectedTextColor: "#ffffff"
                    selectionColor: "#3f6fbd"
                    font.pixelSize: 15
                    verticalAlignment: TextInput.AlignVCenter
                    placeholderText: "Search or enter address"
                    placeholderTextColor: "#657187"
                    background: Item {}
                    onAccepted: root.controller.loadUrl(text)
                }

                ChromeButton {
                    Layout.preferredWidth: 34
                    Layout.preferredHeight: 30
                    text: "Go"
                    onClicked: root.controller.loadUrl(addressBar.text)
                }
            }
        }

        ChromeButton {
            text: "Star"
            onClicked: root.controller.toggleBookmark()
        }

        ChromeButton {
            text: "Find"
            onClicked: root.controller.showFindInPage()
        }

        ChromeButton {
            text: "Menu"
        }
    }
}
