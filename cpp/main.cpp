#include "qt_app.h"
#include "BrowserWindow.h"
#include "servo_callbacks.h"
#include "servoq/src/bridge.rs.h"

#include <QApplication>

namespace servoq {

int run_qt_application()
{
    static int argc = 1;
    static char app_name[] = "servoq";
    static char* argv[] = { app_name, nullptr };

    QApplication app(argc, argv);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [] {
        servoq::begin_servo_shutdown();
    });

    // Phase 0 fix (Hypothesis A): initialize Servo at application startup,
    // before the main window is shown and before Qt delivers any
    // show/resize/paint events to WebContentView widgets.
    //
    // Previously Servo was initialized lazily in WebContentView::showEvent(),
    // which fires during window.show(). At that point Qt is already dispatching
    // events concurrently with Servo's internal thread-pool and font-system
    // startup. If a layout request reaches the font cache before it is fully
    // populated, it can return a stale FontRef whose high bits contain codepoint
    // data — exactly the 0x300e.../0x30d2... pattern seen in the SIGSEGV dumps.
    //
    // Calling init_servo() here gives Servo's constellation, layout, and font
    // subsystems time to fully initialize before any web content or font
    // shaping is requested.
    servoq::init_servo();

    ServoQ::BrowserWindow window;
    window.show();
    int result = app.exec();
    servoq::begin_servo_shutdown();
    return result;
}

}
