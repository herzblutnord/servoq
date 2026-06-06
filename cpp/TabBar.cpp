#include "TabBar.h"

/*
 * Portions of this file are adapted from Ladybird UI/Qt/TabBar.cpp and
 * UI/Qt/ChromeLayout.h.
 *
 * Copyright (c) 2024-2026, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ChromeStyle.h"
#include "ChromeLayout.h"
#include "Icon.h"
#include "Tab.h"

#include <QAction>
#include <QApplication>
#include <QBoxLayout>
#include <QCursor>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEasingCurve>
#include <QEvent>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QDrag>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPixmap>
#include <QFontMetrics>
#include <QApplication>
#include <QStyle>
#include <QToolButton>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>

namespace ServoQ {
namespace {

// Constants mirror Ladybird UI/Qt/TabBar.cpp and UI/Qt/ChromeLayout.h.
constexpr int HorizontalTabStripHeight = 44;
constexpr int HorizontalTabHeight = 38;
constexpr int HorizontalTabMinWidth = 128;
constexpr int HorizontalTabMaxWidth = 240;
constexpr int VerticalTabHeight = 38;
constexpr auto ServoQTabMimeType = "application/x-servoq-tab";
constexpr int VerticalTabsCollapsedWidth = browser_chrome_layout_policy().collapsed_sidebar_width;
constexpr int VerticalTabsDefaultExpandedWidth = browser_chrome_layout_policy().expanded_sidebar_width;
constexpr int VerticalTabsMinExpandedWidth = 190;
constexpr int VerticalTabsMaxExpandedWidth = 400;
constexpr int VerticalTabsResizeHitAreaWidth = 5;
constexpr int VerticalTabsCollapsedSideMargin = 6;
constexpr int VerticalTabsExpandedSideMargin = 5;
constexpr int VerticalTabsTopMargin = 8;
constexpr int VerticalTabsHoverCollapsePollIntervalMs = 250;
constexpr auto VerticalTabsExpandedProperty = "verticalTabsExpanded";
constexpr auto VerticalTabsButtonProperty = "verticalTabsButton";
constexpr auto VerticalTabsResizeHandleHoveredProperty = "hovered";
constexpr auto VerticalTabsResizeHandleActiveProperty = "active";

int verticalTabsSideMargin(bool expanded)
{
    return expanded ? VerticalTabsExpandedSideMargin : VerticalTabsCollapsedSideMargin;
}

int verticalTabsHorizontalMarginWidth(TabLayout layout)
{
    return verticalTabsSideMargin(layout != TabLayout::VerticalCollapsed) * 2;
}

int clampVerticalTabsExpandedWidth(int width)
{
    return std::clamp(width, VerticalTabsMinExpandedWidth, VerticalTabsMaxExpandedWidth);
}

void setDynamicPropertyIfNeeded(QWidget& widget, char const* property, QVariant const& value)
{
    if (widget.property(property) == value)
        return;
    widget.setProperty(property, value);
    widget.style()->unpolish(&widget);
    widget.style()->polish(&widget);
    widget.update();
}

QPainterPath tabShapePath(QRectF const& rect, qreal top_radius, qreal bottom_radius)
{
    top_radius = std::min(top_radius, rect.height() / 2.0);
    bottom_radius = std::min(bottom_radius, rect.height() / 2.0);

    QPainterPath path;
    path.moveTo(rect.left() + top_radius, rect.top());
    path.lineTo(rect.right() - top_radius, rect.top());
    path.quadTo(rect.right(), rect.top(), rect.right(), rect.top() + top_radius);
    path.lineTo(rect.right(), rect.bottom() - bottom_radius);
    path.quadTo(rect.right(), rect.bottom(), rect.right() - bottom_radius, rect.bottom());
    path.lineTo(rect.left() + bottom_radius, rect.bottom());
    path.quadTo(rect.left(), rect.bottom(), rect.left(), rect.bottom() - bottom_radius);
    path.lineTo(rect.left(), rect.top() + top_radius);
    path.quadTo(rect.left(), rect.top(), rect.left() + top_radius, rect.top());
    path.closeSubpath();
    return path;
}

QRectF tabCardShapeRect(QRectF const& rect)
{
    return rect.adjusted(5, 3, -5, -3);
}

QRectF horizontalTabCardShapeRect(QRectF const& rect)
{
    return rect.adjusted(4.0, 3, -4.0, -3);
}

QRectF collapsedVerticalTabShapeRect(QRectF const& rect)
{
    return rect.adjusted(4.0, 3.0, -4.0, -3.0);
}

QColor tabHoverSurface(QPalette const& palette, qreal hover_progress)
{
    auto dark = ChromeStyle::is_dark(palette);
    auto color = dark ? QColor(255, 255, 255) : QColor(0, 0, 0);
    color.setAlpha(static_cast<int>((dark ? 24 : 16) * hover_progress));
    return color;
}

QColor selectedTabBorder(QPalette const& palette, bool collapsed)
{
    auto dark = ChromeStyle::is_dark(palette);
    auto color = dark ? QColor(255, 255, 255) : QColor(0, 0, 0);
    color.setAlpha(collapsed ? (dark ? 32 : 30) : (dark ? 26 : 24));
    return color;
}

QRectF horizontalNewTabButtonShapeRect(QRectF const& rect)
{
    auto x = rect.left() + (rect.width() - 32) / 2.0;
    auto y = rect.top() + (rect.height() - 32) / 2.0;
    return { x, y, 32, 32 };
}

class NewTabButton final : public QToolButton {
public:
    explicit NewTabButton(TabBar& tab_bar, QWidget* parent)
        : QToolButton(parent)
        , m_tab_bar(tab_bar)
    {
    }

private:
    bool isHovered() const
    {
        return rect().contains(mapFromGlobal(QCursor::pos()));
    }

    void enterEvent(QEnterEvent* event) override
    {
        QToolButton::enterEvent(event);
        update();
    }

    void leaveEvent(QEvent* event) override
    {
        QToolButton::leaveEvent(event);
        update();
    }

    void paintEvent(QPaintEvent* event) override
    {
        if (!property(VerticalTabsButtonProperty).toBool()) {
            QToolButton::paintEvent(event);
            return;
        }

        auto expanded = property(VerticalTabsExpandedProperty).toBool();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        auto is_horizontal = m_tab_bar.tabLayout() == TabLayout::Horizontal;
        auto shape_rect = expanded
            ? tabCardShapeRect(QRectF(rect()))
            : (is_horizontal ? horizontalNewTabButtonShapeRect(QRectF(rect())) : QRectF(rect()).adjusted(4.0, 3.0, -4.0, -3.0));
        auto path = tabShapePath(shape_rect, 9.0, 9.0);
        if (isDown()) {
            painter.setBrush(ChromeStyle::chrome_surface_pressed(palette()));
            painter.setPen(QPen(ChromeStyle::chrome_border(palette()), 1));
            painter.drawPath(path);
        } else if (isHovered()) {
            painter.setBrush(tabHoverSurface(palette(), 1.0));
            painter.setPen(Qt::NoPen);
            painter.drawPath(path);
        }

        auto contents_rect = shape_rect.toAlignedRect().adjusted(8, 0, -8, 0);
        auto icon_size = QSize(16, 16);
        QRect icon_rect {
            expanded ? contents_rect.left() : rect().center().x() - icon_size.width() / 2,
            contents_rect.top() + (contents_rect.height() - icon_size.height()) / 2,
            icon_size.width(),
            icon_size.height()
        };
        icon().paint(&painter, icon_rect);
        if (!expanded)
            return;

        contents_rect.setLeft(icon_rect.right() + 8);
        painter.setPen(ChromeStyle::chrome_button_text(palette()));
        QFontMetrics metrics(font());
        painter.drawText(contents_rect, Qt::AlignLeft | Qt::AlignVCenter, metrics.elidedText(text(), Qt::ElideRight, contents_rect.width()));
    }

    TabBar& m_tab_bar;
};

}

TabBar::TabBar(TabWidget* parent)
    : QTabBar(parent)
    , m_hover_animation(new QVariantAnimation(this))
{
    setMouseTracking(true);
    setAcceptDrops(true);
    setDrawBase(false);
    setDocumentMode(true);
    setMovable(true);
    setTabsClosable(true);
    setExpanding(false);
    setUsesScrollButtons(true);
    setElideMode(Qt::ElideRight);
    setFocusPolicy(Qt::NoFocus);
    setIconSize({ 16, 16 });
    setMinimumHeight(HorizontalTabHeight);
    m_hover_animation->setDuration(120);
    m_hover_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_hover_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
        m_hover_progress = value.toReal();
        update();
    });
    connect(m_hover_animation, &QVariantAnimation::finished, this, [this] {
        if (m_hover_progress <= 0.0)
            m_hover_animation_tab_index = -1;
    });
    connect(this, &QTabBar::currentChanged, this, [this](int index) {
        ensureTabVisible(index);
    });
}

void TabBar::setAvailableWidth(int width)
{
    if (m_available_width == width)
        return;
    m_available_width = width;
    refreshTabLayout();
}

void TabBar::setTabLayout(TabLayout layout)
{
    if (m_tab_layout == layout)
        return;

    m_tab_layout = layout;
    if (m_tab_layout == TabLayout::Horizontal) {
        setShape(QTabBar::RoundedNorth);
        setMinimumSize({ 0, HorizontalTabHeight });
        setMaximumWidth(QWIDGETSIZE_MAX);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setUsesScrollButtons(true);
        setVerticalScrollOffset(0);
    } else {
        setShape(QTabBar::RoundedWest);
        setMinimumSize({ 0, 0 });
        setMaximumWidth(QWIDGETSIZE_MAX);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        setUsesScrollButtons(false);
    }
    refreshTabLayout();
}

void TabBar::refreshTabLayout()
{
    setVerticalScrollOffset(m_vertical_scroll_offset);
    updateGeometry();
    update();
}

QSize TabBar::sizeHint() const
{
    if (m_tab_layout != TabLayout::Horizontal)
        return verticalSizeHint(count());
    return QTabBar::sizeHint();
}

QSize TabBar::minimumSizeHint() const
{
    if (m_tab_layout != TabLayout::Horizontal)
        return verticalSizeHint(count() > 0 ? 1 : 0);
    return QTabBar::minimumSizeHint();
}

QSize TabBar::tabSizeHint(int index) const
{
    auto size = QTabBar::tabSizeHint(index);
    if (m_tab_layout != TabLayout::Horizontal) {
        auto width = m_available_width > 0 ? m_available_width : (m_tab_layout == TabLayout::VerticalCollapsed ? VerticalTabsCollapsedWidth : VerticalTabsDefaultExpandedWidth);
        size.setWidth(width);
        size.setHeight(VerticalTabHeight);
        return size;
    }

    if (auto tab_count = count(); tab_count > 0) {
        auto width = (m_available_width > 0 ? m_available_width : this->width()) / tab_count;
        size.setWidth(std::clamp(width, HorizontalTabMinWidth, HorizontalTabMaxWidth));
    }
    size.setHeight(HorizontalTabHeight);
    return size;
}

void TabBar::resizeEvent(QResizeEvent* event)
{
    QTabBar::resizeEvent(event);
    setVerticalScrollOffset(m_vertical_scroll_offset);
    ensureTabVisible(currentIndex());
}

void TabBar::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    auto text_color = ChromeStyle::chrome_text(palette());
    auto muted_text = ChromeStyle::chrome_muted_text(palette());
    auto is_vertical = m_tab_layout != TabLayout::Horizontal;
    auto is_collapsed = m_tab_layout == TabLayout::VerticalCollapsed;

    for (int index = 0; index < count(); ++index) {
        auto tab_rect = visualTabRect(index);
        if (!tab_rect.isValid())
            continue;
        if (is_vertical && (tab_rect.bottom() < 0 || tab_rect.top() > height()))
            continue;

        auto selected = index == currentIndex();
        auto hover_progress = index == m_hover_animation_tab_index ? m_hover_progress : (index == m_hovered_tab_index ? 1.0 : 0.0);

        QRectF shape_rect;
        if (is_collapsed)
            shape_rect = collapsedVerticalTabShapeRect(QRectF(tab_rect));
        else if (is_vertical)
            shape_rect = tabCardShapeRect(QRectF(tab_rect));
        else
            shape_rect = horizontalTabCardShapeRect(QRectF(tab_rect));

        auto shape = tabShapePath(shape_rect, 9.0, 9.0);
        if (selected) {
            painter.setBrush(ChromeStyle::chrome_surface(palette()));
            painter.setPen(QPen(selectedTabBorder(palette(), is_collapsed), 1));
            painter.drawPath(shape);
        } else if (hover_progress > 0.0) {
            painter.setBrush(tabHoverSurface(palette(), hover_progress));
            painter.setPen(Qt::NoPen);
            painter.drawPath(shape);
        }

        auto contents_rect = shape_rect.toAlignedRect().adjusted(8, 0, -8, 0);
        auto icon = tabIcon(index);
        if (icon.isNull())
            icon = create_chrome_icon(ChromeIcon::Globe, palette());
        auto icon_size = is_collapsed ? QSize(16, 16) : QSize(16, 16);
        QRect icon_rect {
            is_collapsed ? tab_rect.center().x() - icon_size.width() / 2 : contents_rect.left(),
            contents_rect.top() + (contents_rect.height() - icon_size.height()) / 2,
            icon_size.width(),
            icon_size.height()
        };
        icon.paint(&painter, icon_rect, Qt::AlignCenter, isEnabled() ? QIcon::Normal : QIcon::Disabled);

        if (is_collapsed)
            continue;

        contents_rect.setLeft(icon_rect.right() + 8);
        if (auto* button = tabButton(index, QTabBar::RightSide); button && button->isVisible())
            contents_rect.setRight(contents_rect.right() - button->width() - 6);

        painter.setPen(selected ? text_color : muted_text);
        QFontMetrics metrics(font());
        auto title = metrics.elidedText(tabText(index), Qt::ElideRight, std::max(0, contents_rect.width()));
        painter.drawText(contents_rect, Qt::AlignLeft | Qt::AlignVCenter, title);
    }

    if (m_drop_indicator_index >= 0) {
        painter.setPen(QPen(ChromeStyle::chrome_accent(palette()), 2));
        if (is_vertical) {
            auto y = m_drop_indicator_index * VerticalTabHeight - m_vertical_scroll_offset;
            painter.drawLine(4, y, width() - 4, y);
        } else {
            int x = 0;
            if (m_drop_indicator_index < count())
                x = visualTabRect(m_drop_indicator_index).left();
            else if (count() > 0)
                x = visualTabRect(count() - 1).right();
            painter.drawLine(x, 7, x, height() - 7);
        }
    }
}

void TabBar::leaveEvent(QEvent* event)
{
    setHoveredTabIndex(-1);
    QTabBar::leaveEvent(event);
}

void TabBar::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && tabAt(event->pos()) == -1) {
        if (auto* widget = static_cast<TabWidget*>(parentWidget())) {
            if (widget->onNewTabRequested)
                widget->onNewTabRequested();
            return;
        }
    }
    QTabBar::mouseDoubleClickEvent(event);
}

void TabBar::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_pressed_tab_index = tabIndexAt(event->pos());
        m_drag_start_position = event->pos();
    }
    QTabBar::mousePressEvent(event);
}

void TabBar::mouseMoveEvent(QMouseEvent* event)
{
    setHoveredTabIndex(tabIndexAt(event->pos()));
    if ((event->buttons() & Qt::LeftButton) && m_pressed_tab_index >= 0) {
        if ((event->pos() - m_drag_start_position).manhattanLength() >= QApplication::startDragDistance()) {
            startTabDrag(m_pressed_tab_index);
            m_pressed_tab_index = -1;
            return;
        }
    }
    QTabBar::mouseMoveEvent(event);
}

void TabBar::mouseReleaseEvent(QMouseEvent* event)
{
    m_pressed_tab_index = -1;
    QTabBar::mouseReleaseEvent(event);
}

void TabBar::wheelEvent(QWheelEvent* event)
{
    if (m_tab_layout == TabLayout::Horizontal || maxVerticalScrollOffset() <= 0) {
        QTabBar::wheelEvent(event);
        return;
    }

    auto angle_delta = event->angleDelta().y();
    if (angle_delta == 0) {
        event->ignore();
        return;
    }
    auto scroll_delta = angle_delta * VerticalTabHeight / 120;
    setVerticalScrollOffset(m_vertical_scroll_offset - scroll_delta);
    event->accept();
}

void TabBar::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasFormat(ServoQTabMimeType)) {
        event->acceptProposedAction();
        return;
    }
    QTabBar::dragEnterEvent(event);
}

void TabBar::dragLeaveEvent(QDragLeaveEvent* event)
{
    m_drop_indicator_index = -1;
    update();
    QTabBar::dragLeaveEvent(event);
}

void TabBar::dragMoveEvent(QDragMoveEvent* event)
{
    if (!event->mimeData()->hasFormat(ServoQTabMimeType)) {
        event->ignore();
        return;
    }
    m_drop_indicator_index = dropIndicatorIndexForInsertionIndex(insertionIndexAt(event->position().toPoint()));
    update();
    event->acceptProposedAction();
}

void TabBar::dropEvent(QDropEvent* event)
{
    if (!event->mimeData()->hasFormat(ServoQTabMimeType)) {
        event->ignore();
        return;
    }
    auto from = event->mimeData()->data(ServoQTabMimeType).toInt();
    auto to = insertionIndexAt(event->position().toPoint());
    if (to > from)
        --to;
    to = std::clamp(to, 0, count() - 1);
    if (from != to)
        moveTab(from, to);
    m_drop_indicator_index = -1;
    update();
    event->acceptProposedAction();
}

QRect TabBar::visualTabRect(int index) const
{
    if (m_tab_layout == TabLayout::Horizontal)
        return tabRect(index);
    return { 0, index * VerticalTabHeight - m_vertical_scroll_offset, tabSizeHint(index).width(), VerticalTabHeight };
}

int TabBar::tabIndexAt(QPoint const& position) const
{
    if (m_tab_layout == TabLayout::Horizontal)
        return tabAt(position);
    auto y = position.y() + m_vertical_scroll_offset;
    if (y < 0 || y >= count() * VerticalTabHeight)
        return -1;
    return std::clamp(y / VerticalTabHeight, 0, count() - 1);
}

int TabBar::insertionIndexAt(QPoint const& position) const
{
    if (m_tab_layout == TabLayout::Horizontal) {
        for (int i = 0; i < count(); ++i) {
            auto rect = tabRect(i);
            if (position.x() < rect.center().x())
                return i;
        }
        return count();
    }
    auto y = position.y() + m_vertical_scroll_offset;
    auto index = y / VerticalTabHeight;
    if (y > (index * VerticalTabHeight) + (VerticalTabHeight / 2))
        ++index;
    return std::clamp(index, 0, count());
}

int TabBar::dropIndicatorIndexForInsertionIndex(int insertion_index) const
{
    return std::clamp(insertion_index, 0, count());
}

QPixmap TabBar::renderTabDragPixmap(int index) const
{
    auto rect = visualTabRect(index);
    QPixmap pixmap(rect.size());
    pixmap.fill(Qt::transparent);
    const_cast<TabBar*>(this)->render(&pixmap, QPoint(), QRegion(rect));
    return pixmap;
}

void TabBar::startTabDrag(int index)
{
    if (index < 0 || index >= count())
        return;
    auto* drag = new QDrag(this);
    auto* mime = new QMimeData;
    mime->setData(ServoQTabMimeType, QByteArray::number(index));
    drag->setMimeData(mime);
    drag->setPixmap(renderTabDragPixmap(index));
    drag->exec(Qt::MoveAction);
}

void TabBar::startHoverAnimation(int tab_index, qreal target_progress)
{
    m_hover_animation_tab_index = tab_index;
    m_hover_animation->stop();
    m_hover_animation->setStartValue(m_hover_progress);
    m_hover_animation->setEndValue(target_progress);
    m_hover_animation->start();
}

void TabBar::setHoveredTabIndex(int index)
{
    if (m_hovered_tab_index == index)
        return;
    auto previous = m_hovered_tab_index;
    m_hovered_tab_index = index;
    if (previous >= 0)
        startHoverAnimation(previous, 0.0);
    if (index >= 0)
        startHoverAnimation(index, 1.0);
    update();
}

QSize TabBar::verticalSizeHint(int tab_count) const
{
    auto width = m_available_width > 0 ? m_available_width : VerticalTabsDefaultExpandedWidth;
    return { width, tab_count * VerticalTabHeight };
}

int TabBar::maxVerticalScrollOffset() const
{
    if (m_tab_layout == TabLayout::Horizontal)
        return 0;
    return std::max(0, (count() * VerticalTabHeight) - height());
}

void TabBar::setVerticalScrollOffset(int offset)
{
    m_vertical_scroll_offset = std::clamp(offset, 0, maxVerticalScrollOffset());
    update();
}

void TabBar::ensureTabVisible(int index)
{
    if (m_tab_layout == TabLayout::Horizontal || index < 0 || index >= count())
        return;
    auto tab_top = index * VerticalTabHeight;
    auto tab_bottom = tab_top + VerticalTabHeight;
    if (tab_top < m_vertical_scroll_offset)
        setVerticalScrollOffset(tab_top);
    else if (tab_bottom > m_vertical_scroll_offset + height())
        setVerticalScrollOffset(tab_bottom - height());
}

TabWidget::TabWidget(QWidget* parent)
    : QWidget(parent)
    , m_tab_bar(new TabBar(this))
    , m_stack(new QStackedWidget(this))
    , m_toolbar_container(new QStackedWidget(this))
    , m_page_column(new QWidget(this))
    , m_tab_bar_row(new QWidget(this))
    , m_vertical_tabs_content(new QWidget(this))
    , m_vertical_tabs_reserved_space(new QWidget(m_vertical_tabs_content))
    , m_vertical_tab_bar_column(new QWidget(this))
    , m_vertical_tabs_resize_handle(new QWidget(this))
    , m_vertical_tabs_separator(new QWidget(this))
    , m_window_controls(new QWidget(this))
    , m_new_tab_button(new NewTabButton(*m_tab_bar, this))
    , m_vertical_tabs_hover_collapse_timer(new QTimer(this))
{
    setAcceptDrops(true);

    m_toolbar_container->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_toolbar_container->installEventFilter(this);

    m_page_column_layout = new QVBoxLayout(m_page_column);
    m_page_column_layout->setSpacing(0);
    m_page_column_layout->setContentsMargins(0, 0, 0, 0);

    m_tab_bar_row->setObjectName("LadybirdTabStrip");
    m_tab_bar_row_layout = new QHBoxLayout(m_tab_bar_row);
    m_tab_bar_row->installEventFilter(this);

    m_vertical_tabs_content_layout = new QHBoxLayout(m_vertical_tabs_content);
    m_vertical_tabs_content_layout->setSpacing(0);
    m_vertical_tabs_content_layout->setContentsMargins(0, 0, 0, 0);
    m_vertical_tabs_content->installEventFilter(this);

    m_vertical_tab_bar_column->setObjectName("LadybirdVerticalTabBar");
    m_vertical_tab_bar_column_layout = new QVBoxLayout(m_vertical_tab_bar_column);
    m_vertical_tab_bar_column->setProperty(VerticalTabsResizeHandleHoveredProperty, false);
    m_vertical_tab_bar_column->setProperty(VerticalTabsResizeHandleActiveProperty, false);
    m_vertical_tab_bar_column->installEventFilter(this);

    m_vertical_tabs_reserved_space->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);

    m_vertical_tabs_resize_handle->setObjectName("LadybirdVerticalTabsResizeHandle");
    m_vertical_tabs_resize_handle->setCursor(Qt::SizeHorCursor);
    m_vertical_tabs_resize_handle->installEventFilter(this);
    m_vertical_tabs_resize_handle->hide();

    m_vertical_tabs_separator->setObjectName("LadybirdVerticalTabsContentSeparator");
    m_vertical_tabs_separator->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_vertical_tabs_separator->hide();

    m_new_tab_button->setObjectName("LadybirdNewTabButton");
    m_new_tab_button->setIconSize({ 18, 18 });
    m_new_tab_button->setFixedSize(32, 32);
    m_new_tab_button->setFocusPolicy(Qt::NoFocus);
    m_new_tab_button->setToolTip("New Tab");
    m_new_tab_button->setAutoRaise(true);
    m_new_tab_button->installEventFilter(this);

    auto* window_controls_layout = new QHBoxLayout(m_window_controls);
    window_controls_layout->setContentsMargins(0, 0, 0, 0);
    window_controls_layout->setSpacing(0);
    m_minimize_window_button = createWindowButton(ChromeIcon::WindowMinimize, QStringLiteral("Minimize"));
    m_maximize_window_button = createWindowButton(ChromeIcon::WindowMaximize, QStringLiteral("Maximize"));
    m_close_window_button = createWindowButton(ChromeIcon::WindowClose, QStringLiteral("Close"));
    window_controls_layout->addWidget(m_minimize_window_button);
    window_controls_layout->addWidget(m_maximize_window_button);
    window_controls_layout->addWidget(m_close_window_button);
    connect(m_minimize_window_button, &QToolButton::clicked, this, [this] { window()->showMinimized(); });
    connect(m_maximize_window_button, &QToolButton::clicked, this, [this] {
        window()->isMaximized() ? window()->showNormal() : window()->showMaximized();
        m_maximize_window_button->setIcon(create_chrome_icon(window()->isMaximized() ? ChromeIcon::WindowRestore : ChromeIcon::WindowMaximize, palette()));
    });
    connect(m_close_window_button, &QToolButton::clicked, this, [this] { window()->close(); });

    m_vertical_tabs_hover_collapse_timer->setInterval(VerticalTabsHoverCollapsePollIntervalMs);
    connect(m_vertical_tabs_hover_collapse_timer, &QTimer::timeout, this, [this] {
        updateVerticalTabsHoverExpanded();
    });

    m_main_layout = new QVBoxLayout(this);
    m_main_layout->setSpacing(0);
    m_main_layout->setContentsMargins(0, 0, 0, 0);

    connect(m_tab_bar, &QTabBar::currentChanged, this, [this](int index) {
        if (index >= 0 && index < m_stack->count())
            m_stack->setCurrentIndex(index);
        if (index >= 0 && index < m_toolbar_container->count())
            m_toolbar_container->setCurrentIndex(index);
        updateVerticalTabsOverlayGeometry();
        updateVerticalTabsResizeHandle();
        if (onCurrentChanged)
            onCurrentChanged(index);
    });
    connect(m_tab_bar, &QTabBar::tabCloseRequested, this, [this](int index) {
        if (onTabCloseRequested)
            onTabCloseRequested(index);
    });
    connect(m_tab_bar, &QTabBar::tabMoved, this, [this](int from, int to) {
        auto* widget = m_stack->widget(from);
        m_stack->removeWidget(widget);
        m_stack->insertWidget(to, widget);
        auto* toolbar = static_cast<Tab*>(widget)->toolbarContainer();
        m_toolbar_container->removeWidget(toolbar);
        m_toolbar_container->insertWidget(to, toolbar);
        m_stack->setCurrentIndex(to);
        m_toolbar_container->setCurrentIndex(to);
    });

    recreateIcons();
    rebuildLayout();
    updateChromeStyle();
}

int TabWidget::addTab(Tab* tab, QString const& label)
{
    auto index = m_tab_bar->addTab(label);
    m_stack->insertWidget(index, tab);
    tab->setToolbarContainerInTabLayout(false);
    m_toolbar_container->insertWidget(index, tab->toolbarContainer());
    tab->setVerticalTabsEnabled(m_vertical_tabs_enabled);
    if (m_tab_bar->count() == 1)
        m_tab_bar->setCurrentIndex(0);
    updateTabLayout();
    return index;
}

void TabWidget::removeTab(int index)
{
    auto* widget = m_stack->widget(index);
    if (!widget)
        return;
    auto* tab = static_cast<Tab*>(widget);
    m_toolbar_container->removeWidget(tab->toolbarContainer());
    m_stack->removeWidget(widget);
    m_tab_bar->removeTab(index);
    tab->setToolbarContainerInTabLayout(true);
    delete widget;
    updateTabLayout();
}

Tab* TabWidget::currentTab() const
{
    return tab(currentIndex());
}

Tab* TabWidget::tab(int index) const
{
    return static_cast<Tab*>(m_stack->widget(index));
}

int TabWidget::indexOf(Tab* tab) const
{
    return m_stack->indexOf(tab);
}

void TabWidget::setNewTabAction(QAction* action)
{
    disconnect(m_new_tab_button, &QToolButton::clicked, nullptr, nullptr);
    if (!action)
        return;
    connect(m_new_tab_button, &QToolButton::clicked, action, &QAction::trigger);
}

void TabWidget::setVerticalTabsEnabled(bool enabled)
{
    if (m_vertical_tabs_enabled == enabled)
        return;
    m_vertical_tabs_enabled = enabled;
    if (!enabled)
        setVerticalTabsHoverExpanded(false);
    for (int i = 0; i < m_stack->count(); ++i)
        tab(i)->setVerticalTabsEnabled(enabled);
    rebuildLayout();
}

void TabWidget::setVerticalTabsExpanded(bool expanded)
{
    if (m_vertical_tabs_expanded == expanded)
        return;
    m_vertical_tabs_expanded = expanded;
    setVerticalTabsHoverExpanded(false);
    rebuildLayout();
}

void TabWidget::setVerticalTabsExpandOnHover(bool expand_on_hover)
{
    if (m_vertical_tabs_expand_on_hover == expand_on_hover)
        return;
    m_vertical_tabs_expand_on_hover = expand_on_hover;
    if (!expand_on_hover)
        setVerticalTabsHoverExpanded(false);
    rebuildLayout();
}

void TabWidget::setTabLayout(TabLayout layout)
{
    setVerticalTabsEnabled(layout != TabLayout::Horizontal);
    setVerticalTabsExpanded(layout == TabLayout::VerticalExpanded);
    refreshTabLayout();
}

void TabWidget::refreshTabLayout()
{
    m_tab_bar->refreshTabLayout();
    updateTabLayout();
}

bool TabWidget::event(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange) {
        recreateIcons();
        updateChromeStyle();
    } else if (event->type() == QEvent::Leave) {
        deferUpdateVerticalTabsHoverExpanded();
    }
    return QWidget::event(event);
}

bool TabWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_vertical_tabs_resize_handle) {
        auto reset_resize = [this] {
            m_is_resizing_vertical_tabs = false;
            m_vertical_tabs_resize_handle->releaseMouse();
            QApplication::restoreOverrideCursor();
            setResizeHandleProperty(VerticalTabsResizeHandleActiveProperty, false);
        };

        if (event->type() == QEvent::Enter) {
            setResizeHandleProperty(VerticalTabsResizeHandleHoveredProperty, true);
        } else if (event->type() == QEvent::Leave) {
            if (!m_is_resizing_vertical_tabs)
                setResizeHandleProperty(VerticalTabsResizeHandleHoveredProperty, false);
        } else if (event->type() == QEvent::MouseButtonDblClick) {
            auto* mouse_event = static_cast<QMouseEvent*>(event);
            if (mouse_event->button() == Qt::LeftButton && m_vertical_tabs_expanded) {
                applyVerticalTabsExpandedWidth(VerticalTabsDefaultExpandedWidth);
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonPress) {
            auto* mouse_event = static_cast<QMouseEvent*>(event);
            if (mouse_event->button() != Qt::LeftButton)
                return QWidget::eventFilter(watched, event);
            m_is_resizing_vertical_tabs = true;
            m_vertical_tabs_resize_start_global_x = mouse_event->globalPosition().toPoint().x();
            m_vertical_tabs_resize_start_width = m_vertical_tabs_expanded_width;
            m_vertical_tabs_resize_handle->grabMouse();
            QApplication::setOverrideCursor(Qt::SizeHorCursor);
            setResizeHandleProperty(VerticalTabsResizeHandleActiveProperty, true);
            return true;
        } else if (event->type() == QEvent::MouseMove) {
            if (!m_is_resizing_vertical_tabs)
                return QWidget::eventFilter(watched, event);
            auto* mouse_event = static_cast<QMouseEvent*>(event);
            auto delta = mouse_event->globalPosition().toPoint().x() - m_vertical_tabs_resize_start_global_x;
            applyVerticalTabsExpandedWidth(m_vertical_tabs_resize_start_width + delta);
            return true;
        } else if (event->type() == QEvent::MouseButtonRelease) {
            if (!m_is_resizing_vertical_tabs)
                return QWidget::eventFilter(watched, event);
            auto* mouse_event = static_cast<QMouseEvent*>(event);
            if (mouse_event->button() != Qt::LeftButton)
                return QWidget::eventFilter(watched, event);
            reset_resize();
            setResizeHandleProperty(VerticalTabsResizeHandleHoveredProperty, m_vertical_tabs_resize_handle->underMouse());
            return true;
        }
    }

    auto is_hover_target = watched == m_vertical_tab_bar_column || watched == m_tab_bar || watched == m_new_tab_button;
    if (is_hover_target) {
        if (event->type() == QEvent::Enter)
            setVerticalTabsHoverExpanded(true);
        else if (event->type() == QEvent::Leave)
            deferUpdateVerticalTabsHoverExpanded();
    }
    if (watched == m_vertical_tabs_content && event->type() == QEvent::Leave)
        deferUpdateVerticalTabsHoverExpanded();
    return QWidget::eventFilter(watched, event);
}

void TabWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateTabLayout();
}

TabLayout TabWidget::currentTabLayout() const
{
    if (!m_vertical_tabs_enabled)
        return TabLayout::Horizontal;
    return verticalTabsEffectivelyExpanded() ? TabLayout::VerticalExpanded : TabLayout::VerticalCollapsed;
}

bool TabWidget::verticalTabsEffectivelyExpanded() const
{
    return m_vertical_tabs_expanded || m_vertical_tabs_hover_expanded;
}

bool TabWidget::canExpandVerticalTabsOnHover() const
{
    return m_vertical_tabs_enabled && m_vertical_tabs_expand_on_hover && !m_vertical_tabs_expanded;
}

bool TabWidget::cursorIsOverVerticalTabs() const
{
    if (!m_vertical_tabs_content->isVisible())
        return false;
    if (m_vertical_tab_bar_column->underMouse() || m_tab_bar->underMouse() || m_new_tab_button->underMouse())
        return true;
    auto rect = QRect {
        m_vertical_tabs_content->mapToGlobal(QPoint { 0, 0 }),
        QSize { currentVerticalTabsWidth(), m_vertical_tabs_content->height() }
    };
    return window()->underMouse() && rect.contains(QCursor::pos());
}

int TabWidget::verticalTabsLayoutWidth() const
{
    return m_vertical_tabs_expanded ? m_vertical_tabs_expanded_width : VerticalTabsCollapsedWidth;
}

int TabWidget::currentVerticalTabsWidth() const
{
    return verticalTabsEffectivelyExpanded() ? m_vertical_tabs_expanded_width : VerticalTabsCollapsedWidth;
}

int TabWidget::verticalTabsTabWidth() const
{
    return std::max(0, currentVerticalTabsWidth() - verticalTabsHorizontalMarginWidth(m_tab_bar->tabLayout()));
}

void TabWidget::clearLayout(QBoxLayout* layout)
{
    while (auto* item = layout->takeAt(0))
        delete item;
}

void TabWidget::rebuildLayout()
{
    clearLayout(m_main_layout);
    clearLayout(m_tab_bar_row_layout);
    clearLayout(m_page_column_layout);
    clearLayout(m_vertical_tabs_content_layout);
    clearLayout(m_vertical_tab_bar_column_layout);

    m_tab_bar->setTabLayout(currentTabLayout());
    if (m_tab_bar->tabLayout() == TabLayout::Horizontal) {
        rebuildLayoutForHorizontalTabs();
        rebuildPageColumn();
        m_main_layout->addWidget(m_tab_bar_row);
        m_main_layout->addWidget(m_page_column, 1);
        m_page_column->show();
        m_vertical_tabs_content->hide();
    } else {
        rebuildLayoutForVerticalTabs();
        m_main_layout->addWidget(m_toolbar_container);
        m_vertical_tabs_content_layout->addWidget(m_vertical_tabs_reserved_space);
        m_vertical_tabs_content_layout->addWidget(m_stack, 1);
        m_main_layout->addWidget(m_vertical_tabs_content, 1);
        m_page_column->hide();
        m_vertical_tabs_content->show();
    }

    updateTabChromeVisibility();
    updateTabLayout();
}

void TabWidget::rebuildLayoutForHorizontalTabs()
{
    m_tab_bar_row->setMinimumHeight(HorizontalTabStripHeight);
    m_tab_bar_row_layout->setSpacing(0);
    m_tab_bar_row_layout->setContentsMargins(12, 2, 4, 1);
    setDynamicPropertyIfNeeded(*m_new_tab_button, VerticalTabsExpandedProperty, false);
    setDynamicPropertyIfNeeded(*m_new_tab_button, VerticalTabsButtonProperty, false);
    m_new_tab_button->setText({});
    m_new_tab_button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_new_tab_button->setFixedSize(HorizontalTabHeight, HorizontalTabHeight);
    m_tab_bar_row_layout->addWidget(m_tab_bar);
    m_tab_bar_row_layout->addWidget(m_new_tab_button, 0, Qt::AlignVCenter);
    m_tab_bar_row_layout->addStretch(1);
    m_window_controls->setVisible(use_right_custom_window_controls());
    if (use_right_custom_window_controls())
        m_tab_bar_row_layout->addWidget(m_window_controls, 0, Qt::AlignVCenter);
}

void TabWidget::rebuildLayoutForVerticalTabs()
{
    m_vertical_tabs_reserved_space->setFixedWidth(verticalTabsLayoutWidth());
    m_vertical_tab_bar_column->setFixedWidth(currentVerticalTabsWidth());
    m_vertical_tab_bar_column_layout->setSpacing(0);
    auto side_margin = verticalTabsSideMargin(m_tab_bar->tabLayout() != TabLayout::VerticalCollapsed);
    m_vertical_tab_bar_column_layout->setContentsMargins(side_margin, VerticalTabsTopMargin, side_margin, 8);
    updateVerticalTabsButtonLayout();
    m_window_controls->setVisible(false);
    m_vertical_tab_bar_column_layout->addWidget(m_tab_bar);
    m_vertical_tab_bar_column_layout->addWidget(m_new_tab_button);
    m_vertical_tab_bar_column_layout->addStretch(1);
}

void TabWidget::rebuildPageColumn()
{
    m_page_column_layout->addWidget(m_toolbar_container);
    m_page_column_layout->addWidget(m_stack, 1);
}

void TabWidget::updateTabLayout()
{
    if (m_tab_bar->tabLayout() != TabLayout::Horizontal) {
        m_vertical_tabs_reserved_space->setFixedWidth(verticalTabsLayoutWidth());
        m_vertical_tab_bar_column->setFixedWidth(currentVerticalTabsWidth());
        updateVerticalTabsButtonLayout();
        updateVerticalTabsOverlayGeometry();
        m_tab_bar->setAvailableWidth(verticalTabsTabWidth());
        updateVerticalTabsResizeHandle();
        updateVerticalTabsSeparator();
        return;
    }

    updateVerticalTabsResizeHandle();
    updateVerticalTabsSeparator();
    auto available_for_tabs = width() - m_new_tab_button->width() - 36;
    m_tab_bar->setAvailableWidth(available_for_tabs);
    auto tab_bar_width = std::min(available_for_tabs, m_tab_bar->count() * HorizontalTabMaxWidth);
    m_tab_bar->setFixedWidth(std::max(0, tab_bar_width));
}

void TabWidget::updateTabChromeVisibility()
{
    auto is_horizontal = m_tab_bar->tabLayout() == TabLayout::Horizontal;
    m_tab_bar_row->setVisible(is_horizontal);
    m_vertical_tab_bar_column->setVisible(!is_horizontal);
    updateVerticalTabsSeparator();
}

void TabWidget::updateVerticalTabsButtonLayout()
{
    auto expanded = verticalTabsEffectivelyExpanded();
    m_new_tab_button->setToolButtonStyle(expanded ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
    m_new_tab_button->setText(expanded ? QStringLiteral("New Tab") : QString {});
    setDynamicPropertyIfNeeded(*m_new_tab_button, VerticalTabsButtonProperty, true);
    setDynamicPropertyIfNeeded(*m_new_tab_button, VerticalTabsExpandedProperty, expanded);
    auto button_size = QSize { verticalTabsTabWidth(), VerticalTabHeight };
    m_new_tab_button->setFixedSize(button_size);
}

void TabWidget::updateVerticalTabsOverlayGeometry()
{
    if (m_tab_bar->tabLayout() == TabLayout::Horizontal) {
        m_vertical_tab_bar_column->hide();
        return;
    }
    auto chrome_height = m_toolbar_container->height();
    m_vertical_tab_bar_column->setGeometry(0, chrome_height, currentVerticalTabsWidth(), std::max(0, height() - chrome_height));
    m_vertical_tab_bar_column->show();
    m_vertical_tab_bar_column->raise();
}

void TabWidget::updateVerticalTabsResizeHandle()
{
    auto show_handle = m_tab_bar->tabLayout() != TabLayout::Horizontal && m_vertical_tabs_expanded;
    m_vertical_tabs_resize_handle->setVisible(show_handle);
    if (!show_handle) {
        m_vertical_tabs_resize_handle->releaseMouse();
        if (m_is_resizing_vertical_tabs)
            QApplication::restoreOverrideCursor();
        m_is_resizing_vertical_tabs = false;
        setResizeHandleProperty(VerticalTabsResizeHandleHoveredProperty, false);
        setResizeHandleProperty(VerticalTabsResizeHandleActiveProperty, false);
        return;
    }
    auto chrome_height = m_toolbar_container->height();
    auto divider_x = verticalTabsLayoutWidth() - 1;
    m_vertical_tabs_resize_handle->setGeometry(divider_x - (VerticalTabsResizeHitAreaWidth / 2), chrome_height, VerticalTabsResizeHitAreaWidth, std::max(0, height() - chrome_height));
    m_vertical_tabs_resize_handle->raise();
}

void TabWidget::updateVerticalTabsSeparator()
{
    auto show_separator = m_tab_bar->tabLayout() != TabLayout::Horizontal;
    m_vertical_tabs_separator->setVisible(show_separator);
    if (!show_separator)
        return;
    auto separator_x = std::max(0, currentVerticalTabsWidth() - 1);
    auto separator_y = std::max(0, m_toolbar_container->height() - 1);
    m_vertical_tabs_separator->setGeometry(separator_x, separator_y, std::max(0, width() - separator_x), 1);
    m_vertical_tabs_separator->raise();
}

void TabWidget::applyVerticalTabsExpandedWidth(int width)
{
    auto clamped_width = clampVerticalTabsExpandedWidth(width);
    if (m_vertical_tabs_expanded_width == clamped_width)
        return;
    m_vertical_tabs_expanded_width = clamped_width;
    updateTabLayout();
}

void TabWidget::setResizeHandleProperty(char const* property, bool enabled)
{
    setDynamicPropertyIfNeeded(*m_vertical_tab_bar_column, property, enabled);
}

void TabWidget::setVerticalTabsHoverExpanded(bool expanded)
{
    expanded &= canExpandVerticalTabsOnHover();
    if (m_vertical_tabs_hover_expanded == expanded)
        return;
    m_vertical_tabs_hover_expanded = expanded;
    if (expanded)
        m_vertical_tabs_hover_collapse_timer->start();
    else
        m_vertical_tabs_hover_collapse_timer->stop();
    m_tab_bar->setTabLayout(currentTabLayout());
    auto side_margin = verticalTabsSideMargin(m_tab_bar->tabLayout() != TabLayout::VerticalCollapsed);
    m_vertical_tab_bar_column_layout->setContentsMargins(side_margin, VerticalTabsTopMargin, side_margin, 8);
    updateTabLayout();
}

void TabWidget::deferUpdateVerticalTabsHoverExpanded()
{
    if (m_vertical_tabs_hover_expanded)
        updateVerticalTabsHoverExpanded();
}

void TabWidget::updateVerticalTabsHoverExpanded()
{
    if (!m_vertical_tabs_hover_expanded)
        return;
    if (!cursorIsOverVerticalTabs())
        setVerticalTabsHoverExpanded(false);
}

void TabWidget::recreateIcons()
{
    m_new_tab_button->setIcon(create_chrome_icon(ChromeIcon::NewTab, palette()));
    m_minimize_window_button->setIcon(create_chrome_icon(ChromeIcon::WindowMinimize, palette()));
    m_maximize_window_button->setIcon(create_chrome_icon(window() && window()->isMaximized() ? ChromeIcon::WindowRestore : ChromeIcon::WindowMaximize, palette()));
    m_close_window_button->setIcon(create_chrome_icon(ChromeIcon::WindowClose, palette()));
}

QToolButton* TabWidget::createWindowButton(ChromeIcon icon, QString const& tooltip)
{
    auto* button = new QToolButton(m_window_controls);
    button->setObjectName(icon == ChromeIcon::WindowClose ? "LadybirdCloseWindowButton" : "LadybirdWindowButton");
    button->setIcon(create_chrome_icon(icon, palette()));
    button->setIconSize({ 18, 18 });
    button->setFixedSize(40, 40);
    button->setFocusPolicy(Qt::NoFocus);
    button->setAutoRaise(true);
    button->setToolTip(tooltip);
    return button;
}

void TabWidget::updateChromeStyle()
{
    if (m_is_updating_chrome_style)
        return;
    m_is_updating_chrome_style = true;
    auto style_sheet = ChromeStyle::tab_widget_style_sheet(palette());
    setStyleSheet(style_sheet);
    m_vertical_tabs_separator->setStyleSheet(style_sheet);
    m_vertical_tabs_resize_handle->setStyleSheet(style_sheet);
    m_is_updating_chrome_style = false;
}

}
