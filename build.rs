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

    // Run Qt6 MOC for headers that define Q_OBJECT signals.
    // Prefer /usr/lib/qt6/moc (Qt6) over the system default which may be Qt5.
    let moc_bin = if std::path::Path::new("/usr/lib/qt6/moc").exists() {
        "/usr/lib/qt6/moc"
    } else {
        "moc"
    };
    for header in &[
        "cpp/BookmarkStore.h",
        "cpp/BookmarksBar.h",
        "cpp/WebContentView.h",
    ] {
        let stem = std::path::Path::new(header)
            .file_stem()
            .unwrap()
            .to_str()
            .unwrap();
        let moc_out = out_dir.join(format!("moc_{stem}.cpp"));
        let mut cmd = std::process::Command::new(moc_bin);
        cmd.arg(header).arg("-o").arg(&moc_out);
        // Pass Qt include paths so moc can resolve Qt types in the headers
        for p in &qt.include_paths {
            cmd.arg("-I").arg(p);
        }
        cmd.arg("-I").arg(".").arg("-I").arg("cpp");
        let status = cmd.status().expect("Qt moc must be available");
        assert!(status.success(), "Qt moc failed for {header}");
        // moc_*.cpp files are compiled by the build below
        let _ = &moc_out; // used below
    }

    let mut build = cxx_build::bridge("src/bridge.rs");
    build
        .cpp(true)
        .std("c++17")
        .warnings(true)
        .flag_if_supported("-fPIC")
        .include(".")
        .include("cpp");

    for path in &qt.include_paths {
        build.include(path);
    }

    for flag in &qt.defines {
        match &flag.1 {
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
        "cpp/WebViewURL.cpp",
        "cpp/BookmarksBar.cpp",
        "cpp/BookmarkStore.cpp",
        "cpp/FindInPageWidget.cpp",
        "cpp/WebContentPlaceholder.cpp",
        "cpp/WebContentView.cpp",
        "cpp/ChromeStyle.cpp",
        "cpp/Icon.cpp",
        "cpp/Settings.cpp",
    ]);
    build.file(resources_cpp);

    // Compile MOC-generated files for Q_OBJECT classes
    for stem in &["BookmarkStore", "BookmarksBar", "WebContentView"] {
        build.file(out_dir.join(format!("moc_{stem}.cpp")));
    }

    build.compile("servoq-qt-widgets");

    println!("cargo:rerun-if-changed=src/bridge.rs");
    println!("cargo:rerun-if-changed=src/servo_controller.rs");
    println!("cargo:rerun-if-changed=cpp");
    println!("cargo:rerun-if-changed=cpp/resources.qrc");
    println!("cargo:rerun-if-changed=cpp/icons/ladybird.png");
}
