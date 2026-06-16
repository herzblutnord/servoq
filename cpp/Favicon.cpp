/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020, Paul Roukema <roukemap@gmail.com>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2023, Srikavin Ramkumar <me@srikavin.me>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Ladybird:
 *   Libraries/LibWeb/DOM/Document.cpp (check_favicon_after_loading_link_resource)
 *   Libraries/LibWeb/HTML/HTMLLinkElement.cpp (process_icon_resource, decode_favicon,
 *     load_fallback_favicon_if_needed)
 *   Libraries/LibGfx/ImageFormats/ICOLoader.cpp (find_largest_image)
 */
// Favicon pipeline: collect <link rel="icon"> candidates (tree order), fall back
// to /favicon.ico, decode each (largest frame in multi-image .ico), and apply the
// one with the largest pixel area (ties go to the last declared).

#include "Favicon.h"
#include "FaviconStore.h"
#include "Tab.h"
#include "WebContentView.h"

#include <QBuffer>
#include <QDebug>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QImageReader>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPixmap>
#include <QRegularExpression>
#include <QSvgRenderer>
#include <QUrl>
#include <QVector>

#include <memory>
#include <optional>

namespace ServoQ {

namespace {

bool debug_enabled()
{
    static bool const v = qEnvironmentVariableIsSet("SERVOQ_DEBUG");
    return v;
}

void debug_log_favicon(int tab_id, QString const& detail)
{
    if (debug_enabled())
        qInfo().nospace() << "SERVOQ_DEBUG favicon tab_id=" << tab_id << " " << detail;
}

// A probe's generation must still be current when its async fetches complete;
// navigations bump the generation so stale results are dropped.
QHash<int, int>& favicon_generations()
{
    static QHash<int, int> s_generations;
    return s_generations;
}

QNetworkAccessManager& favicon_network_manager()
{
    static QNetworkAccessManager manager;
    return manager;
}

QString http_header_value(QNetworkReply* reply, QByteArray const& header)
{
    return QString::fromLatin1(reply->rawHeader(header).trimmed());
}

QNetworkRequest favicon_request(QUrl const& url)
{
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ServoQ/0.1"));
    request.setTransferTimeout(15000);
    // Favicon fetches don't benefit from HTTP/2 multiplexing, and Qt's HTTP/2
    // keep-alive handling is the source of the spurious "QIODevice::read
    // (QSslSocket): device not open" warnings seen during loads. Force HTTP/1.1.
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    return request;
}

bool bytes_look_like_svg(QByteArray const& bytes)
{
    auto prefix = bytes.left(512).trimmed().toLower();
    return prefix.startsWith("<svg") || (prefix.startsWith("<?xml") && prefix.contains("<svg"));
}

QString detect_favicon_format(QUrl const& favicon_url, QString const& content_type, QByteArray const& bytes)
{
    auto path = favicon_url.path().toLower();
    auto mime = content_type.toLower();
    if (path.endsWith(QStringLiteral(".svg")) || mime.contains(QStringLiteral("image/svg+xml")) || bytes_look_like_svg(bytes))
        return QStringLiteral("svg");
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    auto format = reader.format();
    if (!format.isEmpty())
        return QString::fromLatin1(format).toLower();
    if (path.endsWith(QStringLiteral(".ico")))
        return QStringLiteral("ico");
    if (path.endsWith(QStringLiteral(".png")))
        return QStringLiteral("png");
    if (path.endsWith(QStringLiteral(".jpg")) || path.endsWith(QStringLiteral(".jpeg")))
        return QStringLiteral("jpeg");
    return QStringLiteral("unknown");
}

// .ico files bundle several resolutions but QImageReader returns only the first;
// pick the frame with the largest pixel area, tiebreaking on bits per pixel.
QImage read_largest_frame(QImageReader& reader)
{
    auto format = reader.format();
    bool is_multi_resolution_icon = format == QByteArrayLiteral("ico") || format == QByteArrayLiteral("cur");
    if (!is_multi_resolution_icon || reader.imageCount() <= 1)
        return reader.read();

    QImage largest;
    qint64 max_area = 0;
    int max_bits_per_pixel = 0;
    auto frame_count = reader.imageCount();
    for (int index = 0; index < frame_count; ++index) {
        if (!reader.jumpToImage(index))
            continue;
        auto frame = reader.read();
        if (frame.isNull())
            continue;
        auto area = static_cast<qint64>(frame.width()) * frame.height();
        if (largest.isNull() || area > max_area || (area == max_area && frame.depth() > max_bits_per_pixel)) {
            largest = frame;
            max_area = area;
            max_bits_per_pixel = frame.depth();
        }
    }
    return largest;
}

std::optional<QImage> decode_favicon_bytes(int tab_id, QUrl const& page_url, QUrl const& favicon_url, QString const& content_type, QByteArray const& bytes)
{
    auto format = detect_favicon_format(favicon_url, content_type, bytes);
    debug_log_favicon(tab_id,
        QStringLiteral("page_url=%1 favicon_url=%2 mime=%3 input_bytes=%4 detected_input_format=%5")
            .arg(page_url.toString(), favicon_url.toString(), content_type.isEmpty() ? QStringLiteral("<none>") : content_type)
            .arg(bytes.size())
            .arg(format));

    if (format == QStringLiteral("svg")) {
        QSvgRenderer renderer(bytes);
        if (!renderer.isValid()) {
            debug_log_favicon(tab_id,
                QStringLiteral("page_url=%1 favicon_url=%2 decode_failure=invalid_svg").arg(page_url.toString(), favicon_url.toString()));
            return {};
        }

        static constexpr int FaviconBitmapSize = 64;
        QImage image(FaviconBitmapSize, FaviconBitmapSize, QImage::Format_RGBA8888);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        renderer.render(&painter, QRectF(0, 0, FaviconBitmapSize, FaviconBitmapSize));
        painter.end();
        debug_log_favicon(tab_id,
            QStringLiteral("page_url=%1 favicon_url=%2 decoded_output=%3x%4")
                .arg(page_url.toString(), favicon_url.toString())
                .arg(image.width())
                .arg(image.height()));
        return image;
    }

    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly)) {
        debug_log_favicon(tab_id,
            QStringLiteral("page_url=%1 favicon_url=%2 decode_failure=buffer_open_failed").arg(page_url.toString(), favicon_url.toString()));
        return {};
    }

    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    QImage image = read_largest_frame(reader);
    if (image.isNull()) {
        debug_log_favicon(tab_id,
            QStringLiteral("page_url=%1 favicon_url=%2 decode_failure=%3")
                .arg(page_url.toString(), favicon_url.toString(), reader.errorString()));
        return {};
    }

    auto rgba = image.convertToFormat(QImage::Format_RGBA8888);
    debug_log_favicon(tab_id,
        QStringLiteral("page_url=%1 favicon_url=%2 decoded_output=%3x%4")
            .arg(page_url.toString(), favicon_url.toString())
            .arg(rgba.width())
            .arg(rgba.height()));
    return rgba;
}

QString extract_html_attr(QString const& tag, QString const& attr)
{
    QRegularExpression re(QStringLiteral("\\b%1\\s*=\\s*(['\"])(.*?)\\1").arg(QRegularExpression::escape(attr)),
        QRegularExpression::CaseInsensitiveOption);
    auto match = re.match(tag);
    if (match.hasMatch())
        return match.captured(2);

    QRegularExpression unquoted(QStringLiteral("\\b%1\\s*=\\s*([^\\s>]+)").arg(QRegularExpression::escape(attr)),
        QRegularExpression::CaseInsensitiveOption);
    match = unquoted.match(tag);
    return match.hasMatch() ? match.captured(1) : QString {};
}

// Collect every <link rel~="icon"> href in tree order, like Ladybird's
// Document favicon candidate collection. Empty result => caller falls back to
// /favicon.ico (load_fallback_favicon_if_needed).
QList<QUrl> favicon_candidates_from_html(QUrl const& page_url, QByteArray const& html)
{
    // Favicons live in <head>; regexing the whole (multi-MB) document stalled the
    // UI at load-finish, so bound the work to </head>/<body>, capped at 64 KB.
    QByteArray head = html;
    int head_end = head.indexOf("</head>");
    if (head_end < 0)
        head_end = head.indexOf("</HEAD>");
    if (head_end < 0)
        head_end = head.indexOf("<body");
    if (head_end < 0)
        head_end = head.indexOf("<BODY");
    if (head_end >= 0)
        head.truncate(head_end);
    else if (head.size() > 65536)
        head.truncate(65536);

    QList<QUrl> candidates;
    auto text = QString::fromUtf8(head);
    QRegularExpression link_re(QStringLiteral("<link\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    auto it = link_re.globalMatch(text);
    while (it.hasNext()) {
        auto tag = it.next().captured(0);
        auto rel = extract_html_attr(tag, QStringLiteral("rel")).toLower();
        if (!rel.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).contains(QStringLiteral("icon")))
            continue;
        auto href = extract_html_attr(tag, QStringLiteral("href"));
        if (!href.isEmpty())
            candidates.append(page_url.resolved(QUrl(href)));
    }
    return candidates;
}

// One in-flight probe: all candidate fetches for a single page load share this
// state, and selection runs once the last fetch completes.
struct ProbeState {
    int tab_id { 0 };
    int generation { 0 };
    QUrl page_url;
    QList<QUrl> candidates; // tree order
    QVector<QImage> decoded; // parallel to candidates; null = fetch/decode failed
    int remaining { 0 };
};

void finish_probe(std::shared_ptr<ProbeState> const& state)
{
    if (favicon_generations().value(state->tab_id) != state->generation) {
        debug_log_favicon(state->tab_id,
            QStringLiteral("page_url=%1 skipped=stale_generation").arg(state->page_url.toString()));
        return;
    }

    // Port of Document::check_favicon_after_loading_link_resource(): iterate
    // candidates last-to-first keeping only strictly larger icons, so equally
    // sized icons resolve to the last one declared in tree order.
    QImage best;
    qint64 best_area = -1;
    int best_index = -1;
    for (auto i = state->decoded.size(); i-- > 0;) {
        auto const& icon = state->decoded[i];
        if (icon.isNull())
            continue;
        auto area = static_cast<qint64>(icon.width()) * icon.height();
        if (area > best_area) {
            best = icon;
            best_area = area;
            best_index = static_cast<int>(i);
        }
    }
    if (best.isNull()) {
        debug_log_favicon(state->tab_id,
            QStringLiteral("page_url=%1 result=no_icon_decoded candidates=%2")
                .arg(state->page_url.toString())
                .arg(state->candidates.size()));
        return;
    }

    auto* view = WebContentView::findByTabId(state->tab_id);
    if (!view || !view->tab() || QUrl(view->tab()->url()) != state->page_url) {
        debug_log_favicon(state->tab_id,
            QStringLiteral("page_url=%1 skipped=stale_page").arg(state->page_url.toString()));
        return;
    }

    debug_log_favicon(state->tab_id,
        QStringLiteral("page_url=%1 selected_favicon_url=%2 selected=%3/%4 size=%5x%6")
            .arg(state->page_url.toString(), state->candidates[best_index].toString())
            .arg(best_index + 1)
            .arg(state->candidates.size())
            .arg(best.width())
            .arg(best.height()));
    apply_favicon_bitmap(view, best);
}

void fetch_icon_candidate(std::shared_ptr<ProbeState> const& state, int index)
{
    auto favicon_url = state->candidates[index];
    debug_log_favicon(state->tab_id,
        QStringLiteral("page_url=%1 favicon_url=%2 fetch=icon").arg(state->page_url.toString(), favicon_url.toString()));
    auto* reply = favicon_network_manager().get(favicon_request(favicon_url));
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, state, index, favicon_url] {
        reply->deleteLater();
        if (favicon_generations().value(state->tab_id) != state->generation) {
            debug_log_favicon(state->tab_id,
                QStringLiteral("page_url=%1 favicon_url=%2 skipped=stale_generation").arg(state->page_url.toString(), favicon_url.toString()));
            return;
        }
        if (reply->error() == QNetworkReply::NoError) {
            auto content_type = http_header_value(reply, "content-type");
            auto bytes = reply->readAll();
            if (auto decoded = decode_favicon_bytes(state->tab_id, state->page_url, favicon_url, content_type, bytes))
                state->decoded[index] = *decoded;
        } else {
            debug_log_favicon(state->tab_id,
                QStringLiteral("page_url=%1 favicon_url=%2 decode_failure=network_error:%3")
                    .arg(state->page_url.toString(), favicon_url.toString(), reply->errorString()));
        }
        if (--state->remaining == 0)
            finish_probe(state);
    });
}

} // namespace

void apply_favicon_bitmap(WebContentView* view, QImage const& image)
{
    if (!view || !view->tab())
        return;
    auto copy = image.convertToFormat(QImage::Format_RGBA8888);
    view->tab()->on_favicon_change(QIcon(QPixmap::fromImage(copy)));

    // Like Ladybird's ViewImplementation::set_favicon(): persist as PNG, into
    // the favicons database (Chromium-style) so session restore, bookmarks,
    // and history menus can show the icon without a live page.
    QByteArray png_bytes;
    QBuffer buffer(&png_bytes);
    if (buffer.open(QIODevice::WriteOnly) && copy.save(&buffer, "PNG")) {
        auto url = view->tab()->url();
        FaviconStore::the()->storeIcon(url, png_bytes);
        debug_log_favicon(view->tabId(),
            QStringLiteral("page_url=%1 storage=favicon_db png_bytes=%2")
                .arg(url)
                .arg(png_bytes.size()));
    }
}

void start_favicon_probe(WebContentView* view)
{
    if (!view || !view->tab())
        return;
    QUrl page_url(view->tab()->url());
    if (!page_url.isValid() || (page_url.scheme() != QStringLiteral("http") && page_url.scheme() != QStringLiteral("https")))
        return;

    auto tab_id = view->tabId();
    auto generation = favicon_generations().value(tab_id) + 1;
    favicon_generations().insert(tab_id, generation);
    debug_log_favicon(tab_id, QStringLiteral("page_url=%1 fetch=html generation=%2").arg(page_url.toString()).arg(generation));

    auto* reply = favicon_network_manager().get(favicon_request(page_url));
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, tab_id, generation, page_url] {
        reply->deleteLater();
        if (favicon_generations().value(tab_id) != generation) {
            debug_log_favicon(tab_id, QStringLiteral("page_url=%1 skipped=stale_html_generation").arg(page_url.toString()));
            return;
        }

        QList<QUrl> candidates;
        if (reply->error() != QNetworkReply::NoError) {
            debug_log_favicon(tab_id,
                QStringLiteral("page_url=%1 html_fetch_failure=%2 fallback=/favicon.ico")
                    .arg(page_url.toString(), reply->errorString()));
        } else {
            auto content_type = http_header_value(reply, "content-type");
            auto html = reply->readAll();
            candidates = favicon_candidates_from_html(page_url, html);
            debug_log_favicon(tab_id,
                QStringLiteral("page_url=%1 html_mime=%2 html_bytes=%3 candidates=%4")
                    .arg(page_url.toString(), content_type.isEmpty() ? QStringLiteral("<none>") : content_type)
                    .arg(html.size())
                    .arg(candidates.size()));
        }
        // Ladybird's load_fallback_favicon_if_needed(): no <link rel="icon">
        // in the document -> try /favicon.ico.
        if (candidates.isEmpty())
            candidates.append(page_url.resolved(QUrl(QStringLiteral("/favicon.ico"))));

        auto state = std::make_shared<ProbeState>();
        state->tab_id = tab_id;
        state->generation = generation;
        state->page_url = page_url;
        state->candidates = candidates;
        state->decoded.resize(candidates.size());
        state->remaining = static_cast<int>(candidates.size());
        for (int i = 0; i < candidates.size(); ++i)
            fetch_icon_candidate(state, i);
    });
}

void favicon_tab_closed(int tab_id)
{
    favicon_generations().remove(tab_id);
}

} // namespace ServoQ
