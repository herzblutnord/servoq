fn main() {
    let out_dir =
        std::path::PathBuf::from(std::env::var("OUT_DIR").expect("OUT_DIR is set by Cargo"));
    let resources_cpp = out_dir.join("servoq_resources.cpp");
    let rcc_status = std::process::Command::new("rcc")
        .args(["-name", "servoq_resources", "cpp/resources.qrc", "-o"])
        .arg(&resources_cpp)
        .status()
        .expect("Qt rcc must be available to compile cpp/resources.qrc");
    assert!(rcc_status.success(), "Qt rcc failed for cpp/resources.qrc");

    let qt = pkg_config::Config::new()
        .atleast_version("6")
        .probe("Qt6Widgets")
        .expect("Qt 6 Widgets development package must be available via pkg-config");

    let mut build = cxx_build::bridge("src/bridge.rs");
    build
        .cpp(true)
        .std("c++17")
        .warnings(true)
        .flag_if_supported("-fPIC")
        .include(".")
        .include("cpp");

    for path in qt.include_paths {
        build.include(path);
    }

    for flag in qt.defines {
        match flag.1 {
            Some(value) => build.define(&flag.0, Some(value.as_str())),
            None => build.define(&flag.0, None),
        };
    }

    build.files([
        "cpp/main.cpp",
        "cpp/BrowserWindow.cpp",
        "cpp/Tab.cpp",
        "cpp/TabBar.cpp",
        "cpp/LocationEdit.cpp",
        "cpp/BookmarksBar.cpp",
        "cpp/FindInPageWidget.cpp",
        "cpp/WebContentPlaceholder.cpp",
        "cpp/ChromeStyle.cpp",
        "cpp/Icon.cpp",
    ]);
    build.file(resources_cpp);

    build.compile("servoq-qt-widgets");

    println!("cargo:rerun-if-changed=src/bridge.rs");
    println!("cargo:rerun-if-changed=src/servo_controller.rs");
    println!("cargo:rerun-if-changed=cpp");
    println!("cargo:rerun-if-changed=cpp/resources.qrc");
    println!("cargo:rerun-if-changed=cpp/icons/ladybird.png");
}
