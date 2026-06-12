/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2023, Srikavin Ramkumar <me@srikavin.me>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Ladybird:
 *   UI/Qt/BookmarksBar.cpp
 *   Libraries/LibWebView/BookmarkStore.cpp
 *   Libraries/LibWeb/HTML/HTMLLinkElement.cpp
 *   Libraries/LibWebView/ViewImplementation.cpp
 *   UI/Qt/Menu.cpp
 */
#include "BookmarksBar.h"
#include "BookmarkStore.h"
#include "FaviconStore.h"
#include "ChromeStyle.h"
#include "Icon.h"
#include "Settings.h"

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QComboBox>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QStyle>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QToolButton>
#include <QUrl>
#include <QWidget>

namespace ServoQ {

static constexpr int BookmarkButtonMaxWidth         = 150;
static constexpr int BookmarkButtonIconSize         = 16;
static constexpr int BookmarkButtonMinHeight        = 24;
static constexpr int BookmarkButtonVerticalPadding  = 8;
static constexpr int BookmarkButtonHorizontalPadding = 7;
static constexpr int BookmarkButtonIconTextSpacing  = 6;
static constexpr int BookmarkButtonTextElisionPadding = 2;

static constexpr char const* BookmarkItemProperty = "bookmark_item";
static constexpr char const* BookmarkMimeType = "application/x-servoq-bookmark";

struct BookmarkDragPayload {
    QString type;
    QString id;
    QString source_folder_id;
};

static bool debug_enabled()
{
    static bool const v = qEnvironmentVariableIsSet("SERVOQ_DEBUG");
    return v;
}

static void debug_bookmark_drag(QString const& detail)
{
    if (debug_enabled())
        qInfo().nospace() << "SERVOQ_DEBUG " << detail;
}

static QByteArray encode_bookmark_drag_payload(BookmarkDragPayload const& payload)
{
    return QStringLiteral("type=%1\nid=%2\nsource_folder_id=%3")
        .arg(payload.type, payload.id, payload.source_folder_id)
        .toUtf8();
}

static BookmarkDragPayload parse_bookmark_drag_payload(QMimeData const* mime_data)
{
    BookmarkDragPayload payload;
    if (!mime_data || !mime_data->hasFormat(BookmarkMimeType))
        return payload;

    auto data = QString::fromUtf8(mime_data->data(BookmarkMimeType));
    if (data.contains(QLatin1Char('\n'))) {
        for (auto const& line : data.split(QLatin1Char('\n'))) {
            auto equals = line.indexOf(QLatin1Char('='));
            if (equals < 0)
                continue;
            auto key = line.left(equals);
            auto value = line.mid(equals + 1);
            if (key == QStringLiteral("type"))
                payload.type = value;
            else if (key == QStringLiteral("id"))
                payload.id = value;
            else if (key == QStringLiteral("source_folder_id"))
                payload.source_folder_id = value;
        }
        return payload;
    }

    // Backward-compatible parser for the previous root-only "type:id" payload.
    auto colon = data.indexOf(QLatin1Char(':'));
    if (colon >= 0) {
        payload.type = data.left(colon);
        payload.id = data.mid(colon + 1);
    }
    return payload;
}

static QIcon bookmark_icon_for_url(QString const& url, QPalette const& palette)
{
    auto icon = FaviconStore::the()->iconForUrl(url);
    if (!icon.isNull())
        return icon;
    return create_chrome_icon(ChromeIcon::Globe, palette);
}

static bool bookmarksReferenceHost(QString const& host)
{
    auto matches = [&host](BookmarkItem const& bookmark) {
        return QUrl(bookmark.url).host().toLower() == host;
    };
    for (auto const& bookmark : BookmarkStore::the()->rootBookmarks()) {
        if (matches(bookmark))
            return true;
    }
    for (auto const& folder : BookmarkStore::the()->folders()) {
        for (auto const& bookmark : folder.items) {
            if (matches(bookmark))
                return true;
        }
    }
    return false;
}

// Custom paint replicates reference paint_bookmark_button
static void paint_bookmark_button(QToolButton& button)
{
    QStylePainter painter(&button);

    QStyleOptionToolButton option;
    option.initFrom(&button);
    option.rect = button.rect();
    option.icon = button.icon();
    option.iconSize = button.iconSize();
    option.text = button.text();
    option.toolButtonStyle = button.toolButtonStyle();
    option.arrowType = button.arrowType();
    option.subControls = QStyle::SC_ToolButton;
    if (button.autoRaise())
        option.state |= QStyle::State_AutoRaise;
    if (button.menu()) {
        option.features |= QStyleOptionToolButton::HasMenu;
        if (button.popupMode() == QToolButton::InstantPopup)
            option.features |= QStyleOptionToolButton::Menu;
        else if (button.popupMode() == QToolButton::MenuButtonPopup) {
            option.features |= QStyleOptionToolButton::MenuButtonPopup;
            option.subControls |= QStyle::SC_ToolButtonMenu;
        }
    }
    if (button.isDown())
        option.state |= QStyle::State_Sunken;

    // Menu indicator width for folders
    int menu_indicator_width = button.menu()
        ? button.style()->pixelMetric(QStyle::PM_MenuButtonIndicator, &option, &button)
        : 0;

    int icon_width = button.icon().isNull() ? 0 : button.iconSize().width();
    int icon_text_spacing = (icon_width > 0 && !option.text.isEmpty()) ? BookmarkButtonIconTextSpacing : 0;

    QRect content_rect = button.rect();
    content_rect.adjust(BookmarkButtonHorizontalPadding, 0, -BookmarkButtonHorizontalPadding, 0);
    content_rect.adjust(0, 0, -menu_indicator_width, 0);

    int text_left = content_rect.left() + icon_width + icon_text_spacing;
    int available_text_width = qMax(content_rect.right() - text_left + 1, 0);
    QRect text_rect { text_left, content_rect.top(), available_text_width, content_rect.height() };

    QRect icon_rect;
    if (icon_width > 0) {
        auto icon_size = option.iconSize;
        icon_rect = QRect {
            content_rect.left(),
            content_rect.top() + ((content_rect.height() - icon_size.height()) / 2),
            icon_size.width(), icon_size.height()
        };
    }

    // Draw frame only (suppress icon/text in drawComplexControl)
    auto frame_option = option;
    frame_option.icon = {};
    frame_option.text.clear();
    painter.drawComplexControl(QStyle::CC_ToolButton, frame_option);

    // Draw icon
    if (icon_width > 0) {
        auto mode = button.isEnabled() ? QIcon::Normal : QIcon::Disabled;
        if (button.isEnabled() && (option.state & QStyle::State_MouseOver))
            mode = QIcon::Active;
        option.icon.paint(&painter, icon_rect, Qt::AlignCenter, mode, button.isChecked() ? QIcon::On : QIcon::Off);
    }

    // Elide and draw text
    auto elided = button.fontMetrics().elidedText(option.text, Qt::ElideRight, available_text_width);
    button.style()->drawItemText(
        &painter, text_rect,
        Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
        option.palette, button.isEnabled(), elided, QPalette::ButtonText);
}

static void set_bookmark_button_size(QToolButton* button, QString const& title)
{
    button->setProperty(BookmarkItemProperty, true);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->ensurePolished();

    QStyleOptionToolButton option;
    option.initFrom(button);

    int menu_indicator_width = button->menu()
        ? button->style()->pixelMetric(QStyle::PM_MenuButtonIndicator, &option, button)
        : 0;
    int icon_width = button->icon().isNull() ? 0 : button->iconSize().width();
    int icon_text_spacing = (icon_width > 0 && !title.isEmpty()) ? BookmarkButtonIconTextSpacing : 0;

    int bookmark_height = qMax(BookmarkButtonMinHeight,
        qMax(button->fontMetrics().height(), button->iconSize().height()) + BookmarkButtonVerticalPadding);

    QRect max_rect { 0, 0, BookmarkButtonMaxWidth, bookmark_height };
    QRect content_rect = max_rect;
    content_rect.adjust(BookmarkButtonHorizontalPadding, 0, -BookmarkButtonHorizontalPadding, 0);
    content_rect.adjust(0, 0, -menu_indicator_width, 0);
    int available_text_width = qMax(content_rect.right() - (content_rect.left() + icon_width + icon_text_spacing) + 1, 0);

    auto text = button->fontMetrics().elidedText(title, Qt::ElideRight,
        qMax(available_text_width - BookmarkButtonTextElisionPadding, 0));
    button->setText(text);

    int preferred_width = (BookmarkButtonHorizontalPadding * 2)
        + icon_width + icon_text_spacing
        + button->fontMetrics().horizontalAdvance(text)
        + BookmarkButtonTextElisionPadding
        + menu_indicator_width;
    preferred_width = qMin(preferred_width, BookmarkButtonMaxWidth);

    button->setFixedWidth(preferred_width);
    button->setMaximumWidth(BookmarkButtonMaxWidth);
    button->setFixedHeight(bookmark_height);
}

BookmarksBar::BookmarksBar(QWidget* parent)
    : QToolBar(parent)
    , m_drop_indicator(new QWidget(this))
{
    setObjectName("LadybirdBookmarksBar");
    setMovable(false);
    setFloatable(false);
    setAcceptDrops(true);
    setIconSize({ BookmarkButtonIconSize, BookmarkButtonIconSize });
    updateChromeStyle();
    m_drop_indicator->setObjectName("BookmarkDropIndicator");
    m_drop_indicator->setFixedWidth(2);
    m_drop_indicator->setStyleSheet(QStringLiteral("background: palette(highlight); border-radius: 1px;"));
    m_drop_indicator->hide();

    installEventFilter(this);

    // Migrate from legacy Settings if BookmarkStore is empty
    if (BookmarkStore::the()->rootBookmarks().isEmpty() && BookmarkStore::the()->folders().isEmpty()) {
        auto legacy = Settings::the()->bookmarks();
        if (!legacy.isEmpty())
            BookmarkStore::the()->importFlatList(legacy);
    }

    connect(BookmarkStore::the(), &BookmarkStore::changed, this, &BookmarksBar::rebuild);
    // Bookmark icons come from the favicon DB now; refresh when a bookmarked
    // site's icon is (re)fetched. Icons for non-bookmarked hosts arrive on
    // every page load — ignore those, a full bar rebuild per load is wasteful.
    connect(FaviconStore::the(), &FaviconStore::iconsChanged, this, [this](QString const& host) {
        if (host.isEmpty() || bookmarksReferenceHost(host))
            rebuild();
    });

    rebuild();
}

int BookmarksBar::insertionIndicatorX(QPoint const& drop_pos) const
{
    QList<QAction*> candidates;
    for (auto* action : actions()) {
        if (!action->property("bookmark_type").toString().isEmpty())
            candidates.append(action);
    }
    if (candidates.isEmpty())
        return 4;

    QAction* target = const_cast<BookmarksBar*>(this)->actionAt(drop_pos);
    if (!target || target->property("bookmark_type").toString().isEmpty()) {
        if (auto* last = const_cast<BookmarksBar*>(this)->widgetForAction(candidates.last()))
            return last->geometry().right() + 4;
        return width() - 4;
    }

    if (auto* widget = const_cast<BookmarksBar*>(this)->widgetForAction(target)) {
        auto rect = widget->geometry();
        return drop_pos.x() > rect.center().x() ? rect.right() + 4 : rect.left() - 3;
    }
    return width() - 4;
}

void BookmarksBar::hideDropIndicator()
{
    if (m_drop_indicator)
        m_drop_indicator->hide();
}

void BookmarksBar::rebuild()
{
    // Close any open menus before clearing
    for (auto* action : actions()) {
        if (auto* menu = action->menu())
            menu->close();
    }

    clear();

    auto add_bookmark_button = [this](BookmarkItem const& item) {
        auto* action = new QAction(this);
        action->setIcon(bookmark_icon_for_url(item.url, palette()));
        action->setToolTip(item.url);
        action->setProperty("bookmark_id", item.id);
        action->setProperty("bookmark_type", QStringLiteral("bookmark"));
        addAction(action);

        if (auto* button = qobject_cast<QToolButton*>(widgetForAction(action))) {
            button->setAutoRaise(true);
            set_bookmark_button_size(button, item.title);
            button->installEventFilter(this);
            connect(action, &QAction::triggered, this, [this, item] {
                if (m_open_url_callback)
                    m_open_url_callback(item.url);
            });
        }
    };

    auto add_folder_button = [this](BookmarkFolder const& folder) {
        auto* submenu = new QMenu(folder.title, this);
        submenu->setProperty("folder_id", folder.id);
        submenu->setAcceptDrops(true);

        for (auto const& child : folder.items) {
            auto* child_action = submenu->addAction(
                bookmark_icon_for_url(child.url, palette()), child.title);
            child_action->setToolTip(child.url);
            child_action->setProperty("bookmark_id", child.id);
            child_action->setProperty("bookmark_type", QStringLiteral("bookmark"));
            child_action->setProperty("folder_id", folder.id);
            connect(child_action, &QAction::triggered, this, [this, child] {
                if (m_open_url_callback)
                    m_open_url_callback(child.url);
            });
        }

        // Add "New Bookmark in Folder" to submenu separator + action
        submenu->addSeparator();
        auto* add_to_folder = submenu->addAction(QStringLiteral("Add Bookmark Here…"));
        connect(add_to_folder, &QAction::triggered, this, [this, folder] {
            showAddBookmarkDialog(folder.id, {}, {}, {});
        });

        // Install event filter on submenu for middle-click / right-click
        submenu->installEventFilter(this);

        auto* folder_action = new QAction(folder.title, this);
        folder_action->setIcon(create_chrome_icon(ChromeIcon::Folder, palette()));
        folder_action->setMenu(submenu);
        folder_action->setProperty("folder_id", folder.id);
        folder_action->setProperty("bookmark_type", QStringLiteral("folder"));
        addAction(folder_action);

        if (auto* button = qobject_cast<QToolButton*>(widgetForAction(folder_action))) {
            button->setPopupMode(QToolButton::InstantPopup);
            button->setAutoRaise(true);
            set_bookmark_button_size(button, folder.title);
            button->installEventFilter(this);
        }
    };

    // Root-level bookmarks and folders share one ordered list, matching Ladybird's
    // WebView::BookmarkStore::root_items() model.
    for (auto const& entry : BookmarkStore::the()->rootItems()) {
        if (entry.type == QStringLiteral("bookmark")) {
            if (auto const* bookmark = BookmarkStore::the()->findRootBookmark(entry.id))
                add_bookmark_button(*bookmark);
        } else if (entry.type == QStringLiteral("folder")) {
            if (auto const* folder = BookmarkStore::the()->findFolder(entry.id))
                add_folder_button(*folder);
        }
    }
}

bool BookmarksBar::event(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange)
        updateChromeStyle();
    return QToolBar::event(event);
}

void BookmarksBar::updateChromeStyle()
{
    if (m_is_updating_chrome_style)
        return;
    m_is_updating_chrome_style = true;
    // Clear before setting: QToolBarLayout only re-reads its margins/spacing on a
    // real style change, and a sheet applied before the window's first polish (the
    // initial tab's bar is built pre-show) leaves the layout on base-style metrics
    // (margin 6/spacing 0 instead of the QSS margin 4/spacing 3) — making bookmark
    // spacing differ between tabs. Clearing guarantees the second set is a real
    // change that forces the metrics to be recomputed.
    setStyleSheet(QString());
    setStyleSheet(ChromeStyle::bookmarks_bar_style_sheet(palette()));
    m_is_updating_chrome_style = false;
}

bool BookmarksBar::eventFilter(QObject* object, QEvent* event)
{
    // Custom paint for bookmark/folder buttons
    if (event->type() == QEvent::Paint) {
        if (auto* button = qobject_cast<QToolButton*>(object);
            button && button->property(BookmarkItemProperty).toBool()) {
            paint_bookmark_button(*button);
            return true;
        }
    }

    if (event->type() == QEvent::MouseButtonRelease) {
        auto& mouse_event = static_cast<QMouseEvent&>(*event);
        if (mouse_event.button() == Qt::LeftButton && !m_drag_source_id.isEmpty()) {
            if (auto* button = qobject_cast<QToolButton*>(object)) {
                auto* action = button->defaultAction();
                if (action && action->property("bookmark_type").toString() == QStringLiteral("folder")
                    && action->property("folder_id").toString() == m_drag_source_id) {
                    auto delta = (mouse_event.position().toPoint() - m_drag_start_pos).manhattanLength();
                    auto folder_id = m_drag_source_id;
                    m_drag_source_id.clear();
                    m_drag_source_type.clear();
                    m_drag_source_folder_id.clear();
                    if (delta < QApplication::startDragDistance())
                        button->showMenu();
                    return true;
                }
            }
        }
        m_drag_source_id.clear();
        m_drag_source_type.clear();
        m_drag_source_folder_id.clear();
    }

    if (event->type() == QEvent::MouseMove) {
        auto& mouse_event = static_cast<QMouseEvent&>(*event);
        if ((mouse_event.buttons() & Qt::LeftButton) && !m_drag_source_id.isEmpty()) {
            auto delta = (mouse_event.position().toPoint() - m_drag_start_pos).manhattanLength();
            if (delta >= QApplication::startDragDistance()) {
                QString id = m_drag_source_id;
                QString type = m_drag_source_type;
                QString source_folder_id = m_drag_source_folder_id;
                m_drag_source_id.clear();
                m_drag_source_type.clear();
                m_drag_source_folder_id.clear();
                auto* drag = new QDrag(this);
                auto* mime = new QMimeData();
                mime->setData(BookmarkMimeType, encode_bookmark_drag_payload({ type, id, source_folder_id }));
                drag->setMimeData(mime);
                if (auto* button = qobject_cast<QToolButton*>(object))
                    drag->setPixmap(button->grab());
                debug_bookmark_drag(QStringLiteral("bookmark_drag_start type=%1 id=%2 source=%3")
                    .arg(type, id, source_folder_id.isEmpty() ? QStringLiteral("root") : QStringLiteral("folder:%1").arg(source_folder_id)));
                drag->exec(Qt::MoveAction);
                return true;
            }
        }
    }

    if (event->type() == QEvent::MouseButtonPress) {
        auto& mouse_event = static_cast<QMouseEvent&>(*event);

        if (mouse_event.button() == Qt::LeftButton) {
            if (auto* button = qobject_cast<QToolButton*>(object)) {
                auto* action = button->defaultAction();
                if (action) {
                    auto bm_type = action->property("bookmark_type").toString();
                    if (bm_type == QStringLiteral("bookmark")) {
                        m_drag_source_id = action->property("bookmark_id").toString();
                        m_drag_source_type = QStringLiteral("bookmark");
                        m_drag_source_folder_id.clear();
                    } else if (bm_type == QStringLiteral("folder")) {
                        m_drag_source_id = action->property("folder_id").toString();
                        m_drag_source_type = QStringLiteral("folder");
                        m_drag_source_folder_id.clear();
                        m_drag_start_pos = mouse_event.position().toPoint();
                        return true;
                    }
                    m_drag_start_pos = mouse_event.position().toPoint();
                }
            } else if (auto* menu = qobject_cast<QMenu*>(object)) {
                if (auto* action = menu->actionAt(mouse_event.pos())) {
                    if (action->property("bookmark_type").toString() == QStringLiteral("bookmark")) {
                        m_drag_source_id = action->property("bookmark_id").toString();
                        m_drag_source_type = QStringLiteral("bookmark");
                        m_drag_source_folder_id = action->property("folder_id").toString();
                        m_drag_start_pos = mouse_event.position().toPoint();
                    }
                }
            }
        }

        if (mouse_event.button() == Qt::MiddleButton) {
            // Middle-click opens in new tab
            QString url;
            if (auto* button = qobject_cast<QToolButton*>(object)) {
                url = button->defaultAction() ? button->defaultAction()->toolTip() : QString();
            } else if (auto* menu = qobject_cast<QMenu*>(object)) {
                if (auto* action = menu->actionAt(mouse_event.pos()))
                    url = action->toolTip();
            }
            if (!url.isEmpty() && m_open_url_in_new_tab_callback) {
                m_open_url_in_new_tab_callback(url);
                return true;
            }
        }

        if (mouse_event.button() == Qt::RightButton) {
            // Right-click context menu
            if (auto* button = qobject_cast<QToolButton*>(object)) {
                auto* action = button->defaultAction();
                if (!action) return QToolBar::eventFilter(object, event);

                auto bm_type = action->property("bookmark_type").toString();
                auto bm_id = action->property("bookmark_id").toString();
                auto folder_id = action->property("folder_id").toString();

                QMenu ctx(this);
                if (bm_type == QStringLiteral("bookmark")) {
                    ctx.addAction(QStringLiteral("Edit Bookmark…"), this, [this, bm_id] {
                        showEditBookmarkDialog(bm_id);
                    });
                    ctx.addAction(QStringLiteral("Delete Bookmark"), this, [bm_id] {
                        BookmarkStore::the()->removeBookmark(bm_id);
                    });
                } else if (bm_type == QStringLiteral("folder")) {
                    ctx.addAction(QStringLiteral("Add Bookmark to Folder…"), this, [this, folder_id] {
                        showAddBookmarkDialog(folder_id, {}, {}, {});
                    });
                    ctx.addAction(QStringLiteral("Rename Folder…"), this, [this, folder_id] {
                        showEditFolderDialog(folder_id);
                    });
                    ctx.addAction(QStringLiteral("Delete Folder"), this, [folder_id] {
                        BookmarkStore::the()->removeFolder(folder_id);
                    });
                }

                if (!ctx.isEmpty())
                    ctx.exec(mouse_event.globalPosition().toPoint());
                return true;
            }

            // Right-click on empty bar area
            if (object == this) {
                QMenu ctx(this);
                ctx.addAction(QStringLiteral("New Folder…"), this, [this] {
                    showNewFolderDialog();
                });
                ctx.exec(mouse_event.globalPosition().toPoint());
                return true;
            }
        }
    }

    if (auto* menu = qobject_cast<QMenu*>(object)) {
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
            auto* drag_event = static_cast<QDragMoveEvent*>(event);
            auto payload = parse_bookmark_drag_payload(drag_event->mimeData());
            if (payload.type == QStringLiteral("bookmark") && !menu->property("folder_id").toString().isEmpty()) {
                drag_event->acceptProposedAction();
                return true;
            }
        }
        if (event->type() == QEvent::Drop) {
            auto* drop_event = static_cast<QDropEvent*>(event);
            auto payload = parse_bookmark_drag_payload(drop_event->mimeData());
            auto folder_id = menu->property("folder_id").toString();
            if (payload.type == QStringLiteral("bookmark") && !payload.id.isEmpty() && !folder_id.isEmpty()) {
                auto moved = BookmarkStore::the()->moveBookmarkToFolder(payload.id, folder_id);
                debug_bookmark_drag(QStringLiteral("bookmark_drop id=%1 from=%2 to=folder folder_id=%3 index=-1 moved=%4")
                    .arg(payload.id,
                        payload.source_folder_id.isEmpty() ? QStringLiteral("root") : QStringLiteral("folder:%1").arg(payload.source_folder_id),
                        folder_id,
                        moved ? QStringLiteral("true") : QStringLiteral("false")));
                drop_event->acceptProposedAction();
                return true;
            }
        }
    }

    return QToolBar::eventFilter(object, event);
}

void BookmarksBar::showAddBookmarkDialog(QString const& title, QString const& url, QString const& favicon_base64_png)
{
    showAddBookmarkDialog({}, title, url, favicon_base64_png);
}

void BookmarksBar::showAddBookmarkDialog(QString const& folder_id,
                                         QString const& prefill_title,
                                         QString const& prefill_url,
                                         QString const& favicon_base64_png)
{
    auto* dialog = new QDialog(this);
    dialog->setWindowTitle(QStringLiteral("Add Bookmark"));
    auto* layout = new QFormLayout(dialog);

    auto* title_edit = new QLineEdit(prefill_title, dialog);
    auto* url_edit   = new QLineEdit(prefill_url,   dialog);

    // Folder selector
    auto* folder_combo = new QComboBox(dialog);
    folder_combo->addItem(QStringLiteral("Bookmarks Bar"), QStringLiteral(""));
    for (auto const& f : BookmarkStore::the()->folders())
        folder_combo->addItem(f.title, f.id);
    if (!folder_id.isEmpty()) {
        auto idx = folder_combo->findData(folder_id);
        if (idx >= 0) folder_combo->setCurrentIndex(idx);
    }

    layout->addRow(QStringLiteral("Title:"),  title_edit);
    layout->addRow(QStringLiteral("URL:"),    url_edit);
    layout->addRow(QStringLiteral("Folder:"), folder_combo);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);

    connect(dialog, &QDialog::accepted, this, [title_edit, url_edit, folder_combo, favicon_base64_png] {
        auto selected_folder = folder_combo->currentData().toString();
        BookmarkStore::the()->addBookmark(title_edit->text(), url_edit->text(), selected_folder, favicon_base64_png);
    });
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

void BookmarksBar::showEditBookmarkDialog(QString const& id)
{
    // Find the bookmark
    QString title, url;
    for (auto const& bm : BookmarkStore::the()->rootBookmarks())
        if (bm.id == id) { title = bm.title; url = bm.url; break; }
    for (auto const& folder : BookmarkStore::the()->folders())
        for (auto const& bm : folder.items)
            if (bm.id == id) { title = bm.title; url = bm.url; break; }

    auto* dialog = new QDialog(this);
    dialog->setWindowTitle(QStringLiteral("Edit Bookmark"));
    auto* layout = new QFormLayout(dialog);
    auto* title_edit = new QLineEdit(title, dialog);
    auto* url_edit   = new QLineEdit(url,   dialog);
    layout->addRow(QStringLiteral("Title:"), title_edit);
    layout->addRow(QStringLiteral("URL:"),   url_edit);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);

    connect(dialog, &QDialog::accepted, this, [dialog, id, title_edit, url_edit] {
        BookmarkStore::the()->editBookmark(id, title_edit->text(), url_edit->text());
    });
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

void BookmarksBar::showEditFolderDialog(QString const& id)
{
    QString current_title;
    for (auto const& f : BookmarkStore::the()->folders())
        if (f.id == id) { current_title = f.title; break; }

    auto* dlg = new QInputDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(QStringLiteral("Rename Folder"));
    dlg->setLabelText(QStringLiteral("Folder name:"));
    dlg->setTextValue(current_title);
    connect(dlg, &QInputDialog::textValueSelected, this, [this, id](QString const& new_title) {
        if (!new_title.isEmpty())
            BookmarkStore::the()->editFolder(id, new_title);
    });
    dlg->open();
}

void BookmarksBar::showNewFolderDialog()
{
    auto* dlg = new QInputDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(QStringLiteral("New Folder"));
    dlg->setLabelText(QStringLiteral("Folder name:"));
    connect(dlg, &QInputDialog::textValueSelected, this, [this](QString const& title) {
        if (!title.isEmpty())
            BookmarkStore::the()->addFolder(title);
    });
    dlg->open();
}

void BookmarksBar::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasFormat(BookmarkMimeType))
        event->acceptProposedAction();
}

void BookmarksBar::dragLeaveEvent(QDragLeaveEvent* event)
{
    hideDropIndicator();
    QToolBar::dragLeaveEvent(event);
}

void BookmarksBar::dragMoveEvent(QDragMoveEvent* event)
{
    auto payload = parse_bookmark_drag_payload(event->mimeData());
    if (payload.id.isEmpty())
        return;

    auto target = actionAt(event->position().toPoint());
    if (payload.type == QStringLiteral("bookmark")
        && target
        && target->property("bookmark_type").toString() == QStringLiteral("folder")) {
        auto* widget = widgetForAction(target);
        if (widget) {
            auto rect = widget->geometry();
            auto edge_margin = qMin(12, qMax(1, rect.width() / 4));
            if (event->position().toPoint().x() > rect.left() + edge_margin
                && event->position().toPoint().x() < rect.right() - edge_margin) {
                hideDropIndicator();
                event->acceptProposedAction();
                return;
            }
        }
    }

    auto x = insertionIndicatorX(event->position().toPoint());
    m_drop_indicator->setGeometry(x, 4, 2, qMax(1, height() - 8));
    m_drop_indicator->raise();
    m_drop_indicator->show();
    event->acceptProposedAction();
}

void BookmarksBar::dropEvent(QDropEvent* event)
{
    auto payload = parse_bookmark_drag_payload(event->mimeData());
    if (payload.id.isEmpty())
        return;

    auto drop_pos = event->position().toPoint();
    hideDropIndicator();

    auto compute_target_index = [this, &drop_pos]() -> int {
        QAction* target = actionAt(drop_pos);
        QList<QAction*> root_actions;
        for (auto* action : actions()) {
            if (!action->property("bookmark_type").toString().isEmpty())
                root_actions.append(action);
        }

        if (!target) return root_actions.size(); // append to end
        for (int i = 0; i < root_actions.size(); ++i) {
            if (root_actions[i] == target) {
                auto* w = widgetForAction(target);
                if (w && drop_pos.x() > w->geometry().center().x())
                    return i + 1;
                return i;
            }
        }
        return root_actions.size();
    };

    auto* target = actionAt(drop_pos);
    if (payload.type == QStringLiteral("bookmark")
        && target
        && target->property("bookmark_type").toString() == QStringLiteral("folder")) {
        auto* widget = widgetForAction(target);
        bool drop_into_folder = true;
        if (widget) {
            auto rect = widget->geometry();
            auto edge_margin = qMin(12, qMax(1, rect.width() / 4));
            drop_into_folder = drop_pos.x() > rect.left() + edge_margin && drop_pos.x() < rect.right() - edge_margin;
        }
        if (drop_into_folder) {
            auto folder_id = target->property("folder_id").toString();
            auto moved = BookmarkStore::the()->moveBookmarkToFolder(payload.id, folder_id);
            debug_bookmark_drag(QStringLiteral("bookmark_drop id=%1 from=%2 to=folder folder_id=%3 index=-1 moved=%4")
                .arg(payload.id,
                    payload.source_folder_id.isEmpty() ? QStringLiteral("root") : QStringLiteral("folder:%1").arg(payload.source_folder_id),
                    folder_id,
                    moved ? QStringLiteral("true") : QStringLiteral("false")));
            event->acceptProposedAction();
            return;
        }
    }

    int to_index = compute_target_index();
    if (payload.type == QStringLiteral("folder")) {
        BookmarkStore::the()->moveRootItem(payload.id, to_index);
    } else if (payload.type == QStringLiteral("bookmark")) {
        auto moved = BookmarkStore::the()->moveBookmarkToRoot(payload.id, to_index);
        debug_bookmark_drag(QStringLiteral("bookmark_drop id=%1 from=%2 to=root index=%3 moved=%4")
            .arg(payload.id,
                payload.source_folder_id.isEmpty() ? QStringLiteral("root") : QStringLiteral("folder:%1").arg(payload.source_folder_id),
                QString::number(to_index),
                moved ? QStringLiteral("true") : QStringLiteral("false")));
    } else {
        debug_bookmark_drag(QStringLiteral("bookmark_drop ignored reason=unknown_type type=%1 id=%2")
            .arg(payload.type, payload.id));
        return;
    }
    event->acceptProposedAction();
}

}
