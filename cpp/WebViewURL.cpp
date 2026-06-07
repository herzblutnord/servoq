#include "WebViewURL.h"

#include <QFileInfo>
#include <QSet>
#include <QUrl>

namespace ServoQ::WebViewURL {
namespace {

bool debug_enabled()
{
    return qEnvironmentVariableIsSet("SERVOQ_DEBUG");
}

void debug_log(QString const& event, QString const& detail)
{
    if (debug_enabled())
        qInfo().nospace() << "SERVOQ_DEBUG " << event << " " << detail;
}

std::optional<QString> search_url_or_error(QString const& location)
{
    // Ladybird delegates this to Application::settings().search_engine(). ServoQ
    // does not yet have that settings/search-provider infrastructure, so the
    // faithful no-search-engine result is no URL.
    debug_log(QStringLiteral("ladybird_search_provider_unavailable"), QStringLiteral("query=%1").arg(location));
    return std::nullopt;
}

bool has_supported_scheme(QString const& scheme)
{
    static QSet<QString> const supported_schemes {
        QStringLiteral("about"),
        QStringLiteral("data"),
        QStringLiteral("file"),
        QStringLiteral("http"),
        QStringLiteral("https"),
        QStringLiteral("resource"),
    };
    return supported_schemes.contains(scheme.toLower());
}

bool is_ipv4_address(QString const& host)
{
    auto parts = host.split(QLatin1Char('.'));
    if (parts.size() != 4)
        return false;

    for (auto const& part : parts) {
        bool ok = false;
        auto value = part.toInt(&ok);
        if (!ok || value < 0 || value > 255)
            return false;
    }

    return true;
}

bool is_reserved_tld(QString const& domain)
{
    static QStringList const reserved_tlds {
        QStringLiteral(".test"),
        QStringLiteral(".example"),
        QStringLiteral(".invalid"),
        QStringLiteral(".localhost"),
    };

    for (auto const& tld : reserved_tlds) {
        if (domain.size() > tld.size() && domain.endsWith(tld, Qt::CaseInsensitive))
            return true;
    }

    return false;
}

bool has_known_public_suffix(QString const& domain)
{
    // Ladybird uses LibURL public suffix data. ServoQ does not link that data yet;
    // this set covers the common registered suffixes needed by the current chrome
    // tests while preserving Ladybird's invalid-TLD search/error behavior for
    // values like example.def.
    static QSet<QString> const known_tlds {
        QStringLiteral("abc"), QStringLiteral("app"), QStringLiteral("at"), QStringLiteral("biz"),
        QStringLiteral("co"), QStringLiteral("com"), QStringLiteral("de"), QStringLiteral("dev"),
        QStringLiteral("edu"), QStringLiteral("fr"), QStringLiteral("gov"), QStringLiteral("io"),
        QStringLiteral("me"), QStringLiteral("moe"), QStringLiteral("net"), QStringLiteral("org"),
        QStringLiteral("rs"), QStringLiteral("uk"), QStringLiteral("us"), QStringLiteral("xyz"),
    };

    auto labels = domain.toLower().split(QLatin1Char('.'), Qt::SkipEmptyParts);
    if (labels.size() < 2)
        return false;

    return known_tlds.contains(labels.last());
}

bool host_is_acceptable_without_public_suffix(QString const& host)
{
    return host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0 || is_ipv4_address(host);
}

std::optional<QUrl> parse_url(QString const& text)
{
    QUrl url(text, QUrl::StrictMode);
    if (!url.isValid() || url.scheme().isEmpty())
        return std::nullopt;
    return url;
}

QString serialize(QUrl const& url)
{
    auto serialized_url = url;
    auto scheme = serialized_url.scheme().toLower();
    if ((scheme == QStringLiteral("http") || scheme == QStringLiteral("https")) && serialized_url.path().isEmpty())
        serialized_url.setPath(QStringLiteral("/"));
    return serialized_url.toString(QUrl::FullyEncoded);
}

}

std::optional<QString> sanitize_url(QString const& raw_location, AppendTLD append_tld)
{
    auto location = raw_location.trimmed();
    debug_log(QStringLiteral("raw_location_input"), QStringLiteral("input=%1").arg(raw_location));

    QFileInfo file_info(location);
    if (file_info.exists()) {
        auto canonical_path = file_info.canonicalFilePath();
        if (!canonical_path.isEmpty()) {
            auto file_url = QUrl::fromLocalFile(canonical_path);
            auto serialized = serialize(file_url);
            debug_log(QStringLiteral("ladybird_navigation_target"), QStringLiteral("url=%1").arg(serialized));
            return serialized;
        }
        return search_url_or_error(location);
    }

    bool https_scheme_was_guessed = false;
    auto parsed_url = parse_url(location);
    if (!parsed_url.has_value() || parsed_url->scheme().compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0) {
        parsed_url = parse_url(QStringLiteral("https://") + location);
        if (!parsed_url.has_value())
            return search_url_or_error(location);
        https_scheme_was_guessed = true;
    }

    auto url = *parsed_url;
    auto scheme = url.scheme().toLower();
    if (!has_supported_scheme(scheme))
        return search_url_or_error(location);

    auto host = url.host();
    if (!host.isEmpty()) {
        if (host.contains(QLatin1Char('"')))
            return search_url_or_error(location);

        if (is_reserved_tld(host)) {
            auto serialized = serialize(url);
            debug_log(QStringLiteral("ladybird_navigation_target"), QStringLiteral("url=%1").arg(serialized));
            return serialized;
        }

        if (!host_is_acceptable_without_public_suffix(host) && !has_known_public_suffix(host)) {
            if (append_tld == AppendTLD::Yes) {
                url.setHost(host + QStringLiteral(".com"));
            } else if (https_scheme_was_guessed) {
                return search_url_or_error(location);
            }
        }
    }

    auto serialized = serialize(url);
    debug_log(QStringLiteral("ladybird_navigation_target"), QStringLiteral("url=%1").arg(serialized));
    return serialized;
}

bool location_looks_like_url(QString const& location, AppendTLD append_tld)
{
    return sanitize_url(location, append_tld).has_value();
}

QString url_for_display(QString const& raw_url)
{
    QUrl url(raw_url, QUrl::StrictMode);
    auto scheme = url.scheme().toLower();
    if (!url.isValid() || (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")))
        return raw_url;

    QString display;
    auto user_info = url.userInfo(QUrl::PrettyDecoded); // FullyDecoded not permitted here
    if (!user_info.isEmpty())
        display += user_info + QLatin1Char('@');

    auto host = url.host();
    if (host.startsWith(QStringLiteral("www."), Qt::CaseInsensitive))
        host = host.mid(4);
    display += host;

    if (url.port() != -1)
        display += QStringLiteral(":%1").arg(url.port());

    auto path = url.path(QUrl::FullyEncoded);
    auto query = url.query(QUrl::FullyEncoded);
    auto fragment = url.fragment(QUrl::FullyEncoded);
    if (path != QStringLiteral("/") || !query.isEmpty() || !fragment.isEmpty())
        display += path;
    if (!query.isEmpty())
        display += QLatin1Char('?') + query;
    if (!fragment.isEmpty())
        display += QLatin1Char('#') + fragment;

    return display;
}

}
