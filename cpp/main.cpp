#include "qt_app.h"
#include "BrowserWindow.h"
#include "Icon.h"
#include "servo_callbacks.h"
#include "servoq/src/bridge.rs.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImageReader>
#include <QLocale>
#include <QDebug>
#include <QResource>
#include <QStandardPaths>
#include <QTimer>
#include <QWindow>

#include <cstdio>

void init_servoq_resources()
{
    Q_INIT_RESOURCE(servoq_resources);
}

namespace {

constexpr auto AppIconResource = ":/Icons/servo.png";
static QString const AppIconResourceString = QStringLiteral(":/Icons/servo.png");

static QString icon_sizes_to_string(QIcon const& icon)
{
    QStringList parts;
    for (auto const& size : icon.availableSizes())
        parts.append(QStringLiteral("%1x%2").arg(size.width()).arg(size.height()));
    return parts.join(QLatin1Char(','));
}

static void log_icon_state(char const* label, QIcon const& icon)
{
    if (!qEnvironmentVariableIsSet("SERVOQ_DEBUG"))
        return;
    std::fprintf(stderr, "SERVOQ_DEBUG app_icon %s.isNull=%s availableSizes=%s\n",
        label,
        icon.isNull() ? "true" : "false",
        icon_sizes_to_string(icon).toUtf8().constData());
}

static void log_icon_diagnostics(QApplication const& app, QMainWindow const& window, QIcon const& icon)
{
    if (!qEnvironmentVariableIsSet("SERVOQ_DEBUG"))
        return;
    std::fprintf(stderr, "SERVOQ_DEBUG app_icon resource=%s resource_can_read=%s isNull=%s availableSizes=%s\n",
        AppIconResource,
        QImageReader(AppIconResourceString).canRead() ? "true" : "false",
        icon.isNull() ? "true" : "false",
        icon_sizes_to_string(icon).toUtf8().constData());
    log_icon_state("app.windowIcon", app.windowIcon());
    log_icon_state("browser.windowIcon", window.windowIcon());
    if (auto* handle = window.windowHandle())
        log_icon_state("qwindow.icon", handle->icon());
    else
        std::fprintf(stderr, "SERVOQ_DEBUG app_icon qwindow.icon.isNull=<no-window-handle>\n");
    std::fprintf(stderr, "SERVOQ_DEBUG desktopFileName=%s\n", app.desktopFileName().toUtf8().constData());
}

static void apply_window_icon_to_qwindow(QMainWindow& window, QIcon const& icon)
{
    window.setWindowIcon(icon);
    if (auto* handle = window.windowHandle())
        handle->setIcon(icon);
}

}

namespace servoq {

::rust::String servo_profile_data_dir()
{
    auto const dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty())
        return {};
    QDir().mkpath(dir);
    return ::rust::String(dir.toUtf8().constData());
}

::rust::String system_cjk_font_family(::rust::Str generic)
{
    auto generic_name = QString::fromUtf8(generic.data(), static_cast<int>(generic.size()));
    bool serif = generic_name == QStringLiteral("serif");

    // Order the pan-CJK variants by the system locale so unified Han renders
    // with the user's preferred regional glyphs (Servo can't pick per-script).
    QStringList regions;
    switch (QLocale::system().language()) {
    case QLocale::Korean:
        regions = { QStringLiteral("KR"), QStringLiteral("JP"), QStringLiteral("SC"), QStringLiteral("TC"), QStringLiteral("HK") };
        break;
    case QLocale::Chinese:
        if (auto t = QLocale::system().territory(); t == QLocale::Taiwan || t == QLocale::HongKong || t == QLocale::Macau)
            regions = { QStringLiteral("TC"), QStringLiteral("HK"), QStringLiteral("SC"), QStringLiteral("JP"), QStringLiteral("KR") };
        else
            regions = { QStringLiteral("SC"), QStringLiteral("TC"), QStringLiteral("HK"), QStringLiteral("JP"), QStringLiteral("KR") };
        break;
    case QLocale::Japanese:
    default:
        regions = { QStringLiteral("JP"), QStringLiteral("SC"), QStringLiteral("TC"), QStringLiteral("HK"), QStringLiteral("KR") };
        break;
    }

    QStringList candidates;
    auto base = serif ? QStringLiteral("Noto Serif CJK %1") : QStringLiteral("Noto Sans CJK %1");
    for (auto const& region : regions)
        candidates << base.arg(region);
    if (serif)
        candidates << QStringLiteral("Source Han Serif") << QStringLiteral("AR PL UMing CN");
    else
        candidates << QStringLiteral("Source Han Sans") << QStringLiteral("WenQuanYi Micro Hei")
                   << QStringLiteral("Droid Sans Fallback");

    for (auto const& family : candidates) {
        if (QFontDatabase::hasFamily(family))
            return ::rust::String(family.toUtf8().constData());
    }
    return {};
}

int run_qt_application()
{
    static int argc = 1;
    static char app_name[] = "servoq";
    static char* argv[] = { app_name, nullptr };

    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("ServoQ"));
    QCoreApplication::setOrganizationName(QStringLiteral("ServoQ"));
    init_servoq_resources();
    auto icon = ServoQ::app_icon();
    app.setWindowIcon(icon);
#ifdef Q_OS_LINUX
    app.setDesktopFileName(QStringLiteral("servoq"));
#endif
    if (qEnvironmentVariableIsSet("SERVOQ_DEBUG")) {
        std::fprintf(stderr, "SERVOQ_DEBUG app_icon resource=%s resource_can_read=%s isNull=%s availableSizes=%s\n",
            AppIconResource,
            QImageReader(AppIconResourceString).canRead() ? "true" : "false",
            icon.isNull() ? "true" : "false",
            icon_sizes_to_string(icon).toUtf8().constData());
        log_icon_state("app.windowIcon", app.windowIcon());
        std::fprintf(stderr, "SERVOQ_DEBUG desktopFileName=%s\n", app.desktopFileName().toUtf8().constData());
    }
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
    apply_window_icon_to_qwindow(window, icon);
    window.show();
    apply_window_icon_to_qwindow(window, icon);
    log_icon_diagnostics(app, window, icon);
    QTimer::singleShot(0, &window, [&app, &window, icon] {
        apply_window_icon_to_qwindow(window, icon);
        log_icon_diagnostics(app, window, icon);
    });
    int result = app.exec();
    servoq::begin_servo_shutdown();
    return result;
}

}
