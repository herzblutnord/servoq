import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import moe.herz.servoq 1.0

ApplicationWindow {
    id: root

    width: 1280
    height: 800
    visible: true
    title: browser.title.length > 0 ? browser.title : "ServoQ"

    BrowserController {
        id: browser
        url: "https://servo.org/"
        title: "ServoQ"
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            height: 48
            color: "#17171a"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 8

                Button {
                    text: "←"
                    enabled: browser.can_go_back
                    onClicked: browser.goBack()
                }

                Button {
                    text: "→"
                    enabled: browser.can_go_forward
                    onClicked: browser.goForward()
                }

                Button {
                    text: "↻"
                    onClicked: browser.reload()
                }

                TextField {
                    id: addressBar
                    Layout.fillWidth: true
                    text: browser.url
                    selectByMouse: true
                    onAccepted: browser.loadUrl(text)
                }

                Button {
                    text: "Go"
                    onClicked: browser.loadUrl(addressBar.text)
                }
            }
        }

        Rectangle {
            id: webPlaceholder

            Layout.fillWidth: true
            Layout.fillHeight: true

            color: "#101014"

            Column {
                anchors.centerIn: parent
                spacing: 12

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Servo render surface goes here"
                    color: "#f0f0f0"
                    font.pixelSize: 28
                }

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "URL: " + browser.url
                    color: "#b8b8b8"
                    font.pixelSize: 15
                }

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "First milestone: replace this rectangle with a Servo WebView."
                    color: "#8f8f8f"
                    font.pixelSize: 14
                }
            }
        }
    }
}
