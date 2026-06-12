/*
 * Copyright (c) 2024-2026, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Ladybird:
 *   UI/Qt/TabBar.cpp
 *   UI/Qt/ChromeLayout.h
 */
#include "TabBar.h"

#include "BrowserWindow.h"
#include "ChromeStyle.h"
#include "ChromeLayout.h"
#include "NewTabTrace.h"
#include "Icon.h"
#include "Tab.h"
#include "WebContentView.h"

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
#include <QLinearGradient>
#include <QMouseEvent>
#include <QDrag>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPixmap>
#include <QFontMetrics>
#include <QApplication>
#include <QContextMenuEvent>
#include <QMenu>
#include <QPointer>
#include <QStyle>
#include <QToolButton>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <chrono>

// TEMPORARY DIAGNOSTICS (SERVOQ_DIAG) — defined in WebContentView.cpp.
bool servoq_diag_enabled();
QString servoq_diag_describe(QObject const* o);
void servoq_diag_log(QString const& msg);

namespace {
// Monotonic millisecond clock for the hover-expand freeze investigation, so the
// log shows "tab switch -> first accepted click" gaps. steady_clock is allowed
// here (this is plain C++, not a workflow script).
qint64 servoq_diag_now_ms()
{
    static auto const start = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
}
} // namespace

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

QColor selectedTabShadow(QPalette const& palette, int layer)
{
    auto dark = ChromeStyle::is_dark(palette);
    auto color = QColor(0, 0, 0);
    if (layer == 0)
        color.setAlpha(dark ? 112 : 22);
    else
        color.setAlpha(dark ? 50 : 10);
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
    , m_tab_widget(parent)
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
        updateTabButtonGeometry();
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
    updateTabButtonGeometry();
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
    updateTabButtonGeometry();
}

// recalculated (initial show, tab insert/remove, layout mode change). Without this
// override the close button stays at its construction-time position until the next
// resizeEvent or hover, causing the "wrong position on first render" bug.
void TabBar::tabLayoutChange()
{
    QTabBar::tabLayoutChange();
    setVerticalScrollOffset(m_vertical_scroll_offset);
    updateTabButtonGeometry();
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
            painter.setPen(Qt::NoPen);
            painter.setBrush(selectedTabShadow(palette(), 1));
            painter.drawPath(shape.translated(0, 2));
            painter.setBrush(selectedTabShadow(palette(), 0));
            painter.drawPath(shape.translated(0, 1));

            auto selected_gradient = QLinearGradient(shape_rect.topLeft(), shape_rect.bottomLeft());
            selected_gradient.setColorAt(0.0, ChromeStyle::chrome_active_tab_surface_top(palette()));
            selected_gradient.setColorAt(1.0, ChromeStyle::chrome_active_tab_surface_bottom(palette()));
            painter.setBrush(selected_gradient);
            painter.setPen(QPen(selectedTabBorder(palette(), is_collapsed), 1));
            painter.drawPath(shape);
        } else if (hover_progress > 0.0) {
            painter.setBrush(tabHoverSurface(palette(), hover_progress));
            painter.setPen(Qt::NoPen);
            painter.drawPath(shape);
        }

        if (!selected && hover_progress <= 0.0 && index > 0 && index != currentIndex() + 1 && !is_collapsed) {
            auto separator = ChromeStyle::chrome_border(palette());
            separator.setAlpha(ChromeStyle::is_dark(palette()) ? 24 : 20);
            painter.setPen(separator);
            if (is_vertical)
                painter.drawLine(QPoint(tab_rect.left() + 16, tab_rect.top()), QPoint(tab_rect.right() - 16, tab_rect.top()));
            else
                painter.drawLine(QPoint(tab_rect.left(), 15), QPoint(tab_rect.left(), height() - 15));
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

        auto tab_font = font();
        if (selected)
            tab_font.setWeight(QFont::DemiBold);
        painter.setFont(tab_font);

        auto tab_text_color = selected ? text_color : muted_text;
        if (!selected)
            tab_text_color.setAlpha(hover_progress > 0.0 ? (ChromeStyle::is_dark(palette()) ? 236 : 228) : (ChromeStyle::is_dark(palette()) ? 226 : 216));
        painter.setPen(tab_text_color);
        QFontMetrics metrics(tab_font);
        auto title = metrics.elidedText(tabText(index), Qt::ElideRight, std::max(0, contents_rect.width()));
        painter.drawText(contents_rect, Qt::AlignLeft | Qt::AlignVCenter, title);
    }

    if (m_drop_indicator_index >= 0 && count() > 0) {
        auto indicator_color = ChromeStyle::chrome_accent(palette());
        indicator_color.setAlpha(220);
        painter.setPen(QPen(indicator_color, 3, Qt::SolidLine, Qt::RoundCap));

        if (is_vertical) {
            auto indicator_y = m_drop_indicator_index >= count()
                ? visualTabRect(count() - 1).bottom() + 3
                : visualTabRect(m_drop_indicator_index).top() + 1;
            indicator_y = std::max(3, std::min(height() - 3, indicator_y));
            painter.drawLine(QPointF(10, indicator_y), QPointF(width() - 10, indicator_y));
            return;
        }

        auto indicator_x = m_drop_indicator_index >= count()
            ? visualTabRect(count() - 1).right() + 3
            : visualTabRect(m_drop_indicator_index).left() + 1;
        indicator_x = std::max(2, std::min(width() - 3, indicator_x));
        painter.drawLine(QPointF(indicator_x, 8), QPointF(indicator_x, height() - 6));
    }
}

void TabBar::leaveEvent(QEvent* event)
{
    setHoveredTabIndex(-1);
    QTabBar::leaveEvent(event);
}

void TabBar::mouseDoubleClickEvent(QMouseEvent* event)
{
    // Use tabIndexAt() instead of tabAt() so hit-testing is consistent with
    // mousePressEvent in vertical mode. Qt's internal tabAt() uses QTabBar's own
    // rect calculations which don't match our custom vertical layout; returning -1
    // for a position that visually lands on a tab causes new-tab creation on double-click.
    if (event->button() == Qt::LeftButton && tabIndexAt(event->pos()) == -1) {
        if (m_tab_widget && m_tab_widget->onNewTabRequested) {
            m_tab_widget->onNewTabRequested();
            return;
        }
    }
    QTabBar::mouseDoubleClickEvent(event);
}

void TabBar::mousePressEvent(QMouseEvent* event)
{
    // Middle-click closes a tab. We must NOT close on press: emitting the close
    // here mutated the tab bar mid-interaction — the deferred close shifted the
    // next tab under the cursor and a second close fired against the now-stale
    // index, closing two tabs. Instead, on press we only *capture* the target by
    // stable identity (QPointer<Tab>, survives reordering and index shifts) and
    // swallow the event (no base-class handling). The actual close happens once,
    // on release. (See mouseReleaseEvent.)
    if (event->button() == Qt::MiddleButton) {
        int index = tabIndexAt(event->pos());
        m_middle_close_target = (index >= 0 && m_tab_widget) ? m_tab_widget->tab(index) : nullptr;
        if (servoq_diag_enabled())
            servoq_diag_log(QStringLiteral("TabBar::middle_press index=%1 captured=%2")
                .arg(index).arg(m_middle_close_target ? 1 : 0));
        event->accept();
        return;
    }
    if (servoq_diag_enabled() && event->button() == Qt::LeftButton)
        servoq_diag_log(QStringLiteral("TabBar::mousePressEvent DELIVERED customTabAt=%1 qtTabAt=%2 currentIndex=%3 pos=(%4,%5) | %6")
            .arg(tabIndexAt(event->pos()))
            .arg(QTabBar::tabAt(event->pos()))
            .arg(currentIndex())
            .arg(event->pos().x()).arg(event->pos().y())
            .arg(m_tab_widget ? m_tab_widget->hoverDiagState() : QStringLiteral("<no tab_widget>")));
    if (event->button() == Qt::LeftButton) {
        m_pressed_tab_index = tabIndexAt(event->pos());
        m_drag_start_position = event->pos();
        // Activate via our vertical-layout-aware hit test instead of relying on
        // QTabBar::mousePressEvent's internal tabAt(). That internal hit test
        // returns -1 for valid positions right after the hover-expand popup is
        // reparented (stale internal tab geometry) and silently ignores the
        // click — the proven cause of the ~1 s post-switch tab-bar freeze.
        // Our tabIndexAt() stays correct across the reparent, so drive the
        // activation from it. Idempotent: skipped when the tab is already current,
        // and drag-to-reorder is unaffected (handled in mouseMoveEvent).
        if (m_pressed_tab_index >= 0 && m_pressed_tab_index != currentIndex())
            setCurrentIndex(m_pressed_tab_index);
        if (qEnvironmentVariableIsSet("SERVOQ_DEBUG")) {
            qInfo().nospace()
                << "SERVOQ_DEBUG tab_bar_press"
                << " tab_at=" << m_pressed_tab_index
                << " current=" << currentIndex()
                << " pos=(" << event->pos().x() << "," << event->pos().y() << ")";
        }
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
    // Middle-click close, completed here so the bar is only mutated once the
    // mouse interaction is over. Resolve the captured identity back to its
    // current index and emit the same tabCloseRequested signal the close button
    // uses (BrowserWindow defers the real close via QTimer::singleShot — no
    // synchronous widget deletion / removeTab from this handler). The target is
    // cleared *before* emitting so any re-entrant event cannot close twice.
    if (event->button() == Qt::MiddleButton) {
        QPointer<Tab> target = m_middle_close_target;
        m_middle_close_target = nullptr;
        int index = (target && m_tab_widget) ? m_tab_widget->indexOf(target) : -1;
        if (servoq_diag_enabled())
            servoq_diag_log(QStringLiteral("TabBar::middle_release resolved_index=%1 stale=%2")
                .arg(index).arg(target && index < 0 ? 1 : 0));
        if (index >= 0)
            emit tabCloseRequested(index);
        event->accept();
        return;
    }
    int released_tab_index = tabIndexAt(event->pos());
    int pressed_tab_index = m_pressed_tab_index;
    if (servoq_diag_enabled() && event->button() == Qt::LeftButton)
        servoq_diag_log(QStringLiteral("TabBar::mouseReleaseEvent DELIVERED tab_at=%1 pressed=%2 current=%3 | %4")
            .arg(released_tab_index).arg(pressed_tab_index).arg(currentIndex())
            .arg(m_tab_widget ? m_tab_widget->hoverDiagState() : QStringLiteral("<no tab_widget>")));
    if (event->button() == Qt::LeftButton && qEnvironmentVariableIsSet("SERVOQ_DEBUG")) {
        qInfo().nospace()
            << "SERVOQ_DEBUG tab_bar_release"
            << " tab_at=" << released_tab_index
            << " pressed=" << pressed_tab_index
            << " current=" << currentIndex()
            << " pos=(" << event->pos().x() << "," << event->pos().y() << ")";
    }
    m_pressed_tab_index = -1;
    QTabBar::mouseReleaseEvent(event);

    if (event->button() == Qt::LeftButton && qEnvironmentVariableIsSet("SERVOQ_DEBUG")) {
        qInfo().nospace()
            << "SERVOQ_DEBUG tab_bar_release_after_base"
            << " tab_at=" << released_tab_index
            << " pressed=" << pressed_tab_index
            << " current=" << currentIndex();
    }
}

void TabBar::contextMenuEvent(QContextMenuEvent* event)
{
    int index = tabIndexAt(event->pos());
    if (index < 0)
        return;

    auto* tab_widget = m_tab_widget;
    auto* browser_window = dynamic_cast<BrowserWindow*>(tab_widget ? tab_widget->parent() : nullptr);
    if (!browser_window)
        return;

    auto* tab = tab_widget->tab(index);
    if (!tab)
        return;

    // All lambdas capture QPointer<Tab> so they are no-ops if the tab is
    // deleted while the menu is open. Use popup() instead of exec() to avoid
    // starting a blocking nested event loop: exec() causes the Wayland compositor
    // to stop delivering pointer events to the main window surface after the
    // popup closes, leaving the tab bar in a "locked" state where clicks are
    // ignored until something forces a focus re-evaluation.
    QPointer<Tab> tab_ptr = tab;
    QPointer<TabWidget> tab_widget_ptr = tab_widget;

    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    auto* reload_action = menu->addAction("Reload Tab");
    connect(reload_action, &QAction::triggered, tab, [tab] {
        tab->navigate(tab->url());
    });

    menu->addSeparator();

    auto* duplicate_action = menu->addAction("Duplicate Tab");
    connect(duplicate_action, &QAction::triggered, browser_window, [browser_window, tab_ptr] {
        if (!tab_ptr)
            return;
        browser_window->createNewTab(tab_ptr->url());
    });

    menu->addSeparator();

    auto* move_start_action = menu->addAction("Move Tab to Start");
    connect(move_start_action, &QAction::triggered, this, [this, tab_widget_ptr, tab_ptr] {
        if (!tab_widget_ptr || !tab_ptr)
            return;
        int idx = tab_widget_ptr->indexOf(tab_ptr);
        if (idx > 0)
            moveTab(idx, 0);
    });

    auto* move_end_action = menu->addAction("Move Tab to End");
    connect(move_end_action, &QAction::triggered, this, [this, tab_widget_ptr, tab_ptr] {
        if (!tab_widget_ptr || !tab_ptr)
            return;
        int idx = tab_widget_ptr->indexOf(tab_ptr);
        if (idx >= 0 && idx < count() - 1)
            moveTab(idx, count() - 1);
    });

    menu->addSeparator();

    auto* close_action = menu->addAction("Close Tab");
    connect(close_action, &QAction::triggered, browser_window, [browser_window, tab_widget_ptr, tab_ptr] {
        if (!tab_ptr || !tab_widget_ptr)
            return;
        int idx = tab_widget_ptr->indexOf(tab_ptr);
        if (idx >= 0)
            browser_window->closeTabFromContextMenu(idx);
    });

    auto* close_other_action = menu->addAction("Close Other Tabs");
    connect(close_other_action, &QAction::triggered, browser_window, [browser_window, tab_widget_ptr, tab_ptr] {
        if (!tab_ptr || !tab_widget_ptr)
            return;
        int idx = tab_widget_ptr->indexOf(tab_ptr);
        if (idx >= 0)
            browser_window->closeOtherTabs(idx);
    });

    auto* close_right_action = menu->addAction("Close Tabs to the Right");
    connect(close_right_action, &QAction::triggered, browser_window, [browser_window, tab_widget_ptr, tab_ptr] {
        if (!tab_ptr || !tab_widget_ptr)
            return;
        int idx = tab_widget_ptr->indexOf(tab_ptr);
        if (idx >= 0)
            browser_window->closeTabsToRight(idx);
    });

    close_other_action->setEnabled(tab_widget->count() > 1);
    close_right_action->setEnabled(index < tab_widget->count() - 1);

    menu->popup(event->globalPos());
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
    updateTabButtonGeometry();
    update();
}

void TabBar::updateTabButtonGeometry()
{
    auto prepare_button = [&](QWidget* button) {
        if (!button)
            return;
        button->installEventFilter(this);
    };

    auto place_expanded_button = [&](int index, QTabBar::ButtonPosition position, QRect shape_rect) {
        auto* button = tabButton(index, position);
        if (!button)
            return;
        prepare_button(button);
        auto should_be_visible = position != QTabBar::RightSide || index == currentIndex() || index == m_hovered_tab_index;
        bool did_update_button = false;
        if (button->isVisible() != should_be_visible) {
            button->setVisible(should_be_visible);
            did_update_button = true;
        }

        auto button_size = button->size();
        if (button_size.isEmpty())
            button_size = button->sizeHint();
        if (button_size.isEmpty())
            return;

        auto x = position == QTabBar::RightSide ? shape_rect.right() - button_size.width() - 6 : shape_rect.left() + 6;
        auto y = shape_rect.top() + ((shape_rect.height() - button_size.height()) / 2);
        QRect button_geometry { { x, y }, button_size };
        if (button->geometry() != button_geometry) {
            button->setGeometry(button_geometry);
            did_update_button = true;
        }
        if (did_update_button)
            button->raise();
    };

    auto place_collapsed_button = [&](int index, QTabBar::ButtonPosition position, QRect tab_rect, QRect shape_rect) {
        auto* button = tabButton(index, position);
        if (!button)
            return;
        prepare_button(button);
        auto should_be_visible = position == QTabBar::LeftSide || index == m_hovered_tab_index;
        bool did_update_button = false;
        if (button->isVisible() != should_be_visible) {
            button->setVisible(should_be_visible);
            did_update_button = true;
        }

        auto button_size = button->size();
        if (button_size.isEmpty())
            button_size = button->sizeHint();
        if (button_size.isEmpty())
            return;

        QPoint button_position;
        if (position == QTabBar::RightSide) {
            button_position = {
                std::max(tab_rect.left(), shape_rect.left() - 5),
                std::max(tab_rect.top(), shape_rect.top() - 5),
            };
        } else {
            auto x = std::min(tab_rect.right() - button_size.width() + 1, shape_rect.right() - button_size.width() + 5);
            auto y = std::min(tab_rect.bottom() - button_size.height() + 1, shape_rect.bottom() - button_size.height() + 5);
            button_position = { x, y };
        }

        QRect button_geometry { button_position, button_size };
        if (button->geometry() != button_geometry) {
            button->setGeometry(button_geometry);
            did_update_button = true;
        }
        if (did_update_button)
            button->raise();
    };

    for (int index = 0; index < count(); ++index) {
        auto tab_rect = visualTabRect(index);
        if (!tab_rect.isValid())
            continue;

        if (m_tab_layout == TabLayout::VerticalCollapsed) {
            auto shape_rect = collapsedVerticalTabShapeRect(tab_rect).toAlignedRect();
            place_collapsed_button(index, QTabBar::LeftSide, tab_rect, shape_rect);
            place_collapsed_button(index, QTabBar::RightSide, tab_rect, shape_rect);
        } else {
            auto shape_rect = (m_tab_layout == TabLayout::Horizontal ? horizontalTabCardShapeRect(tab_rect) : tabCardShapeRect(tab_rect)).toAlignedRect();
            place_expanded_button(index, QTabBar::LeftSide, shape_rect);
            place_expanded_button(index, QTabBar::RightSide, shape_rect);
        }
    }
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
        if (qEnvironmentVariableIsSet("SERVOQ_DEBUG")) {
            qInfo().nospace()
                << "SERVOQ_DEBUG tab_widget_current_changed index=" << index
                << " has_callback=" << (onCurrentChanged ? 1 : 0);
            dumpPresentationState("tab_widget_current_changed");
        }
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
        if (onTabsReordered)
            onTabsReordered();
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
    // Use deleteLater() instead of delete so the Tab and its children (WebContentView,
    // toolbars, timers) remain valid for the rest of the current event-delivery chain.
    // Synchronous delete here can free widgets while Qt's mouse-event dispatch still
    // holds a raw pointer to one of them, causing a vtable-corruption SIGSEGV.
    widget->deleteLater();
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
    if (m_main_window_filter_installed && watched == window() &&
        (event->type() == QEvent::Move || event->type() == QEvent::Resize)) {
        if (m_vertical_tab_bar_column->isWindow())
            updateVerticalTabsOverlayGeometry();
        return false;
    }

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
    if (servoq_diag_enabled() && is_hover_target) {
        auto t = event->type();
        if (t == QEvent::Enter || t == QEvent::Leave
            || t == QEvent::MouseButtonPress || t == QEvent::MouseButtonRelease) {
            char const* name = t == QEvent::Enter ? "Enter"
                : t == QEvent::Leave ? "Leave"
                : t == QEvent::MouseButtonPress ? "MousePress" : "MouseRelease";
            servoq_diag_log(QStringLiteral("eventFilter %1 on %2 | %3")
                .arg(QString::fromUtf8(name))
                .arg(servoq_diag_describe(watched))
                .arg(hoverDiagState()));
        }
    }
    if (is_hover_target) {
        if (event->type() == QEvent::Enter) {
            // When the floating window receives its first Enter, the Wayland compositor
            // has delivered wl_pointer.enter — underMouse() is now reliable. Clear the
            // transition guard so the 250ms collapse-poll timer can resume.
            if (watched == m_vertical_tab_bar_column && m_vertical_tab_bar_column->isWindow() && !m_tab_column_floating_entered) {
                m_tab_column_floating_entered = true;
                m_hover_expand_in_progress = false;
            }
            setVerticalTabsHoverExpanded(true);
        } else if (event->type() == QEvent::Leave)
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
    // When the tab column is a floating Qt::ToolTip window (hover-expanded), the cursor
    // is over a separate Wayland surface, so window()->underMouse() is false and the
    // underMouse() flags above may not have been set yet (the compositor delivers the
    // wl_pointer.enter asynchronously after show()). Check the global cursor position
    // against the floating window's rect directly to avoid a spurious collapse that would
    // cause rapid expand/collapse oscillation and a ~1 s click freeze on the tab bar.
    if (m_vertical_tab_bar_column->isWindow()) {
        // QCursor::pos() is unreliable on Wayland when the cursor is over a separate
        // wl_surface: it only updates when the compositor delivers wl_pointer.motion
        // to Qt's main surface, so it lags behind the actual position. Using it here
        // caused the 250 ms poll timer to see a stale "not over" position and collapse
        // the panel prematurely, creating a rapid expand/collapse oscillation.
        //
        // Instead: trust the event-driven underMouse() flag. It is set by the Enter
        // event that the floating window receives from the compositor once the cursor
        // actually enters it. Before the first such Enter is confirmed (tracked by
        // m_tab_column_floating_entered), assume the cursor is still over the panel —
        // we just expanded for a hover and the compositor hasn't delivered Enter yet.
        if (!m_tab_column_floating_entered)
            return true;
        return m_vertical_tab_bar_column->underMouse()
            || m_tab_bar->underMouse()
            || m_new_tab_button->underMouse();
    }
    auto rect = QRect {
        m_vertical_tabs_content->mapToGlobal(QPoint { 0, 0 }),
        QSize { currentVerticalTabsWidth(), m_vertical_tabs_content->height() }
    };
    return window()->underMouse() && rect.contains(QCursor::pos());
}

// TEMPORARY DIAGNOSTICS (SERVOQ_DIAG): full hover-expand state snapshot.
QString TabWidget::hoverDiagState() const
{
    return QStringLiteral(
        "t=%1ms current=%2 hover_enabled=%3 expanded=%4 hover_expanded=%5 floating(isWindow)=%6 "
        "in_progress=%7 floating_entered=%8 collapse_timer_active=%9 cursorIsOver=%10 "
        "underMouse[col=%11 tabbar=%12 newtab=%13] grabber=%14 cursor=(%15,%16)")
        .arg(servoq_diag_now_ms())
        .arg(m_tab_bar->currentIndex())
        .arg(canExpandVerticalTabsOnHover() ? 1 : 0)
        .arg(m_vertical_tabs_expanded ? 1 : 0)
        .arg(m_vertical_tabs_hover_expanded ? 1 : 0)
        .arg(m_vertical_tab_bar_column->isWindow() ? 1 : 0)
        .arg(m_hover_expand_in_progress ? 1 : 0)
        .arg(m_tab_column_floating_entered ? 1 : 0)
        .arg(m_vertical_tabs_hover_collapse_timer->isActive() ? 1 : 0)
        .arg(cursorIsOverVerticalTabs() ? 1 : 0)
        .arg(m_vertical_tab_bar_column->underMouse() ? 1 : 0)
        .arg(m_tab_bar->underMouse() ? 1 : 0)
        .arg(m_new_tab_button->underMouse() ? 1 : 0)
        .arg(servoq_diag_describe(QWidget::mouseGrabber()))
        .arg(QCursor::pos().x())
        .arg(QCursor::pos().y());
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
    m_new_tab_button->setText({});
    setDynamicPropertyIfNeeded(*m_new_tab_button, VerticalTabsExpandedProperty, false);
    setDynamicPropertyIfNeeded(*m_new_tab_button, VerticalTabsButtonProperty, true);
    m_new_tab_button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_new_tab_button->setFixedSize(HorizontalTabHeight, HorizontalTabHeight);
    m_new_tab_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
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
    auto panel_width = currentVerticalTabsWidth();
    auto panel_height = std::max(0, height() - chrome_height);
    if (m_vertical_tab_bar_column->isWindow()) {
        m_vertical_tab_bar_column->move(mapToGlobal(QPoint(0, chrome_height)));
        m_vertical_tab_bar_column->resize(panel_width, panel_height);
    } else {
        m_vertical_tab_bar_column->setGeometry(0, chrome_height, panel_width, panel_height);
    }
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
    if (servoq_diag_enabled())
        servoq_diag_log(QStringLiteral("setVerticalTabsHoverExpanded(req=%1) was=%2 | %3")
            .arg(expanded ? 1 : 0).arg(m_vertical_tabs_hover_expanded ? 1 : 0).arg(hoverDiagState()));
    if (m_vertical_tabs_hover_expanded == expanded)
        return;
    NewTabTraceScope trace_scope(expanded ? "hover_expand" : "hover_collapse");
    m_vertical_tabs_hover_expanded = expanded;
    m_hover_expand_in_progress = true;
    if (expanded) {
        // Save keyboard focus so we can restore it when the hover panel collapses.
        // On Wayland the floating tooltip window may disrupt compositor focus tracking;
        // saving here lets us put it back cleanly without guessing which widget owned it.
        m_saved_focus_before_hover_expand = QApplication::focusWidget();

        // Reset the "first Enter confirmed" flag. We must not collapse based on
        // QCursor::pos() or underMouse() until the Wayland compositor has delivered
        // wl_pointer.enter to the new floating surface — which happens asynchronously
        // after show(). cursorIsOverVerticalTabs() returns true while this is false.
        m_tab_column_floating_entered = false;

        // The tab column must be a floating window while hover-expanded so it
        // paints above the native embedded webview surface, which always renders
        // above ordinary child widgets regardless of z-order calls.
        //
        // Qt::ToolTip (with a parent) maps to xdg_popup on Wayland, which lets
        // xdg_positioner enforce the exact position.  Qt::Tool maps to
        // xdg_toplevel, whose position is compositor-controlled and ignored.
        // Keep the parent as window() so the compositor knows which surface to
        // anchor against when computing the popup offset.
        m_vertical_tab_bar_column->hide();
        m_vertical_tab_bar_column->setParent(window(), Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
        m_vertical_tab_bar_column->setAttribute(Qt::WA_ShowWithoutActivating);
        m_vertical_tabs_hover_collapse_timer->start();
        if (!m_main_window_filter_installed) {
            if (auto* w = window()) {
                w->installEventFilter(this);
                m_main_window_filter_installed = true;
            }
        }
    } else {
        m_tab_column_floating_entered = false;

        // setParent(parent, Qt::Widget) resets the window-type flags in one
        // call so isWindow() returns false and the overlay reverts to a normal
        // child widget positioned via setGeometry().
        m_vertical_tab_bar_column->hide();
        m_vertical_tab_bar_column->setParent(this, Qt::Widget);
        m_vertical_tabs_hover_collapse_timer->stop();
    }
    m_tab_bar->setTabLayout(currentTabLayout());
    auto side_margin = verticalTabsSideMargin(m_tab_bar->tabLayout() != TabLayout::VerticalCollapsed);
    m_vertical_tab_bar_column_layout->setContentsMargins(side_margin, VerticalTabsTopMargin, side_margin, 8);
    updateChromeStyle();
    updateTabLayout();

    if (!expanded) {
        // Collapse path: clear the guard now that layout is stable, and restore
        // keyboard focus to whichever widget had it before the panel appeared.
        m_hover_expand_in_progress = false;
        if (m_saved_focus_before_hover_expand) {
            m_saved_focus_before_hover_expand->setFocus(Qt::OtherFocusReason);
            m_saved_focus_before_hover_expand = nullptr;
        }
        // Queue a toplevel repaint so the collapse reaches the compositor in
        // one commit, including any pending shared-subsurface state (e.g. a
        // park from an empty-tab activation that happened while the floating
        // panel was open).
        if (auto* toplevel = window())
            toplevel->update();
        newtab_trace_point("hover_collapse_toplevel_update_queued");
    } else {
        // Expand path: keep m_hover_expand_in_progress = true until the floating
        // window receives its first wl_pointer.enter event (handled in eventFilter).
        // This prevents the 250ms collapse-poll timer from seeing stale QCursor::pos()
        // data and triggering a spurious collapse before the compositor has notified us.
        // Safety: clear after 500 ms in case the Enter event never arrives (cursor left
        // very quickly after the window appeared, before compositor could notify us).
        QTimer::singleShot(500, this, [this] {
            if (m_vertical_tabs_hover_expanded)
                m_hover_expand_in_progress = false;
        });
    }
}

void TabWidget::deferUpdateVerticalTabsHoverExpanded()
{
    // Suppress Leave events that fire synchronously during setVerticalTabsHoverExpanded
    // itself (from hide()/setParent() reparenting). At that moment the floating window
    // has not been shown/positioned yet, so cursorIsOverVerticalTabs() would see stale
    // geometry and trigger a spurious collapse → expand → collapse oscillation.
    if (m_hover_expand_in_progress)
        return;
    QTimer::singleShot(0, this, [this]() {
        updateVerticalTabsHoverExpanded();
    });
}

void TabWidget::updateVerticalTabsHoverExpanded()
{
    if (!m_vertical_tabs_hover_expanded)
        return;
    // Guard: do not poll while the expand transition is in progress. The floating
    // window has not yet received wl_pointer.enter from the compositor, so any
    // hover check would be based on stale state and would trigger a spurious collapse.
    if (m_hover_expand_in_progress) {
        if (servoq_diag_enabled())
            servoq_diag_log(QStringLiteral("updateVerticalTabsHoverExpanded POLL skipped(in_progress) | %1").arg(hoverDiagState()));
        return;
    }
    bool over = cursorIsOverVerticalTabs();
    if (servoq_diag_enabled())
        servoq_diag_log(QStringLiteral("updateVerticalTabsHoverExpanded POLL cursorIsOver=%1 -> %2 | %3")
            .arg(over ? 1 : 0)
            .arg(over ? QStringLiteral("keep") : QStringLiteral("COLLAPSE"))
            .arg(hoverDiagState()));
    if (!over)
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
    m_tab_bar_row->setStyleSheet(style_sheet);
    m_vertical_tab_bar_column->setStyleSheet(style_sheet);
    m_vertical_tabs_separator->setStyleSheet(style_sheet);
    m_vertical_tabs_resize_handle->setStyleSheet(style_sheet);
    m_is_updating_chrome_style = false;
}

// ── Wayland activation transaction ──────────────────────────────────────────

void TabWidget::updateContainerGeometry()
{
    if (auto* current = currentTab()) {
        if (auto* v = current->view())
            v->updateContainerGeometry();
    }
}

// activateTab is the sole entry point for Wayland container ownership transfers.
// BrowserWindow calls it from the QTabBar::currentChanged transaction so Qt's
// stack current page, Servo active tab, and the shared native Wayland owner do
// not drift across different mouse-event phases.
//
// TODO: background tabs on Wayland do not start their engine until activated
// (deferred in WebContentView::startEngineIfNeeded).  Background tabs appear
// blank until clicked.  Fix requires either software-renderer fallback or a
// dedicated off-screen Wayland surface per tab.
void TabWidget::activateTab(int index)
{
    if (index < 0 || index >= count()) {
        if (qEnvironmentVariableIsSet("SERVOQ_DEBUG"))
            qInfo().nospace() << "SERVOQ_DEBUG activate_tab_return_invalid_index index=" << index << " count=" << count();
        return;
    }

    auto* new_tab = tab(index);
    if (!new_tab) {
        if (qEnvironmentVariableIsSet("SERVOQ_DEBUG"))
            qInfo().nospace() << "SERVOQ_DEBUG activate_tab_return_null_tab index=" << index;
        return;
    }

    auto t0 = std::chrono::steady_clock::now();
    if (servoq_diag_enabled())
        servoq_diag_log(QStringLiteral(">>> activateTab ENTER index=%1 tab_id=%2 | %3")
            .arg(index).arg(new_tab->controllerId()).arg(hoverDiagState()));
    if (qEnvironmentVariableIsSet("SERVOQ_DEBUG")) {
        qInfo().nospace()
            << "SERVOQ_DEBUG activate_tab_enter index=" << index
            << " tab_id=" << new_tab->controllerId();
        dumpPresentationState("activate_tab_enter");
    }

    for (int i = 0; i < count(); ++i) {
        auto* t = tab(i);
        if (!t || t == new_tab)
            continue;
        // Only notify the actual owner. m_wayland_renderer_active is preserved
        // across inactive state, so checking waylandRendererActivePublic() would
        // fire for every previously-loaded tab — wasteful and incorrect.
        if (auto* v = t->view(); v && v->isCurrentWaylandOwner()) {
            NewTabTraceScope scope("onBecomeInactiveTab_old_owner", t->controllerId());
            v->onBecomeInactiveTab();
        }
    }

    // Do NOT call updateContainerGeometry() here. onBecomeInactiveTab() parks
    // the container off-screen; calling updateContainerGeometry() before
    // onBecomeActiveTab() immediately un-parks it to full size, causing the
    // previous tab's content to flash to the left of the window for one frame
    // (ghost rendering bug). onBecomeActiveTab() calls updateContainerGeometry()
    // internally after attaching the container to the new owner.

    if (auto* v = new_tab->view())
        v->onBecomeActiveTab();

    // NOTE: no hover-expand "freeze guard" here. A previous attempt reset
    // m_tab_column_floating_entered=false + m_hover_expand_in_progress=true (with a
    // 500 ms timer) on every tab switch, intending to suppress a collapse during the
    // Wayland surface churn. That was a misdiagnosis: the real tab-bar freeze was
    // QTabBar::mousePressEvent ignoring clicks via its stale internal tabAt() (now
    // fixed by activating from our own tabIndexAt() in TabBar::mousePressEvent).
    //
    // The guard also caused a real bug: with m_tab_column_floating_entered=false,
    // cursorIsOverVerticalTabs() returns true unconditionally, so after clicking a
    // tab the hover panel could not collapse until a fresh wl_pointer.enter re-set
    // the flag — which never happens when the cursor leaves into the webview
    // subsurface. Removing the guard lets the normal Leave/poll path collapse the
    // panel as soon as the cursor leaves it.
    if (servoq_diag_enabled())
        servoq_diag_log(QStringLiteral("activateTab after onBecomeActiveTab (no hover-guard) | %1").arg(hoverDiagState()));

    if (qEnvironmentVariableIsSet("SERVOQ_DEBUG")) {
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count();
        qInfo().nospace()
            << "SERVOQ_DEBUG activate_tab_done tab_id=" << new_tab->controllerId()
            << " elapsed_us=" << us;
        if (us > 100000)
            qWarning().nospace()
                << "SERVOQ_WARN activate_tab very slow: " << us << " us";
        dumpPresentationState("activate_tab_done");
    }
}

void TabWidget::dumpPresentationState(const char* reason, int activation_serial) const
{
    if (!qEnvironmentVariableIsSet("SERVOQ_DEBUG"))
        return;

    auto* owner = WebContentView::currentWaylandOwner();
    auto* container = WebContentView::sharedWaylandContainer();

    qInfo().nospace()
        << "SERVOQ_STATE [" << reason << "]"
        << " serial=" << activation_serial
        << " tab_bar_idx=" << m_tab_bar->currentIndex()
        << " stack_idx=" << m_stack->currentIndex()
        << " tab_count=" << m_tab_bar->count();

    for (int i = 0; i < m_stack->count(); ++i) {
        auto* t = tab(i);
        auto* v = t ? t->view() : nullptr;
        qInfo().nospace()
            << "  tab[" << i << "]"
            << " id=" << (t ? t->controllerId() : -1)
            << " url=" << (t ? t->url().left(60) : QStringLiteral("?"))
            << " has_view=" << (v ? 1 : 0)
            << " webview_created=" << (v ? v->webviewCreated() : 0)
            << " wayland_active=" << (v ? v->waylandRendererActivePublic() : 0)
            << " pending_present=" << (v ? v->waylandPresentPendingPublic() : 0)
            << " empty=" << (v ? v->isEmptyNewTab() : 0)
            << " is_owner=" << (v && v->isCurrentWaylandOwner() ? 1 : 0)
            << " is_stack_current=" << (m_stack->currentWidget() == t ? 1 : 0);
    }

    qInfo().nospace()
        << "  wayland_owner_ptr=" << (void*)owner
        << " owner_tab_id=" << (owner ? owner->tabId() : 0)
        << " container=" << (void*)container
        << " container_visible=" << (container ? (container->isVisible() ? 1 : 0) : -1);

    if (container) {
        auto c_geom = container->geometry();
        auto c_global = container->mapToGlobal(QPoint(0, 0));
        QRect c_global_rect(c_global, container->size());
        qInfo().nospace()
            << "  container_local=(" << c_geom.x() << "," << c_geom.y()
            << " " << c_geom.width() << "x" << c_geom.height() << ")"
            << " container_global=(" << c_global.x() << "," << c_global.y()
            << " " << c_global_rect.width() << "x" << c_global_rect.height() << ")";
        if (auto* current = currentTab(); current && current->view()) {
            auto* view = current->view();
            auto v_global = view->mapToGlobal(QPoint(0, 0));
            qInfo().nospace()
                << "  viewport_global=(" << v_global.x() << "," << v_global.y()
                << " " << view->width() << "x" << view->height() << ")";
        }

        // Check for overlap with tab bar.
        auto tb_global = m_tab_bar->mapToGlobal(QPoint(0, 0));
        QRect tb_global_rect(tb_global, m_tab_bar->size());
        if (container->isVisible() && c_global_rect.intersects(tb_global_rect)) {
            qWarning().nospace()
                << "SERVOQ_WARN container overlaps tab bar!"
                << " container=" << c_global_rect
                << " tab_bar=" << tb_global_rect;
        } else {
            qInfo().nospace()
                << "  tab_bar_global=(" << tb_global.x() << "," << tb_global.y()
                << " " << tb_global_rect.width() << "x" << tb_global_rect.height() << ")"
                << " overlap=" << (container->isVisible() && c_global_rect.intersects(tb_global_rect) ? "YES_BUG" : "no");
        }
    }
}

}
