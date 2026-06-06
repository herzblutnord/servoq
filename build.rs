use cxx_qt_build::{CxxQtBuilder, QmlModule};

fn main() {
    CxxQtBuilder::new_qml_module(
        QmlModule::new("moe.herz.servoq")
            .version(1, 0)
            .qml_file("qml/main.qml")
            .qml_file("qml/BrowserWindow.qml")
            .qml_file("qml/BrowserToolbar.qml")
            .qml_file("qml/TabStrip.qml")
            .qml_file("qml/BookmarkBar.qml")
            .qml_file("qml/FindInPageBar.qml")
            .qml_file("qml/WebViewPlaceholder.qml")
            .qml_file("qml/ChromeButton.qml"),
    )
    .qt_module("Network")
    .files(["src/browser_controller.rs"])
    .build();
}
