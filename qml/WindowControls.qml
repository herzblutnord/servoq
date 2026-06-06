import QtQuick
import QtQuick.Layouts

RowLayout {
    id: root

    spacing: 0

    ChromeButton {
        Layout.preferredWidth: 38
        Layout.preferredHeight: 38
        radius: 0
        text: "−"
        fontSize: 16
    }

    ChromeButton {
        Layout.preferredWidth: 38
        Layout.preferredHeight: 38
        radius: 0
        text: "□"
        fontSize: 13
    }

    ChromeButton {
        Layout.preferredWidth: 38
        Layout.preferredHeight: 38
        radius: 0
        text: "×"
        fontSize: 18
        destructive: true
    }
}
