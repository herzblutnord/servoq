/*
 * Copyright (c) 2026, ServoQ contributors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "InternalPageView.h"
#include "FaviconStore.h"
#include "HistoryStore.h"
#include "Icon.h"
#include "PermissionStore.h"
#include "Settings.h"
#include "WebViewURL.h"
#include "servoq/src/bridge.rs.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPdfDocument>
#include <QPdfPageNavigator>
#include <QPdfPageSelector>
#include <QPdfView>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStringList>
#include <QTemporaryFile>
#include <QToolButton>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>

namespace ServoQ {

namespace {

// Normalize a configured homepage / new-tab URL the same way the old Home & New
// Tab dialog did: blank stays about:blank, otherwise run it through the address
// bar's sanitizer (so "example.com" becomes "https://example.com/"). Falls back
// to the raw trimmed text if it can't be made into a URL, so the field is never
// silently emptied.
QString normalize_configured_url(QString const& raw)
{
    auto trimmed = raw.trimmed();
    if (trimmed.isEmpty() || trimmed.compare(QStringLiteral("about:blank"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("about:blank");
    auto sanitized = WebViewURL::sanitize_url(trimmed);
    // Reject inputs that only sanitize into a search query (not a real URL).
    if (sanitized.has_value() && *sanitized != Settings::the()->search_url_for_query(trimmed))
        return *sanitized;
    return trimmed;
}

QString source_url_for_internal_pdf_url(QString const& url)
{
    QUrl parsed(url);
    QUrlQuery query(parsed);
    return query.queryItemValue(QStringLiteral("url"), QUrl::FullyDecoded);
}

QString pdf_error_text(QPdfDocument::Error error)
{
    switch (error) {
    case QPdfDocument::Error::None: return {};
    case QPdfDocument::Error::Unknown: return QStringLiteral("The PDF could not be opened.");
    case QPdfDocument::Error::DataNotYetAvailable: return QStringLiteral("The PDF is still loading.");
    case QPdfDocument::Error::FileNotFound: return QStringLiteral("The PDF file was not found.");
    case QPdfDocument::Error::InvalidFileFormat: return QStringLiteral("This file is not a valid PDF document.");
    case QPdfDocument::Error::IncorrectPassword: return QStringLiteral("This PDF needs a password.");
    case QPdfDocument::Error::UnsupportedSecurityScheme: return QStringLiteral("This PDF uses an unsupported security scheme.");
    }
    return QStringLiteral("The PDF could not be opened.");
}

}

// ---- ConsoleLog ---------------------------------------------------------

static constexpr int kConsoleRingSize = 1000;

ConsoleLog* ConsoleLog::the()
{
    static ConsoleLog* instance = new ConsoleLog();
    return instance;
}

void ConsoleLog::append(int tab_id, int level, QString const& text)
{
    Message msg { tab_id, level, text, QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")) };
    m_messages.append(msg);
    if (m_messages.size() > kConsoleRingSize)
        m_messages.removeFirst();
    emit appended(msg);
}

void ConsoleLog::clear()
{
    m_messages.clear();
    emit cleared();
}

void ConsoleLog::addConsumer()
{
    if (++m_consumers == 1)
        servoq::set_console_capture_enabled(true);
}

void ConsoleLog::removeConsumer()
{
    if (m_consumers > 0 && --m_consumers == 0)
        servoq::set_console_capture_enabled(false);
}

// ---- InternalPageView ---------------------------------------------------

InternalPageView::InternalPageView(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("ServoQInternalPage"));
    setAutoFillBackground(true);
    // Render against the window's base (page) color so internal pages read as
    // content, not chrome, in both light and dark palettes.
    auto pal = palette();
    pal.setColor(QPalette::Window, pal.color(QPalette::Base));
    setPalette(pal);

    m_root_layout = new QVBoxLayout(this);
    m_root_layout->setContentsMargins(0, 0, 0, 0);
    m_root_layout->setSpacing(0);
}

InternalPageView::~InternalPageView()
{
    setConsoleConsuming(false);
}

bool InternalPageView::isInternalUrl(QString const& url)
{
    return url.startsWith(QStringLiteral("servoq://"), Qt::CaseInsensitive);
}

bool InternalPageView::isPdfSourceUrl(QString const& url)
{
    auto parsed = QUrl(url);
    if (!parsed.isValid())
        return false;
    auto scheme = parsed.scheme().toLower();
    if (scheme != QStringLiteral("file") && scheme != QStringLiteral("http") && scheme != QStringLiteral("https"))
        return false;
    return parsed.path().endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive);
}

QString InternalPageView::urlForPdfSource(QString const& source_url)
{
    QUrl url;
    url.setScheme(QStringLiteral("servoq"));
    url.setHost(QStringLiteral("pdf"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("url"), source_url);
    url.setQuery(query);
    return url.toString(QUrl::FullyEncoded);
}

InternalPageView::Kind InternalPageView::kindForUrl(QString const& url)
{
    if (!isInternalUrl(url))
        return Kind::Unknown;
    auto host = QUrl(url).host();
    if (host.isEmpty()) {
        host = url.mid(QStringLiteral("servoq://").size());
        int slash = host.indexOf(QLatin1Char('/'));
        if (slash >= 0)
            host = host.left(slash);
        int query = host.indexOf(QLatin1Char('?'));
        if (query >= 0)
            host = host.left(query);
    }
    host = host.toLower();
    if (host == QStringLiteral("settings"))
        return Kind::Settings;
    if (host == QStringLiteral("history"))
        return Kind::History;
    if (host == QStringLiteral("downloads"))
        return Kind::Downloads;
    if (host == QStringLiteral("debug"))
        return Kind::Debug;
    if (host == QStringLiteral("pdf"))
        return Kind::Pdf;
    return Kind::Unknown;
}

QString InternalPageView::titleForUrl(QString const& url)
{
    switch (kindForUrl(url)) {
    case Kind::Settings: return QStringLiteral("Settings");
    case Kind::History: return QStringLiteral("History");
    case Kind::Downloads: return QStringLiteral("Downloads");
    case Kind::Debug: return QStringLiteral("Debug");
    case Kind::Pdf: {
        auto source = source_url_for_internal_pdf_url(url);
        if (source.isEmpty())
            return QStringLiteral("PDF Viewer");
        auto file_name = QFileInfo(QUrl(source).isLocalFile() ? QUrl(source).toLocalFile() : QUrl(source).path()).fileName();
        return file_name.isEmpty() ? QStringLiteral("PDF Viewer") : file_name;
    }
    case Kind::Unknown: break;
    }
    return QStringLiteral("ServoQ");
}

void InternalPageView::clearContent()
{
    setConsoleConsuming(false);
    m_history_search = nullptr;
    m_history_list = nullptr;
    m_pdf_document = nullptr;
    m_pdf_view = nullptr;
    m_pdf_page_selector = nullptr;
    m_pdf_status_label = nullptr;
    if (m_pdf_network) {
        m_pdf_network->deleteLater();
        m_pdf_network = nullptr;
    }
    if (m_pdf_temp_file) {
        m_pdf_temp_file->deleteLater();
        m_pdf_temp_file = nullptr;
    }
    if (m_content) {
        m_root_layout->removeWidget(m_content);
        delete m_content;
        m_content = nullptr;
    }
}

QWidget* InternalPageView::makeScrollHost(QVBoxLayout*& out_layout)
{
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* inner = new QWidget(scroll);
    auto* outer = new QVBoxLayout(inner);
    outer->setContentsMargins(0, 0, 0, 0);
    // Center the content column on wide windows, like a real settings page.
    auto* column = new QWidget(inner);
    column->setMaximumWidth(840);
    auto* col_row = new QHBoxLayout;
    col_row->addStretch(1);
    col_row->addWidget(column, 0);
    col_row->addStretch(1);
    outer->addLayout(col_row);
    outer->addStretch(1);

    out_layout = new QVBoxLayout(column);
    out_layout->setContentsMargins(32, 28, 32, 28);
    out_layout->setSpacing(18);

    scroll->setWidget(inner);
    return scroll;
}

void InternalPageView::showUrl(QString const& url)
{
    auto kind = kindForUrl(url);
    clearContent();
    m_kind = kind;
    switch (kind) {
    case Kind::Settings: buildSettingsPage(); break;
    case Kind::History: buildHistoryPage(); break;
    case Kind::Downloads: buildDownloadsPage(); break;
    case Kind::Debug: buildDebugPage(); break;
    case Kind::Pdf: buildPdfPage(source_url_for_internal_pdf_url(url)); break;
    case Kind::Unknown: {
        // Unknown servoq:// host: show a small "page not found" notice.
        QVBoxLayout* layout = nullptr;
        auto* host = makeScrollHost(layout);
        auto* title = new QLabel(QStringLiteral("This servoq:// page does not exist."), host);
        auto f = title->font();
        f.setPointSizeF(f.pointSizeF() * 1.4);
        title->setFont(f);
        layout->addWidget(title);
        layout->addWidget(new QLabel(url, host));
        m_content = host;
        m_root_layout->addWidget(m_content, 1);
        break;
    }
    }
}

// ---- PDF viewer ---------------------------------------------------------

void InternalPageView::buildPdfPage(QString const& source_url)
{
    auto* host = new QWidget(this);
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(host);
    toolbar->setObjectName(QStringLiteral("ServoQPdfToolbar"));
    auto* toolbar_layout = new QHBoxLayout(toolbar);
    toolbar_layout->setContentsMargins(12, 8, 12, 8);
    toolbar_layout->setSpacing(8);

    auto* title = new QLabel(titleForUrl(urlForPdfSource(source_url)), toolbar);
    title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    toolbar_layout->addWidget(title, 1);

    auto* zoom_out = new QPushButton(QStringLiteral("-"), toolbar);
    zoom_out->setToolTip(QStringLiteral("Zoom out"));
    auto* zoom_in = new QPushButton(QStringLiteral("+"), toolbar);
    zoom_in->setToolTip(QStringLiteral("Zoom in"));
    auto* fit_width = new QPushButton(QStringLiteral("Fit Width"), toolbar);
    auto* open_external = new QPushButton(QStringLiteral("Open Externally"), toolbar);

    m_pdf_page_selector = new QPdfPageSelector(toolbar);
    toolbar_layout->addWidget(m_pdf_page_selector);
    toolbar_layout->addWidget(zoom_out);
    toolbar_layout->addWidget(zoom_in);
    toolbar_layout->addWidget(fit_width);
    toolbar_layout->addWidget(open_external);
    layout->addWidget(toolbar);

    m_pdf_status_label = new QLabel(QStringLiteral("Loading PDF..."), host);
    m_pdf_status_label->setAlignment(Qt::AlignCenter);
    m_pdf_status_label->setMinimumHeight(32);
    layout->addWidget(m_pdf_status_label);

    m_pdf_document = new QPdfDocument(host);
    m_pdf_view = new QPdfView(host);
    m_pdf_view->setDocument(m_pdf_document);
    m_pdf_view->setPageMode(QPdfView::PageMode::MultiPage);
    m_pdf_view->setZoomMode(QPdfView::ZoomMode::FitToWidth);
    m_pdf_view->setPageSpacing(10);
    m_pdf_view->setDocumentMargins(QMargins(12, 12, 12, 12));
    m_pdf_page_selector->setDocument(m_pdf_document);
    layout->addWidget(m_pdf_view, 1);

    connect(zoom_out, &QPushButton::clicked, this, [this] {
        if (!m_pdf_view)
            return;
        m_pdf_view->setZoomMode(QPdfView::ZoomMode::Custom);
        m_pdf_view->setZoomFactor(std::max<qreal>(0.25, m_pdf_view->zoomFactor() / 1.2));
    });
    connect(zoom_in, &QPushButton::clicked, this, [this] {
        if (!m_pdf_view)
            return;
        m_pdf_view->setZoomMode(QPdfView::ZoomMode::Custom);
        m_pdf_view->setZoomFactor(std::min<qreal>(8.0, m_pdf_view->zoomFactor() * 1.2));
    });
    connect(fit_width, &QPushButton::clicked, this, [this] {
        if (m_pdf_view)
            m_pdf_view->setZoomMode(QPdfView::ZoomMode::FitToWidth);
    });
    connect(open_external, &QPushButton::clicked, this, [source_url] {
        QDesktopServices::openUrl(QUrl(source_url));
    });
    connect(m_pdf_page_selector, &QPdfPageSelector::currentPageChanged, this, [this](int page) {
        if (m_pdf_view && m_pdf_view->pageNavigator() && page >= 0)
            m_pdf_view->pageNavigator()->jump(page, QPointF(), 0);
    });
    connect(m_pdf_view->pageNavigator(), &QPdfPageNavigator::currentPageChanged,
        m_pdf_page_selector, &QPdfPageSelector::setCurrentPage);
    connect(m_pdf_document, &QPdfDocument::passwordRequired, this, [this] {
        if (!m_pdf_document)
            return;
        bool ok = false;
        auto password = QInputDialog::getText(this, QStringLiteral("PDF Password"),
            QStringLiteral("Enter the password for this PDF:"), QLineEdit::Password, {}, &ok);
        if (ok)
            m_pdf_document->setPassword(password);
    });
    connect(m_pdf_document, &QPdfDocument::statusChanged, this, [this](QPdfDocument::Status status) {
        if (!m_pdf_status_label || !m_pdf_document)
            return;
        if (status == QPdfDocument::Status::Ready) {
            m_pdf_status_label->setText(QStringLiteral("%1 page%2")
                .arg(m_pdf_document->pageCount())
                .arg(m_pdf_document->pageCount() == 1 ? QString() : QStringLiteral("s")));
        } else if (status == QPdfDocument::Status::Error) {
            m_pdf_status_label->setText(pdf_error_text(m_pdf_document->error()));
        } else if (status == QPdfDocument::Status::Loading) {
            m_pdf_status_label->setText(QStringLiteral("Loading PDF..."));
        }
    });

    m_content = host;
    m_root_layout->addWidget(m_content, 1);

    auto parsed = QUrl(source_url);
    if (!parsed.isValid() || source_url.isEmpty()) {
        showPdfError(QStringLiteral("No PDF URL was provided."));
    } else if (parsed.isLocalFile()) {
        openPdfFile(parsed.toLocalFile());
    } else {
        startPdfDownload(source_url);
    }
}

void InternalPageView::openPdfFile(QString const& path)
{
    if (!m_pdf_document)
        return;
    auto error = m_pdf_document->load(path);
    if (error != QPdfDocument::Error::None)
        showPdfError(pdf_error_text(error));
}

void InternalPageView::startPdfDownload(QString const& source_url)
{
    if (!m_pdf_document)
        return;
    auto* owner = m_content ? m_content : this;
    m_pdf_network = new QNetworkAccessManager(owner);
    m_pdf_temp_file = new QTemporaryFile(QDir::tempPath() + QStringLiteral("/servoq-pdf-XXXXXX.pdf"), owner);
    if (!m_pdf_temp_file->open()) {
        showPdfError(QStringLiteral("Could not create a temporary file for the PDF."));
        return;
    }

    QNetworkRequest request { QUrl(source_url) };
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ServoQ"));
    auto* reply = m_pdf_network->get(request);
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (m_pdf_temp_file)
            m_pdf_temp_file->write(reply->readAll());
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        if (!m_pdf_status_label)
            return;
        if (total > 0)
            m_pdf_status_label->setText(QStringLiteral("Downloading PDF... %1%").arg((received * 100) / total));
        else
            m_pdf_status_label->setText(QStringLiteral("Downloading PDF..."));
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        finishPdfDownload(reply);
    });
}

void InternalPageView::finishPdfDownload(QNetworkReply* reply)
{
    if (!reply)
        return;
    reply->deleteLater();
    if (!m_pdf_temp_file)
        return;
    m_pdf_temp_file->write(reply->readAll());
    m_pdf_temp_file->flush();

    auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError || (status >= 400 && status < 600)) {
        showPdfError(QStringLiteral("Could not download the PDF: %1").arg(reply->errorString()));
        return;
    }

    auto content_type = reply->header(QNetworkRequest::ContentTypeHeader).toString();
    if (!content_type.isEmpty() && !content_type.startsWith(QStringLiteral("application/pdf"), Qt::CaseInsensitive)
        && !content_type.startsWith(QStringLiteral("text/pdf"), Qt::CaseInsensitive)) {
        showPdfError(QStringLiteral("The server did not return a PDF document."));
        return;
    }

    m_pdf_temp_file->close();
    openPdfFile(m_pdf_temp_file->fileName());
}

void InternalPageView::showPdfError(QString const& message)
{
    if (m_pdf_status_label)
        m_pdf_status_label->setText(message);
}

// ---- Settings page (M4.3) + site data / privacy UI (M4.5) ---------------

void InternalPageView::buildSettingsPage()
{
    QVBoxLayout* layout = nullptr;
    auto* host = makeScrollHost(layout);
    auto* settings = Settings::the();

    auto* heading = new QLabel(QStringLiteral("Settings"), host);
    auto hf = heading->font();
    hf.setPointSizeF(hf.pointSizeF() * 1.8);
    hf.setBold(true);
    heading->setFont(hf);
    layout->addWidget(heading);

    auto notify_changed = [this] {
        if (onSettingsChanged)
            onSettingsChanged();
    };

    // Adds a checkbox row to a form, wired to a setter + live re-apply.
    auto add_check = [this, notify_changed](QFormLayout* form, QWidget* parent, QString const& label,
                         bool checked, std::function<void(bool)> setter) {
        auto* box = new QCheckBox(label, parent);
        box->setChecked(checked);
        connect(box, &QCheckBox::toggled, this, [setter, notify_changed](bool on) {
            setter(on);
            notify_changed();
        });
        form->addRow(box);
        return box;
    };

    // --- Home and startup ---
    {
        auto* group = new QGroupBox(QStringLiteral("Home and startup"), host);
        auto* form = new QFormLayout(group);

        auto* homepage = new QLineEdit(settings->homepage_url(), group);
        homepage->setPlaceholderText(QStringLiteral("about:blank or https://example.com/"));
        connect(homepage, &QLineEdit::editingFinished, this, [homepage, settings, notify_changed] {
            auto normalized = normalize_configured_url(homepage->text());
            settings->set_homepage_url(normalized);
            if (homepage->text().trimmed() != normalized) {
                QSignalBlocker block(homepage);
                homepage->setText(normalized);
            }
            notify_changed();
        });
        form->addRow(QStringLiteral("Homepage:"), homepage);

        // Home-button toggle sits next to the homepage it controls.
        add_check(form, group, QStringLiteral("Show home button in the toolbar"),
            settings->show_home_button(), [settings](bool on) { settings->set_show_home_button(on); });

        auto* new_tab_combo = new QComboBox(group);
        new_tab_combo->addItem(QStringLiteral("Blank page"));
        new_tab_combo->addItem(QStringLiteral("Homepage"));
        new_tab_combo->addItem(QStringLiteral("Custom URL"));
        switch (settings->new_tab_page_behavior()) {
        case NewTabPageBehavior::Blank: new_tab_combo->setCurrentIndex(0); break;
        case NewTabPageBehavior::Homepage: new_tab_combo->setCurrentIndex(1); break;
        case NewTabPageBehavior::CustomUrl: new_tab_combo->setCurrentIndex(2); break;
        }
        form->addRow(QStringLiteral("New tab opens:"), new_tab_combo);

        auto* custom_new_tab = new QLineEdit(settings->custom_new_tab_url(), group);
        custom_new_tab->setPlaceholderText(QStringLiteral("about:blank or https://example.com/"));
        custom_new_tab->setVisible(settings->new_tab_page_behavior() == NewTabPageBehavior::CustomUrl);
        form->addRow(QStringLiteral("Custom new-tab URL:"), custom_new_tab);

        connect(new_tab_combo, &QComboBox::currentIndexChanged, this,
            [custom_new_tab, settings, notify_changed](int index) {
                auto behavior = index == 1 ? NewTabPageBehavior::Homepage
                    : index == 2           ? NewTabPageBehavior::CustomUrl
                                           : NewTabPageBehavior::Blank;
                settings->set_new_tab_page_behavior(behavior);
                custom_new_tab->setVisible(behavior == NewTabPageBehavior::CustomUrl);
                notify_changed();
            });
        connect(custom_new_tab, &QLineEdit::editingFinished, this,
            [custom_new_tab, settings, notify_changed] {
                auto normalized = normalize_configured_url(custom_new_tab->text());
                settings->set_custom_new_tab_url(normalized);
                if (custom_new_tab->text().trimmed() != normalized) {
                    QSignalBlocker block(custom_new_tab);
                    custom_new_tab->setText(normalized);
                }
                notify_changed();
            });

        add_check(form, group, QStringLiteral("Continue where you left off (restore tabs on startup)"),
            settings->restore_session_on_startup(),
            [settings](bool on) { settings->set_restore_session_on_startup(on); });

        layout->addWidget(group);
    }

    // --- Appearance ---
    {
        auto* group = new QGroupBox(QStringLiteral("Appearance"), host);
        auto* form = new QFormLayout(group);

        auto* tab_layout = new QComboBox(group);
        tab_layout->addItem(QStringLiteral("Horizontal tabs"));
        tab_layout->addItem(QStringLiteral("Vertical tabs (collapsed)"));
        tab_layout->addItem(QStringLiteral("Vertical tabs (expanded)"));
        if (!settings->vertical_tabs_enabled())
            tab_layout->setCurrentIndex(0);
        else
            tab_layout->setCurrentIndex(settings->vertical_tabs_expanded() ? 2 : 1);

        auto* hover_expand = new QCheckBox(QStringLiteral("Expand vertical tabs on hover"), group);
        hover_expand->setChecked(settings->vertical_tabs_expand_on_hover());
        hover_expand->setEnabled(settings->vertical_tabs_enabled());

        connect(tab_layout, &QComboBox::currentIndexChanged, this,
            [settings, hover_expand, notify_changed](int index) {
                settings->set_vertical_tabs_enabled(index != 0);
                settings->set_vertical_tabs_expanded(index == 2);
                hover_expand->setEnabled(index != 0);
                notify_changed();
            });
        connect(hover_expand, &QCheckBox::toggled, this, [settings, notify_changed](bool on) {
            settings->set_vertical_tabs_expand_on_hover(on);
            notify_changed();
        });
        form->addRow(QStringLiteral("Tab layout:"), tab_layout);
        form->addRow(hover_expand);

        add_check(form, group, QStringLiteral("Show bookmarks bar"), settings->show_bookmarks_bar(),
            [settings](bool on) { settings->set_show_bookmarks_bar(on); });
        add_check(form, group, QStringLiteral("Show menu bar"), settings->show_menu_bar(),
            [settings](bool on) { settings->set_show_menu_bar(on); });

        layout->addWidget(group);
    }

    // --- Search ---
    {
        auto* group = new QGroupBox(QStringLiteral("Search"), host);
        auto* form = new QFormLayout(group);

        auto* engine_combo = new QComboBox(group);
        auto rebuild_engines = [engine_combo, settings] {
            QSignalBlocker block(engine_combo);
            engine_combo->clear();
            engine_combo->addItems(settings->search_engine_names());
            int idx = engine_combo->findText(settings->search_engine_name());
            if (idx >= 0)
                engine_combo->setCurrentIndex(idx);
        };
        rebuild_engines();
        connect(engine_combo, &QComboBox::currentTextChanged, this, [settings, notify_changed](QString const& name) {
            if (!name.isEmpty())
                settings->set_search_engine_name(name);
            notify_changed();
        });
        form->addRow(QStringLiteral("Default search engine:"), engine_combo);

        auto* engine_buttons = new QWidget(group);
        auto* eb_layout = new QHBoxLayout(engine_buttons);
        eb_layout->setContentsMargins(0, 0, 0, 0);
        auto* add_engine = new QPushButton(QStringLiteral("Add custom…"), engine_buttons);
        auto* remove_engine = new QPushButton(QStringLiteral("Remove custom"), engine_buttons);
        eb_layout->addWidget(add_engine);
        eb_layout->addWidget(remove_engine);
        eb_layout->addStretch(1);
        connect(add_engine, &QPushButton::clicked, this, [this, settings, rebuild_engines, notify_changed] {
            bool ok = false;
            auto name = QInputDialog::getText(this, QStringLiteral("Add Search Engine"),
                QStringLiteral("Name:"), QLineEdit::Normal, QString(), &ok);
            if (!ok || name.trimmed().isEmpty())
                return;
            auto url = QInputDialog::getText(this, QStringLiteral("Add Search Engine"),
                QStringLiteral("Query URL (use %s for the search term):"),
                QLineEdit::Normal, QStringLiteral("https://example.com/search?q=%s"), &ok);
            if (!ok || !url.contains(QStringLiteral("%s")))
                return;
            if (settings->add_custom_search_engine(name.trimmed(), url.trimmed())) {
                settings->set_search_engine_name(name.trimmed());
                rebuild_engines();
                notify_changed();
            }
        });
        connect(remove_engine, &QPushButton::clicked, this,
            [this, engine_combo, settings, rebuild_engines, notify_changed] {
                auto name = engine_combo->currentText();
                if (!settings->is_custom_search_engine(name)) {
                    QMessageBox::information(this, QStringLiteral("Search Engines"),
                        QStringLiteral("Only custom search engines can be removed."));
                    return;
                }
                settings->remove_custom_search_engine(name);
                rebuild_engines();
                notify_changed();
            });
        form->addRow(QString(), engine_buttons);

        layout->addWidget(group);
    }

    // --- Content blocking and filters ---
    {
        auto* group = new QGroupBox(QStringLiteral("Content blocking and filters"), host);
        auto* vbox = new QVBoxLayout(group);

        auto* content_blocking = new QCheckBox(QStringLiteral("Block ads and trackers (content blocking)"), group);
        content_blocking->setChecked(settings->content_blocking_enabled());
        connect(content_blocking, &QCheckBox::toggled, this, [settings, notify_changed](bool on) {
            settings->set_content_blocking_enabled(on);
            notify_changed();
        });
        vbox->addWidget(content_blocking);

        auto* buttons = new QWidget(group);
        auto* row = new QHBoxLayout(buttons);
        row->setContentsMargins(0, 8, 0, 0);
        auto* exceptions = new QPushButton(QStringLiteral("Blocking exceptions…"), buttons);
        auto* edit_list = new QPushButton(QStringLiteral("Edit custom filter list…"), buttons);
        auto* reload_lists = new QPushButton(QStringLiteral("Reload filter lists"), buttons);
        row->addWidget(exceptions);
        row->addWidget(edit_list);
        row->addWidget(reload_lists);
        row->addStretch(1);
        vbox->addWidget(buttons);

        // Per-site content-blocking exceptions (was the menu's "Disable Blocking
        // for <host>" toggle; now a managed list of hosts where blocking is off).
        connect(exceptions, &QPushButton::clicked, this, [this, settings] {
            QDialog dialog(this);
            dialog.setWindowTitle(QStringLiteral("Content blocking exceptions"));
            dialog.resize(440, 360);
            auto* dl = new QVBoxLayout(&dialog);
            dl->addWidget(new QLabel(QStringLiteral("Sites where content blocking is turned off:"), &dialog));
            auto* list = new QListWidget(&dialog);
            list->setSelectionMode(QAbstractItemView::ExtendedSelection);
            dl->addWidget(list, 1);

            auto reload = [list, settings] {
                list->clear();
                for (auto const& host : settings->content_blocking_allowlist_hosts())
                    list->addItem(host);
                if (list->count() == 0)
                    list->addItem(new QListWidgetItem(QStringLiteral("No exceptions.")));
            };
            reload();

            auto* btns = new QHBoxLayout;
            auto* add_site = new QPushButton(QStringLiteral("Add site…"), &dialog);
            auto* remove_selected = new QPushButton(QStringLiteral("Remove selected"), &dialog);
            auto* close = new QPushButton(QStringLiteral("Close"), &dialog);
            btns->addWidget(add_site);
            btns->addWidget(remove_selected);
            btns->addStretch(1);
            btns->addWidget(close);
            dl->addLayout(btns);

            connect(add_site, &QPushButton::clicked, &dialog, [this, &dialog, settings, reload] {
                bool ok = false;
                auto host = QInputDialog::getText(&dialog, QStringLiteral("Add exception"),
                    QStringLiteral("Host (e.g. example.com):"), QLineEdit::Normal, QString(), &ok);
                host = host.trimmed().toLower();
                if (!ok || host.isEmpty())
                    return;
                settings->set_content_blocking_disabled_for_host(host, true);
                reload();
            });
            connect(remove_selected, &QPushButton::clicked, &dialog, [list, settings, reload] {
                for (auto* item : list->selectedItems()) {
                    auto host = item->text();
                    if (!host.isEmpty())
                        settings->set_content_blocking_disabled_for_host(host, false);
                }
                reload();
            });
            connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
            dialog.exec();
        });

        connect(edit_list, &QPushButton::clicked, this, [this] {
            auto blocklist_path = QString::fromStdString(std::string(servoq::user_blocklist_path()));
            QFileInfo info(blocklist_path);
            QDir().mkpath(info.absolutePath());
            if (!info.exists()) {
                QFile file(blocklist_path);
                if (file.open(QIODevice::WriteOnly))
                    file.close();
            }
            QDesktopServices::openUrl(QUrl::fromLocalFile(blocklist_path));
            QMessageBox::information(this, QStringLiteral("Custom filter list"),
                QStringLiteral("Place EasyList-compatible rules in:\n%1\n\nUse \"Reload filter lists\" to apply changes.")
                    .arg(blocklist_path));
        });

        connect(reload_lists, &QPushButton::clicked, this, [this] {
            bool ok = servoq::reload_blocklists();
            QMessageBox::information(this, QStringLiteral("Filter lists"),
                ok ? QStringLiteral("Filter lists reloaded.")
                   : QStringLiteral("Filter lists could not be reloaded."));
        });

        layout->addWidget(group);
    }

    // --- Privacy and data ---
    {
        auto* group = new QGroupBox(QStringLiteral("Privacy and data"), host);
        auto* vbox = new QVBoxLayout(group);

        auto* buttons = new QWidget(group);
        auto* btn_layout = new QHBoxLayout(buttons);
        btn_layout->setContentsMargins(0, 0, 0, 0);
        auto* clear_data = new QPushButton(QStringLiteral("Clear browsing data…"), buttons);
        auto* manage_sites = new QPushButton(QStringLiteral("Manage site data…"), buttons);
        auto* clear_perms = new QPushButton(QStringLiteral("Clear site permissions"), buttons);
        btn_layout->addWidget(clear_data);
        btn_layout->addWidget(manage_sites);
        btn_layout->addWidget(clear_perms);
        btn_layout->addStretch(1);

        connect(clear_data, &QPushButton::clicked, this, [this] {
            QDialog dialog(this);
            dialog.setWindowTitle(QStringLiteral("Clear browsing data"));
            auto* dl = new QVBoxLayout(&dialog);
            dl->addWidget(new QLabel(QStringLiteral("Select what to clear:"), &dialog));
            auto* history_box = new QCheckBox(QStringLiteral("Browsing history"), &dialog);
            auto* cookies_box = new QCheckBox(QStringLiteral("Cookies and site data"), &dialog);
            auto* cache_box = new QCheckBox(QStringLiteral("Cached files"), &dialog);
            history_box->setChecked(true);
            cookies_box->setChecked(true);
            cache_box->setChecked(true);
            dl->addWidget(history_box);
            dl->addWidget(cookies_box);
            dl->addWidget(cache_box);
            auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
            bb->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Clear data"));
            dl->addWidget(bb);
            connect(bb, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
            connect(bb, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            if (dialog.exec() != QDialog::Accepted)
                return;
            if (history_box->isChecked())
                HistoryStore::the()->clearHistory();
            if (cookies_box->isChecked())
                servoq::clear_all_cookies();
            if (cache_box->isChecked())
                servoq::clear_http_cache();
            QMessageBox::information(this, QStringLiteral("Clear browsing data"),
                QStringLiteral("The selected data has been cleared."));
        });

        connect(manage_sites, &QPushButton::clicked, this, [this] {
            QDialog dialog(this);
            dialog.setWindowTitle(QStringLiteral("Site data"));
            dialog.resize(480, 420);
            auto* dl = new QVBoxLayout(&dialog);
            dl->addWidget(new QLabel(QStringLiteral("Sites storing cookies or local data:"), &dialog));
            auto* list = new QListWidget(&dialog);
            list->setSelectionMode(QAbstractItemView::ExtendedSelection);
            dl->addWidget(list, 1);

            auto reload = [list] {
                list->clear();
                auto data = QString::fromStdString(std::string(servoq::list_site_data()));
                auto rows = data.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
                for (auto const& row : rows) {
                    auto cols = row.split(QLatin1Char('\t'));
                    if (cols.isEmpty())
                        continue;
                    auto site = cols[0];
                    int bits = cols.size() > 1 ? cols[1].toInt() : 0;
                    QStringList kinds;
                    if (bits & 1) kinds << QStringLiteral("cookies");
                    if (bits & 2) kinds << QStringLiteral("local storage");
                    if (bits & 4) kinds << QStringLiteral("session storage");
                    auto* item = new QListWidgetItem(
                        kinds.isEmpty() ? site : QStringLiteral("%1  —  %2").arg(site, kinds.join(QStringLiteral(", "))));
                    item->setData(Qt::UserRole, site);
                    list->addItem(item);
                }
                if (list->count() == 0)
                    list->addItem(new QListWidgetItem(QStringLiteral("No stored site data.")));
            };
            reload();

            auto* btns = new QHBoxLayout;
            auto* remove_selected = new QPushButton(QStringLiteral("Remove selected"), &dialog);
            auto* remove_all = new QPushButton(QStringLiteral("Remove all"), &dialog);
            auto* close = new QPushButton(QStringLiteral("Close"), &dialog);
            btns->addWidget(remove_selected);
            btns->addWidget(remove_all);
            btns->addStretch(1);
            btns->addWidget(close);
            dl->addLayout(btns);

            connect(remove_selected, &QPushButton::clicked, &dialog, [list, reload] {
                QStringList sites;
                for (auto* item : list->selectedItems()) {
                    auto site = item->data(Qt::UserRole).toString();
                    if (!site.isEmpty())
                        sites << site;
                }
                if (sites.isEmpty())
                    return;
                servoq::clear_site_data_for(sites.join(QLatin1Char('\n')).toStdString());
                reload();
            });
            connect(remove_all, &QPushButton::clicked, &dialog, [this, &dialog, reload] {
                if (QMessageBox::question(&dialog, QStringLiteral("Site data"),
                        QStringLiteral("Remove all cookies and site data for every site?"))
                    != QMessageBox::Yes)
                    return;
                servoq::clear_all_cookies();
                reload();
            });
            connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
            dialog.exec();
        });

        connect(clear_perms, &QPushButton::clicked, this, [this] {
            if (PermissionStore::the()->isEmpty()) {
                QMessageBox::information(this, QStringLiteral("Site permissions"),
                    QStringLiteral("No site permission decisions are stored."));
                return;
            }
            if (QMessageBox::question(this, QStringLiteral("Clear site permissions"),
                    QStringLiteral("Forget all Allow/Block permission decisions for all sites?"))
                == QMessageBox::Yes)
                PermissionStore::the()->clearAll();
        });

        vbox->addWidget(buttons);
        layout->addWidget(group);
    }

    // --- Advanced ---
    {
        auto* group = new QGroupBox(QStringLiteral("Advanced"), host);
        auto* form = new QFormLayout(group);
        add_check(form, group, QStringLiteral("Enable experimental web platform features"),
            settings->experimental_features_enabled(),
            [settings](bool on) { settings->set_experimental_features_enabled(on); });
        layout->addWidget(group);
    }

    layout->addStretch(1);
    m_content = host;
    m_root_layout->addWidget(m_content, 1);
}

// ---- History page (M4.1) ------------------------------------------------

void InternalPageView::buildHistoryPage()
{
    auto* container = new QWidget(this);
    auto* vbox = new QVBoxLayout(container);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    auto* header = new QWidget(container);
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(24, 18, 24, 12);

    auto* heading = new QLabel(QStringLiteral("History"), header);
    auto hf = heading->font();
    hf.setPointSizeF(hf.pointSizeF() * 1.6);
    hf.setBold(true);
    heading->setFont(hf);
    header_layout->addWidget(heading);
    header_layout->addStretch(1);

    m_history_search = new QLineEdit(header);
    m_history_search->setPlaceholderText(QStringLiteral("Search history"));
    m_history_search->setClearButtonEnabled(true);
    m_history_search->setMinimumWidth(260);
    header_layout->addWidget(m_history_search);

    auto* clear_all = new QPushButton(QStringLiteral("Clear browsing history"), header);
    header_layout->addWidget(clear_all);

    vbox->addWidget(header);

    m_history_list = new QListWidget(container);
    m_history_list->setObjectName(QStringLiteral("ServoQHistoryList"));
    m_history_list->setFrameShape(QFrame::NoFrame);
    m_history_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_history_list->setUniformItemSizes(false);
    vbox->addWidget(m_history_list, 1);

    connect(m_history_search, &QLineEdit::textChanged, this, [this](QString const& text) {
        refreshHistoryList(text);
    });
    connect(m_history_list, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        auto url = item ? item->data(Qt::UserRole).toString() : QString();
        if (!url.isEmpty() && onNavigate)
            onNavigate(url);
    });
    connect(m_history_list, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        auto url = item ? item->data(Qt::UserRole).toString() : QString();
        if (!url.isEmpty() && onNavigate)
            onNavigate(url);
    });
    connect(clear_all, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, QStringLiteral("Clear browsing history"),
                QStringLiteral("Delete all browsing history? This cannot be undone."))
            == QMessageBox::Yes)
            HistoryStore::the()->clearHistory();
    });
    // Live-refresh when history changes (deletion here, or visits elsewhere).
    connect(HistoryStore::the(), &HistoryStore::changed, this, [this] {
        if (m_history_list)
            refreshHistoryList(m_history_search ? m_history_search->text() : QString());
    });

    m_content = container;
    m_root_layout->addWidget(m_content, 1);
    refreshHistoryList(QString());
}

void InternalPageView::refreshHistoryList(QString const& filter)
{
    if (!m_history_list)
        return;
    m_history_list->clear();

    auto needle = filter.trimmed().toLower();
    auto const& entries = HistoryStore::the()->entries();
    QString current_section;
    auto today = QDate::currentDate();

    auto section_for = [&today](QDateTime const& dt) -> QString {
        auto date = dt.date();
        if (date == today)
            return QStringLiteral("Today");
        if (date == today.addDays(-1))
            return QStringLiteral("Yesterday");
        return date.toString(QStringLiteral("dddd, d MMMM yyyy"));
    };

    bool any = false;
    for (auto const& entry : entries) {
        if (!needle.isEmpty() && !entry.url_lower.contains(needle) && !entry.title_lower.contains(needle))
            continue;
        any = true;

        auto section = section_for(entry.visited_at);
        if (section != current_section) {
            current_section = section;
            auto* header_item = new QListWidgetItem(section, m_history_list);
            header_item->setFlags(Qt::NoItemFlags);
            auto f = header_item->font();
            f.setBold(true);
            header_item->setFont(f);
            header_item->setData(Qt::UserRole, QString());
        }

        auto* item = new QListWidgetItem(m_history_list);
        item->setData(Qt::UserRole, entry.url);

        auto* row = new QWidget(m_history_list);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(24, 4, 16, 4);
        row_layout->setSpacing(10);

        auto* icon_label = new QLabel(row);
        auto icon = FaviconStore::the()->iconForUrl(entry.url);
        if (icon.isNull())
            icon = create_chrome_icon(ChromeIcon::Globe, palette());
        icon_label->setPixmap(icon.pixmap(16, 16));
        icon_label->setFixedWidth(20);
        row_layout->addWidget(icon_label);

        auto* time_label = new QLabel(entry.visited_at.toString(QStringLiteral("HH:mm")), row);
        time_label->setFixedWidth(44);
        time_label->setEnabled(false);
        row_layout->addWidget(time_label);

        auto* text_label = new QLabel(row);
        auto title = entry.title.isEmpty() ? entry.url : entry.title;
        text_label->setText(QStringLiteral("%1   <span style='color:gray'>%2</span>")
                                .arg(title.toHtmlEscaped(), entry.url.toHtmlEscaped()));
        text_label->setTextFormat(Qt::RichText);
        row_layout->addWidget(text_label, 1);

        auto* delete_btn = new QToolButton(row);
        delete_btn->setText(QStringLiteral("✕"));
        delete_btn->setToolTip(QStringLiteral("Remove from history"));
        delete_btn->setAutoRaise(true);
        delete_btn->setCursor(Qt::PointingHandCursor);
        auto url = entry.url;
        connect(delete_btn, &QToolButton::clicked, this, [url] {
            HistoryStore::the()->removeUrl(url);
        });
        row_layout->addWidget(delete_btn);

        item->setSizeHint(row->sizeHint());
        m_history_list->setItemWidget(item, row);
    }

    if (!any) {
        auto* empty = new QListWidgetItem(
            needle.isEmpty() ? QStringLiteral("    No browsing history yet.")
                             : QStringLiteral("    No history matches your search."),
            m_history_list);
        empty->setFlags(Qt::NoItemFlags);
        empty->setData(Qt::UserRole, QString());
    }
}

// ---- Downloads page (placeholder; full panel is M5.1) -------------------

void InternalPageView::buildDownloadsPage()
{
    QVBoxLayout* layout = nullptr;
    auto* host = makeScrollHost(layout);

    auto* heading = new QLabel(QStringLiteral("Downloads"), host);
    auto hf = heading->font();
    hf.setPointSizeF(hf.pointSizeF() * 1.8);
    hf.setBold(true);
    heading->setFont(hf);
    layout->addWidget(heading);

    auto* note = new QLabel(
        QStringLiteral("No downloads yet.\n\nDownload management is not available in this build — "
                       "Servo does not expose a download API."),
        host);
    note->setWordWrap(true);
    note->setEnabled(false);
    layout->addWidget(note);
    layout->addStretch(1);

    m_content = host;
    m_root_layout->addWidget(m_content, 1);
}

// ---- Debug page (M4.4): shell state + console panel ---------------------

void InternalPageView::buildDebugPage()
{
    auto* container = new QWidget(this);
    auto* vbox = new QVBoxLayout(container);
    vbox->setContentsMargins(24, 18, 24, 18);
    vbox->setSpacing(14);

    auto* heading = new QLabel(QStringLiteral("Debug"), container);
    auto hf = heading->font();
    hf.setPointSizeF(hf.pointSizeF() * 1.6);
    hf.setBold(true);
    heading->setFont(hf);
    vbox->addWidget(heading);

    // Shell state.
    auto* settings = Settings::the();
    QStringList rows;
    rows << QStringLiteral("ServoQ — Servo engine browser shell");
    rows << QStringLiteral("Profile directory: %1")
                .arg(QString::fromStdString(std::string(servoq::servo_profile_data_dir())));
    rows << QStringLiteral("Settings directory: %1").arg(settings->directory());
    rows << QStringLiteral("User filter list: %1")
                .arg(QString::fromStdString(std::string(servoq::user_blocklist_path())));
    rows << QStringLiteral("Content blocking: %1")
                .arg(settings->content_blocking_enabled() ? QStringLiteral("on") : QStringLiteral("off"));
    rows << QStringLiteral("Experimental features: %1")
                .arg(settings->experimental_features_enabled() ? QStringLiteral("on") : QStringLiteral("off"));
    rows << QStringLiteral("Default search engine: %1").arg(settings->search_engine_name());
    rows << QStringLiteral("History entries (indexed): %1").arg(HistoryStore::the()->entries().size());

    auto* state_group = new QGroupBox(QStringLiteral("Shell state"), container);
    auto* state_layout = new QVBoxLayout(state_group);
    auto* state_label = new QLabel(rows.join(QLatin1Char('\n')), state_group);
    state_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    state_label->setWordWrap(true);
    state_layout->addWidget(state_label);
    vbox->addWidget(state_group);

    // Console panel.
    auto* console_group = new QGroupBox(QStringLiteral("Console"), container);
    auto* console_layout = new QVBoxLayout(console_group);
    auto* console_controls = new QHBoxLayout;
    auto* status = new QLabel(QStringLiteral("Capturing page console output…"), console_group);
    status->setEnabled(false);
    auto* clear_console = new QPushButton(QStringLiteral("Clear"), console_group);
    console_controls->addWidget(status);
    console_controls->addStretch(1);
    console_controls->addWidget(clear_console);
    console_layout->addLayout(console_controls);

    auto* console_view = new QPlainTextEdit(console_group);
    console_view->setReadOnly(true);
    console_view->setMaximumBlockCount(kConsoleRingSize + 50);
    QFont mono(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    console_view->setFont(mono);
    console_layout->addWidget(console_view, 1);
    vbox->addWidget(console_group, 1);

    auto format_message = [](ConsoleLog::Message const& m) {
        static char const* const names[] = { "LOG", "DEBUG", "INFO", "WARN", "ERROR", "TRACE" };
        auto level = (m.level >= 0 && m.level < 6) ? names[m.level] : "LOG";
        return QStringLiteral("[%1] [tab %2] %3: %4").arg(m.time).arg(m.tab_id).arg(QString::fromLatin1(level), m.text);
    };
    for (auto const& m : ConsoleLog::the()->messages())
        console_view->appendPlainText(format_message(m));

    connect(ConsoleLog::the(), &ConsoleLog::appended, console_view,
        [console_view, format_message](ConsoleLog::Message const& m) {
            console_view->appendPlainText(format_message(m));
        });
    connect(ConsoleLog::the(), &ConsoleLog::cleared, console_view, [console_view] {
        console_view->clear();
    });
    connect(clear_console, &QPushButton::clicked, this, [] {
        ConsoleLog::the()->clear();
    });

    m_content = container;
    m_root_layout->addWidget(m_content, 1);
    setConsoleConsuming(true);
}

void InternalPageView::setConsoleConsuming(bool consuming)
{
    if (consuming == m_console_consuming)
        return;
    m_console_consuming = consuming;
    if (consuming)
        ConsoleLog::the()->addConsumer();
    else
        ConsoleLog::the()->removeConsumer();
}

}

// ---- Rust -> C++ callback (servoq::notify_console_message) ---------------

namespace servoq {

void notify_console_message(::std::int32_t tab_id, ::std::int32_t level, ::rust::Str message)
{
    ServoQ::ConsoleLog::the()->append(static_cast<int>(tab_id), static_cast<int>(level),
        QString::fromUtf8(message.data(), static_cast<int>(message.size())));
}

}
