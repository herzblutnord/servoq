use cxx_qt_build::{CxxQtBuilder, QmlModule};

fn main() {
    CxxQtBuilder::new_qml_module(
        QmlModule::new("moe.herz.servoq")
            .version(1, 0)
            .qml_file("qml/main.qml"),
    )
    .qt_module("Network")
    .files(["src/browser_controller.rs"])
    .build();
}
