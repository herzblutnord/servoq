#[cxx_qt::bridge]
pub mod qobject {
    unsafe extern "C++" {
        include!("cxx-qt-lib/qstring.h");

        type QString = cxx_qt_lib::QString;
    }

    extern "RustQt" {
        #[qobject]
        #[qml_element]
        #[qproperty(QString, url)]
        #[qproperty(QString, title)]
        #[qproperty(bool, loading)]
        #[qproperty(bool, can_go_back)]
        #[qproperty(bool, can_go_forward)]
        #[qproperty(QString, status_text)]
        #[qproperty(bool, bookmarks_bar_visible)]
        #[qproperty(bool, find_in_page_visible)]
        #[namespace = "servoq"]
        type BrowserController = super::BrowserControllerRust;

        #[qinvokable]
        #[cxx_name = "loadUrl"]
        fn load_url(self: Pin<&mut BrowserController>, url: &QString);

        #[qinvokable]
        #[cxx_name = "goBack"]
        fn go_back(self: Pin<&mut BrowserController>);

        #[qinvokable]
        #[cxx_name = "goForward"]
        fn go_forward(self: Pin<&mut BrowserController>);

        #[qinvokable]
        fn reload(self: Pin<&mut BrowserController>);

        #[qinvokable]
        #[cxx_name = "newTab"]
        fn new_tab(self: Pin<&mut BrowserController>);

        #[qinvokable]
        #[cxx_name = "closeCurrentTab"]
        fn close_current_tab(self: Pin<&mut BrowserController>);

        #[qinvokable]
        #[cxx_name = "toggleBookmark"]
        fn toggle_bookmark(self: Pin<&mut BrowserController>);

        #[qinvokable]
        #[cxx_name = "showFindInPage"]
        fn show_find_in_page(self: Pin<&mut BrowserController>);

        #[qinvokable]
        #[cxx_name = "hideFindInPage"]
        fn hide_find_in_page(self: Pin<&mut BrowserController>);
    }
}

use core::pin::Pin;
use cxx_qt_lib::QString;

#[derive(Default)]
pub struct BrowserControllerRust {
    url: QString,
    title: QString,
    loading: bool,
    can_go_back: bool,
    can_go_forward: bool,
    status_text: QString,
    bookmarks_bar_visible: bool,
    find_in_page_visible: bool,
}

impl qobject::BrowserController {
    pub fn load_url(mut self: Pin<&mut Self>, url: &QString) {
        println!("TODO: load Servo URL: {url}");

        self.as_mut().set_url(url.clone());
        self.as_mut()
            .set_title(QString::from("ServoQ - placeholder"));
        self.as_mut().set_loading(false);
        self.as_mut().set_status_text(QString::from(
            "Servo renderer placeholder: navigation queued",
        ));
    }

    pub fn go_back(mut self: Pin<&mut Self>) {
        println!("TODO: Servo go_back()");
        self.as_mut()
            .set_status_text(QString::from("Back navigation is not wired yet"));
    }

    pub fn go_forward(mut self: Pin<&mut Self>) {
        println!("TODO: Servo go_forward()");
        self.as_mut()
            .set_status_text(QString::from("Forward navigation is not wired yet"));
    }

    pub fn reload(mut self: Pin<&mut Self>) {
        println!("TODO: Servo reload()");
        self.as_mut()
            .set_status_text(QString::from("Reload will refresh Servo content later"));
    }

    pub fn new_tab(mut self: Pin<&mut Self>) {
        println!("TODO: create browser tab");
        self.as_mut().set_url(QString::from("about:blank"));
        self.as_mut().set_title(QString::from("New Tab"));
        self.as_mut()
            .set_status_text(QString::from("New tab placeholder created"));
    }

    pub fn close_current_tab(mut self: Pin<&mut Self>) {
        println!("TODO: close current browser tab");
        self.as_mut()
            .set_status_text(QString::from("Close tab placeholder"));
    }

    pub fn toggle_bookmark(mut self: Pin<&mut Self>) {
        println!("TODO: toggle bookmark for current page");
        self.as_mut()
            .set_status_text(QString::from("Bookmark toggle placeholder"));
    }

    pub fn show_find_in_page(mut self: Pin<&mut Self>) {
        println!("TODO: show find in page");
        self.as_mut().set_find_in_page_visible(true);
        self.as_mut()
            .set_status_text(QString::from("Find in page placeholder"));
    }

    pub fn hide_find_in_page(mut self: Pin<&mut Self>) {
        println!("TODO: hide find in page");
        self.as_mut().set_find_in_page_visible(false);
    }
}
