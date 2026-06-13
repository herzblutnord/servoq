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
    let qt_svg = pkg_config::Config::new()
        .atleast_version("6")
        .probe("Qt6Svg")
        .expect("Qt 6 Svg development package must be available via pkg-config");
    let qt_network = pkg_config::Config::new()
        .atleast_version("6")
        .probe("Qt6Network")
        .expect("Qt 6 Network development package must be available via pkg-config");
    let qt_sql = pkg_config::Config::new()
        .atleast_version("6")
        .probe("Qt6Sql")
        .expect("Qt 6 Sql development package must be available via pkg-config");
    let qt_dbus = pkg_config::Config::new()
        .atleast_version("6")
        .probe("Qt6DBus")
        .expect("Qt 6 DBus development package must be available via pkg-config");
    let qt_wayland = pkg_config::Config::new()
        .atleast_version("6")
        .probe("Qt6WaylandClient")
        .ok();

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
        "cpp/FaviconStore.h",
        "cpp/HistoryStore.h",
        "cpp/WebContentView.h",
        "cpp/MprisManager.h",
        "cpp/InternalPageView.h",
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
    for path in &qt_svg.include_paths {
        build.include(path);
    }
    for path in &qt_network.include_paths {
        build.include(path);
    }
    for path in &qt_sql.include_paths {
        build.include(path);
    }
    for path in &qt_dbus.include_paths {
        build.include(path);
    }
    if let Some(qt_wayland) = &qt_wayland {
        for path in &qt_wayland.include_paths {
            build.include(path);
        }
    }
    for private_include in [
        "/usr/include/qt6/QtCore/6.11.1",
        "/usr/include/qt6/QtGui/6.11.1",
        "/usr/include/qt6/QtWaylandClient/6.11.1",
    ] {
        if std::path::Path::new(private_include).exists() {
            build.include(private_include);
        }
    }

    for flag in &qt.defines {
        match &flag.1 {
            Some(value) => build.define(&flag.0, Some(value.as_str())),
            None => build.define(&flag.0, None),
        };
    }
    for flag in qt_svg
        .defines
        .iter()
        .chain(qt_network.defines.iter())
        .chain(qt_sql.defines.iter())
        .chain(qt_dbus.defines.iter())
    {
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
        "cpp/HistoryStore.cpp",
        "cpp/FaviconStore.cpp",
        "cpp/SessionStore.cpp",
        "cpp/PermissionStore.cpp",
        "cpp/StorageDb.cpp",
        "cpp/TabSearch.cpp",
        "cpp/Favicon.cpp",
        "cpp/FindInPageWidget.cpp",
        "cpp/WebContentPlaceholder.cpp",
        "cpp/WebContentView.cpp",
        "cpp/WebDialogs.cpp",
        "cpp/ChromeStyle.cpp",
        "cpp/Icon.cpp",
        "cpp/Settings.cpp",
        "cpp/MprisManager.cpp",
        "cpp/InternalPageView.cpp",
    ]);
    build.file(resources_cpp);

    // Compile MOC-generated files for Q_OBJECT classes
    for stem in &[
        "BookmarkStore",
        "BookmarksBar",
        "FaviconStore",
        "HistoryStore",
        "WebContentView",
        "MprisManager",
        "InternalPageView",
    ] {
        build.file(out_dir.join(format!("moc_{stem}.cpp")));
    }

    build.compile("servoq-qt-widgets");

    // commit_toplevel_wl_surface (WebContentView.cpp) calls wl_surface_commit /
    // wl_display_flush directly; make sure libwayland-client is linked even if
    // no other dependency pulls it in.
    println!("cargo:rustc-link-lib=wayland-client");

    // On Linux, statically linked HarfBuzz symbols are exported by default from
    // the executable.  libfreetype.so.6 imports hb_* symbols and the dynamic
    // linker would resolve them to our bundled HarfBuzz 8.4.0 instead of the
    // system libharfbuzz.so.0 (14.2.1 on Arch).  The struct layout of hb_face_t
    // changed between those versions, so FreeType writes callback pointers at
    // offsets from 14.2.1 but our 8.4.0 reads them at different offsets ->
    // garbage function pointer -> SIGSEGV in hb_face_reference_table.
    //
    // --exclude-libs,ALL marks every symbol that came from a static archive as
    // hidden (local), removing them from the .dynsym table.  libfreetype.so.6
    // then falls back to resolving hb_* from libharfbuzz.so.0 (the same version
    // it was compiled against), eliminating the ABI mismatch.
    //
    // Servo's own shaping still uses the bundled 8.4.0 (calls are resolved at
    // compile time) and the two HarfBuzz environments never share objects.
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("linux") {
        println!("cargo:rustc-link-arg=-Wl,--exclude-libs,ALL");
    }

    println!("cargo:rerun-if-changed=src/bridge.rs");
    println!("cargo:rerun-if-changed=src/servo_controller.rs");
    println!("cargo:rerun-if-changed=cpp");
    println!("cargo:rerun-if-changed=cpp/resources.qrc");
    println!("cargo:rerun-if-changed=cpp/icons/servo.png");
    println!("cargo:rerun-if-changed=data/servoq.desktop");
    println!("cargo:rerun-if-changed=scripts/install-dev-desktop-file.sh");
}
