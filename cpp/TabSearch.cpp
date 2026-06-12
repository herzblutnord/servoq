#include "TabSearch.h"

#include "BrowserWindow.h"
#include "ChromeStyle.h"
#include "Icon.h"
#include "Tab.h"
#include "TabBar.h"
#include "WebViewURL.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPointer>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

Q_DECLARE_METATYPE(QPointer<ServoQ::Tab>)

namespace ServoQ {

namespace {

constexpr int PopupWidth = 400;
constexpr int PopupMaxListHeight = 420;
constexpr int RowHeight = 44;
constexpr int TabPointerRole = Qt::UserRole;
constexpr int SearchTextRole = Qt::UserRole + 1;

// Row widget: favicon, title over dimmed URL, hover-revealed close button.
class TabSearchRow final : public QWidget {
public:
    TabSearchRow(QIcon const& icon, QString const& title, QString const& url,
        std::function<void()> on_close, QWidget* parent)
        : QWidget(parent)
    {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(10, 4, 8, 4);
        layout->setSpacing(10);

        auto* icon_label = new QLabel(this);
        icon_label->setFixedSize(16, 16);
        icon_label->setPixmap(icon.pixmap(16, 16));
        icon_label->setScaledContents(true);
        layout->addWidget(icon_label);

        auto* text_column = new QWidget(this);
        auto* text_layout = new QVBoxLayout(text_column);
        text_layout->setContentsMargins(0, 0, 0, 0);
        text_layout->setSpacing(0);
        auto* title_label = new QLabel(title, text_column);
        title_label->setTextFormat(Qt::PlainText);
        auto* url_label = new QLabel(WebViewURL::url_for_display(url), text_column);
        url_label->setTextFormat(Qt::PlainText);
        auto muted = ChromeStyle::chrome_muted_text(palette());
        url_label->setStyleSheet(QStringLiteral("color: rgba(%1,%2,%3,%4); font-size: 11px;")
                                     .arg(muted.red()).arg(muted.green()).arg(muted.blue()).arg(muted.alpha()));
        for (auto* label : { title_label, url_label }) {
            label->setAttribute(Qt::WA_TransparentForMouseEvents);
            // Let the row shrink below the text's natural width; elision via
            // fixed sizing keeps rows uniform.
            label->setMinimumWidth(0);
            label->setMaximumWidth(PopupWidth - 90);
        }
        text_layout->addWidget(title_label);
        text_layout->addWidget(url_label);
        layout->addWidget(text_column, 1);

        m_close_button = new QToolButton(this);
        m_close_button->setIcon(create_chrome_icon(ChromeIcon::TabClose, palette()));
        m_close_button->setIconSize({ 14, 14 });
        m_close_button->setFixedSize(22, 22);
        m_close_button->setAutoRaise(true);
        m_close_button->setFocusPolicy(Qt::NoFocus);
        m_close_button->setToolTip(QStringLiteral("Close tab"));
        m_close_button->setCursor(Qt::PointingHandCursor);
        m_close_button->hide();
        connect(m_close_button, &QToolButton::clicked, this, [on_close = std::move(on_close)] {
            if (on_close)
                on_close();
        });
        layout->addWidget(m_close_button);
    }

protected:
    void enterEvent(QEnterEvent*) override { m_close_button->show(); }
    void leaveEvent(QEvent*) override { m_close_button->hide(); }

private:
    QToolButton* m_close_button { nullptr };
};

}

TabSearchPopup::TabSearchPopup(BrowserWindow* window, TabWidget* tabs)
    : QWidget(window, Qt::Popup | Qt::FramelessWindowHint)
    , m_window(window)
    , m_tabs(tabs)
{
    setAttribute(Qt::WA_DeleteOnClose, false);
    setFixedWidth(PopupWidth);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    m_search_edit = new QLineEdit(this);
    m_search_edit->setPlaceholderText(QStringLiteral("Search tabs"));
    m_search_edit->setClearButtonEnabled(true);
    m_search_edit->installEventFilter(this);
    layout->addWidget(m_search_edit);

    m_list = new QListWidget(this);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setUniformItemSizes(true);
    m_list->setMouseTracking(true);
    layout->addWidget(m_list);

    connect(m_search_edit, &QLineEdit::textChanged, this, [this](QString const& query) {
        applyFilter(query);
    });
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        activateItem(item);
    });
}

Tab* TabSearchPopup::tabForItem(QListWidgetItem* item) const
{
    if (!item)
        return nullptr;
    auto pointer = item->data(TabPointerRole).value<QPointer<Tab>>();
    return pointer.data();
}

void TabSearchPopup::rebuildList()
{
    m_list->clear();
    if (!m_tabs)
        return;

    for (int i = 0; i < m_tabs->count(); ++i) {
        auto* tab = m_tabs->tab(i);
        if (!tab)
            continue;
        auto title = tab->title();
        auto url = tab->isEmptyNewTab() ? QString() : tab->url();

        auto* item = new QListWidgetItem(m_list);
        item->setSizeHint(QSize(PopupWidth - 16, RowHeight));
        item->setData(TabPointerRole, QVariant::fromValue(QPointer<Tab>(tab)));
        item->setData(SearchTextRole, (title + QLatin1Char('\n') + url).toLower());

        QPointer<Tab> tab_ptr = tab;
        auto* row = new TabSearchRow(tab->siteIcon(), title, url, [this, tab_ptr] {
            if (!tab_ptr || !m_window)
                return;
            auto index = m_tabs->indexOf(tab_ptr);
            if (index >= 0)
                m_window->closeTabFromContextMenu(index);
            // Rebuild after the deferred close has run.
            QTimer::singleShot(0, this, [this] {
                if (isVisible()) {
                    rebuildList();
                    applyFilter(m_search_edit->text());
                }
            });
        }, m_list);
        m_list->setItemWidget(item, row);

        if (i == m_tabs->currentIndex())
            m_list->setCurrentItem(item);
    }
}

void TabSearchPopup::applyFilter(QString const& query)
{
    auto needle = query.trimmed().toLower();
    QListWidgetItem* first_visible = nullptr;
    for (int i = 0; i < m_list->count(); ++i) {
        auto* item = m_list->item(i);
        auto matches = needle.isEmpty() || item->data(SearchTextRole).toString().contains(needle);
        item->setHidden(!matches);
        if (matches && !first_visible)
            first_visible = item;
    }
    // While filtering, keep the selection on the best (first) match so Enter
    // does what the user expects.
    if (!needle.isEmpty())
        m_list->setCurrentItem(first_visible);
    else if (!m_list->currentItem() && first_visible)
        m_list->setCurrentItem(first_visible);
}

void TabSearchPopup::activateItem(QListWidgetItem* item)
{
    auto* tab = tabForItem(item);
    if (!tab || !m_tabs) {
        close();
        return;
    }
    auto index = m_tabs->indexOf(tab);
    close();
    if (index >= 0)
        m_tabs->setCurrentIndex(index);
}

void TabSearchPopup::closeTabForItem(QListWidgetItem* item)
{
    auto* tab = tabForItem(item);
    if (!tab || !m_window)
        return;
    auto index = m_tabs->indexOf(tab);
    if (index >= 0)
        m_window->closeTabFromContextMenu(index);
    QTimer::singleShot(0, this, [this] {
        if (isVisible()) {
            rebuildList();
            applyFilter(m_search_edit->text());
        }
    });
}

void TabSearchPopup::moveSelection(int delta)
{
    auto current = m_list->currentRow();
    auto row = current;
    for (int step = 0; step < m_list->count(); ++step) {
        row += delta;
        if (row < 0)
            row = m_list->count() - 1;
        else if (row >= m_list->count())
            row = 0;
        if (!m_list->item(row)->isHidden()) {
            m_list->setCurrentRow(row);
            m_list->scrollToItem(m_list->item(row));
            return;
        }
    }
}

bool TabSearchPopup::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_search_edit && event->type() == QEvent::KeyPress) {
        auto* key_event = static_cast<QKeyEvent*>(event);
        switch (key_event->key()) {
        case Qt::Key_Down:
            moveSelection(1);
            return true;
        case Qt::Key_Up:
            moveSelection(-1);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            activateItem(m_list->currentItem());
            return true;
        case Qt::Key_Escape:
            close();
            return true;
        // Ctrl+Shift+W closes the selected tab from the list (Chrome's tab
        // search uses the per-row close affordance; this is the keyboard path).
        case Qt::Key_W:
            if (key_event->modifiers().testFlag(Qt::ControlModifier)) {
                closeTabForItem(m_list->currentItem());
                return true;
            }
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TabSearchPopup::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        close();
        return;
    }
    QWidget::keyPressEvent(event);
}

void TabSearchPopup::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(palette().window());
    painter.setPen(QPen(ChromeStyle::chrome_border(palette()), 1));
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8.0, 8.0);
}

void TabSearchPopup::open()
{
    rebuildList();
    m_search_edit->clear();
    applyFilter({});

    auto visible_rows = 0;
    for (int i = 0; i < m_list->count(); ++i) {
        if (!m_list->item(i)->isHidden())
            ++visible_rows;
    }
    auto list_height = std::min(PopupMaxListHeight, std::max(1, visible_rows) * RowHeight + 4);
    m_list->setFixedHeight(list_height);
    adjustSize();

    // Anchor at the top-right of the page area, just below the toolbar (where
    // Chrome's tab-search dropdown lives).
    if (m_tabs && m_tabs->contentViewport()) {
        auto* viewport = m_tabs->contentViewport();
        auto top_right = viewport->mapToGlobal(QPoint(viewport->width(), 0));
        move(top_right.x() - width() - 12, top_right.y() + 8);
    } else if (m_window) {
        auto anchor = m_window->mapToGlobal(QPoint(m_window->width(), 0));
        move(anchor.x() - width() - 12, anchor.y() + 86);
    }
    show();
    m_search_edit->setFocus();
}

}
