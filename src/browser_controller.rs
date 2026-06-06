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
}

impl qobject::BrowserController {
    pub fn load_url(mut self: Pin<&mut Self>, url: &QString) {
        println!("TODO: load Servo URL: {url}");

        self.as_mut().set_url(url.clone());
        self.as_mut()
            .set_title(QString::from("ServoQ - placeholder"));
        self.as_mut().set_loading(false);
    }

    pub fn go_back(self: Pin<&mut Self>) {
        println!("TODO: Servo go_back()");
    }

    pub fn go_forward(self: Pin<&mut Self>) {
        println!("TODO: Servo go_forward()");
    }

    pub fn reload(self: Pin<&mut Self>) {
        println!("TODO: Servo reload()");
    }
}
