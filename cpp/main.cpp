#include "qt_app.h"
#include "BrowserWindow.h"

#include <QApplication>

namespace servoq {

int run_qt_application()
{
    static int argc = 1;
    static char app_name[] = "servoq";
    static char* argv[] = { app_name, nullptr };

    QApplication app(argc, argv);
    ServoQ::BrowserWindow window;
    window.show();
    return app.exec();
}

}
