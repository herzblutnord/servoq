import QtQuick
import QtQuick.Controls
import moe.herz.servoq 1.0

ApplicationWindow {
    id: root

    width: 1280
    height: 820
    minimumWidth: 900
    minimumHeight: 620
    visible: true
    color: "#08090d"
    title: browser.title.length > 0 ? browser.title : "ServoQ"

    BrowserController {
        id: browser
        url: "https://servo.org/"
        title: "ServoQ"
        status_text: "Ready"
        bookmarks_bar_visible: true
    }

    BrowserWindow {
        anchors.fill: parent
        controller: browser
    }
}
