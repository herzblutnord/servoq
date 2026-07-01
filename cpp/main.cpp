#include "qt_app.h"
#include "ui/BrowserWindow.h"
#include "ui/Icon.h"
#include "engine/servo_callbacks.h"
#include "engine/WebViewURL.h"
#include "servoq/src/bridge.rs.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImageReader>
#include <QLocale>
#include <QDebug>
#include <QResource>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QWindow>

#include <cstdio>
#include <string>
#include <vector>

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

// CLI arguments: an existing local path (relative or absolute) opens as a
// file URL; everything else goes through the URL-bar grammar (search fallback).
static QString startup_url_for_argument(QString const& argument)
{
    QFileInfo info(argument);
    if (info.exists())
        return QUrl::fromLocalFile(info.absoluteFilePath()).toString(QUrl::FullyEncoded);
    return ServoQ::WebViewURL::sanitize_url(argument).value_or(argument);
}

int run_qt_application(::rust::Vec<::rust::String> args)
{
    // QApplication keeps argc/argv references for its whole lifetime.
    static std::vector<std::string> arg_storage;
    static std::vector<char*> arg_pointers;
    static int argc = 0;
    for (auto const& arg : args)
        arg_storage.emplace_back(std::string(arg));
    if (arg_storage.empty())
        arg_storage.emplace_back("servoq");
    for (auto& arg : arg_storage)
        arg_pointers.push_back(arg.data());
    arg_pointers.push_back(nullptr);
    argc = static_cast<int>(arg_storage.size());

    QApplication app(argc, arg_pointers.data());
    QCoreApplication::setApplicationName(QStringLiteral("ServoQ"));
    QCoreApplication::setOrganizationName(QStringLiteral("ServoQ"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("ServoQ — a Qt shell for the Servo web engine"));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("url"),
        QStringLiteral("Web addresses or local files to open, one tab each."),
        QStringLiteral("[url...]"));
    QCommandLineOption active_tab_option(QStringLiteral("active-tab"),
        QStringLiteral("Select tab <index> (0-based, over all tabs) after startup."),
        QStringLiteral("index"));
    parser.addOption(active_tab_option);
    parser.process(app);
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

    // Initialize Servo eagerly here, before window.show() and any Qt
    // show/resize/paint, to avoid the lazy-init font-cache crash
    // (docs/DEVIATIONS.md §0o).
    servoq::init_servo();

    ServoQ::BrowserWindow window;

    QStringList startup_urls;
    for (auto const& argument : parser.positionalArguments())
        startup_urls.append(startup_url_for_argument(argument));
    bool active_tab_ok = false;
    int active_tab = parser.value(active_tab_option).toInt(&active_tab_ok);
    window.openStartupUrls(startup_urls, active_tab_ok ? active_tab : -1);

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
