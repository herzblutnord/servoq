#include "BookmarksBar.h"
#include "BookmarkStore.h"
#include "ChromeStyle.h"
#include "Icon.h"
#include "Settings.h"

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QComboBox>
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
#include <QWidget>

namespace ServoQ {

// [ladybird: BookmarksBar.cpp:29-35]
static constexpr int BookmarkButtonMaxWidth         = 150;
static constexpr int BookmarkButtonIconSize         = 16;
static constexpr int BookmarkButtonMinHeight        = 24;
static constexpr int BookmarkButtonVerticalPadding  = 8;
static constexpr int BookmarkButtonHorizontalPadding = 7;
static constexpr int BookmarkButtonIconTextSpacing  = 6;
static constexpr int BookmarkButtonTextElisionPadding = 2;

static constexpr char const* BookmarkItemProperty = "bookmark_item";

static QIcon bookmark_icon_from_base64(QString const& favicon_base64_png, QPalette const& palette)
{
    if (!favicon_base64_png.isEmpty()) {
        QPixmap pixmap;
        if (pixmap.loadFromData(QByteArray::fromBase64(favicon_base64_png.toLatin1()), "PNG"))
            return QIcon(pixmap);
    }
    return create_chrome_icon(ChromeIcon::Globe, palette);
}

// Custom paint replicates reference paint_bookmark_button — [ladybird: BookmarksBar.cpp:130-160]
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

    // Menu indicator width for folders — [ladybird: BookmarksBar.cpp:96]
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

    // Draw frame only (suppress icon/text in drawComplexControl) — [ladybird: BookmarksBar.cpp:137-140]
    auto frame_option = option;
    frame_option.icon = {};
    frame_option.text.clear();
    painter.drawComplexControl(QStyle::CC_ToolButton, frame_option);

    // Draw icon — [ladybird: BookmarksBar.cpp:142-148]
    if (icon_width > 0) {
        auto mode = button.isEnabled() ? QIcon::Normal : QIcon::Disabled;
        if (button.isEnabled() && (option.state & QStyle::State_MouseOver))
            mode = QIcon::Active;
        option.icon.paint(&painter, icon_rect, Qt::AlignCenter, mode, button.isChecked() ? QIcon::On : QIcon::Off);
    }

    // Elide and draw text — [ladybird: BookmarksBar.cpp:150-159]
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

    // [ladybird: BookmarksBar.cpp:221-234]
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
    setObjectName("LadybirdBookmarksBar");  // [ladybird: BookmarksBar.cpp:29-35]
    setMovable(false);
    setFloatable(false);
    setAcceptDrops(true);
    setIconSize({ BookmarkButtonIconSize, BookmarkButtonIconSize }); // [ladybird: BookmarksBar.cpp:30]
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

void BookmarksBar::rebuild() // [ladybird: BookmarksBar.cpp:205-273]
{
    // Close any open menus before clearing — [ladybird: BookmarksBar.cpp:207-210]
    for (auto* action : actions()) {
        if (auto* menu = action->menu())
            menu->close();
    }

    clear();

    auto add_bookmark_button = [this](BookmarkItem const& item) {
        auto* action = new QAction(this);
        action->setIcon(bookmark_icon_from_base64(item.favicon_base64_png, palette()));
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

        for (auto const& child : folder.items) {
            auto* child_action = submenu->addAction(
                bookmark_icon_from_base64(child.favicon_base64_png, palette()), child.title);
            child_action->setToolTip(child.url);
            child_action->setProperty("bookmark_id", child.id);
            child_action->setProperty("bookmark_type", QStringLiteral("bookmark"));
            connect(child_action, &QAction::triggered, this, [this, child] {
                if (m_open_url_callback)
                    m_open_url_callback(child.url);
            });
        }

        // Add "New Bookmark in Folder" to submenu separator + action
        submenu->addSeparator();
        auto* add_to_folder = submenu->addAction(QStringLiteral("Add Bookmark Here…"));
        connect(add_to_folder, &QAction::triggered, this, [this, folder] {
            showAddBookmarkDialog(folder.id);
        });

        // Install event filter on submenu for middle-click / right-click — [ladybird: BookmarksBar.cpp:162-170]
        submenu->installEventFilter(this);

        auto* folder_action = new QAction(folder.title, this);
        folder_action->setIcon(create_chrome_icon(ChromeIcon::Folder, palette()));
        folder_action->setMenu(submenu);
        folder_action->setProperty("folder_id", folder.id);
        folder_action->setProperty("bookmark_type", QStringLiteral("folder"));
        addAction(folder_action);

        if (auto* button = qobject_cast<QToolButton*>(widgetForAction(folder_action))) {
            button->setPopupMode(QToolButton::InstantPopup); // [ladybird: BookmarksBar.cpp:266]
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
    setStyleSheet(ChromeStyle::bookmarks_bar_style_sheet(palette()));
    m_is_updating_chrome_style = false;
}

// [ladybird: BookmarksBar.cpp:293-313]
bool BookmarksBar::eventFilter(QObject* object, QEvent* event)
{
    // Custom paint for bookmark/folder buttons — [ladybird: BookmarksBar.cpp:295-299]
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
                    if (delta < QApplication::startDragDistance())
                        button->showMenu();
                    return true;
                }
            }
        }
        m_drag_source_id.clear();
        m_drag_source_type.clear();
    }

    if (event->type() == QEvent::MouseMove) {
        auto& mouse_event = static_cast<QMouseEvent&>(*event);
        if ((mouse_event.buttons() & Qt::LeftButton) && !m_drag_source_id.isEmpty()) {
            auto delta = (mouse_event.position().toPoint() - m_drag_start_pos).manhattanLength();
            if (delta >= QApplication::startDragDistance()) {
                QString id = m_drag_source_id;
                QString type = m_drag_source_type;
                m_drag_source_id.clear();
                auto* drag = new QDrag(this);
                auto* mime = new QMimeData();
                mime->setData("application/x-servoq-bookmark", (type + ":" + id).toUtf8());
                drag->setMimeData(mime);
                if (auto* button = qobject_cast<QToolButton*>(object))
                    drag->setPixmap(button->grab());
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
                    } else if (bm_type == QStringLiteral("folder")) {
                        m_drag_source_id = action->property("folder_id").toString();
                        m_drag_source_type = QStringLiteral("folder");
                        m_drag_start_pos = mouse_event.position().toPoint();
                        return true;
                    }
                    m_drag_start_pos = mouse_event.position().toPoint();
                }
            }
        }

        if (mouse_event.button() == Qt::MiddleButton) {
            // Middle-click opens in new tab — [ladybird: BookmarksBar.cpp:323-343]
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
            // Right-click context menu — [ladybird: BookmarksBar.cpp:345-393]
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
                        showAddBookmarkDialog(folder_id);
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

    return QToolBar::eventFilter(object, event);
}

// [ladybird: BrowserWindow.cpp:360 — Ctrl+D add bookmark]
void BookmarksBar::showAddBookmarkDialog(QString const& title, QString const& url)
{
    showAddBookmarkDialog({}, title, url);
}

void BookmarksBar::showAddBookmarkDialog(QString const& folder_id,
                                         QString const& prefill_title,
                                         QString const& prefill_url)
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

    if (dialog->exec() == QDialog::Accepted) {
        auto selected_folder = folder_combo->currentData().toString();
        BookmarkStore::the()->addBookmark(title_edit->text(), url_edit->text(), selected_folder);
    }
    dialog->deleteLater();
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

    if (dialog->exec() == QDialog::Accepted)
        BookmarkStore::the()->editBookmark(id, title_edit->text(), url_edit->text());
    dialog->deleteLater();
}

void BookmarksBar::showEditFolderDialog(QString const& id)
{
    QString current_title;
    for (auto const& f : BookmarkStore::the()->folders())
        if (f.id == id) { current_title = f.title; break; }

    bool ok;
    auto new_title = QInputDialog::getText(this, QStringLiteral("Rename Folder"),
        QStringLiteral("Folder name:"), QLineEdit::Normal, current_title, &ok);
    if (ok && !new_title.isEmpty())
        BookmarkStore::the()->editFolder(id, new_title);
}

void BookmarksBar::showNewFolderDialog()
{
    bool ok;
    auto title = QInputDialog::getText(this, QStringLiteral("New Folder"),
        QStringLiteral("Folder name:"), QLineEdit::Normal, {}, &ok);
    if (ok && !title.isEmpty())
        BookmarkStore::the()->addFolder(title);
}

void BookmarksBar::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasFormat("application/x-servoq-bookmark"))
        event->acceptProposedAction();
}

void BookmarksBar::dragLeaveEvent(QDragLeaveEvent* event)
{
    hideDropIndicator();
    QToolBar::dragLeaveEvent(event);
}

void BookmarksBar::dragMoveEvent(QDragMoveEvent* event)
{
    if (!event->mimeData()->hasFormat("application/x-servoq-bookmark"))
        return;

    auto data = QString::fromUtf8(event->mimeData()->data("application/x-servoq-bookmark"));
    auto colon = data.indexOf(':');
    if (colon < 0)
        return;
    auto x = insertionIndicatorX(event->position().toPoint());
    m_drop_indicator->setGeometry(x, 4, 2, qMax(1, height() - 8));
    m_drop_indicator->raise();
    m_drop_indicator->show();
    event->acceptProposedAction();
}

void BookmarksBar::dropEvent(QDropEvent* event)
{
    if (!event->mimeData()->hasFormat("application/x-servoq-bookmark"))
        return;

    auto data = QString::fromUtf8(event->mimeData()->data("application/x-servoq-bookmark"));
    auto colon = data.indexOf(':');
    if (colon < 0) return;
    auto id = data.mid(colon + 1);
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

    int to_index = compute_target_index();
    BookmarkStore::the()->moveRootItem(id, to_index);
    event->acceptProposedAction();
}

}
