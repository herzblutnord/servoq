import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    property bool open: false
    signal newTabRequested
    signal findRequested

    implicitWidth: 36
    implicitHeight: 36

    ChromeButton {
        anchors.fill: parent
        text: "☰"
        fontSize: 17
        onClicked: root.open = !root.open
    }

    Popup {
        id: popup
        x: root.width - width
        y: root.height + 2
        width: 238
        modal: false
        focus: true
        visible: root.open
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: 5
        onClosed: root.open = false

        background: Rectangle {
            color: "#1d1f23"
            radius: 7
            border.width: 1
            border.color: "#373a3f"
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 1

            Repeater {
                model: [
                    { label: "New Tab", action: "new-tab" },
                    { label: "New Window", action: "noop" },
                    { label: "Reopen Recently Closed Tab", action: "noop" },
                    { label: "Close Current Tab", action: "close" },
                    { label: "Find in Page...", action: "find" },
                    { label: "Bookmarks", action: "noop" },
                    { label: "Settings", action: "noop" },
                    { label: "About ServoQ", action: "noop" }
                ]

                delegate: Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 30
                    radius: 5
                    color: menuMouse.containsMouse ? "#26292d" : "transparent"

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 14
                        anchors.right: parent.right
                        anchors.rightMargin: 14
                        text: modelData.label
                        color: "#eef1f6"
                        font.pixelSize: 13
                        elide: Text.ElideRight
                    }

                    MouseArea {
                        id: menuMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            if (modelData.action === "new-tab")
                                root.newTabRequested()
                            else if (modelData.action === "find")
                                root.findRequested()
                            popup.close()
                        }
                    }
                }
            }
        }
    }
}
