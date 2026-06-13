/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Ladybird:
 *   UI/Qt/LocationEdit.cpp
 *   Libraries/LibWebView/HistoryStore.cpp
 *   UI/Qt/Autocomplete.h
 */
#include "LocationEdit.h"
#include "ChromeStyle.h"
#include "FaviconStore.h"
#include "HistoryStore.h"
#include "Icon.h"
#include "WebViewURL.h"

#include <QAction>
#include <QAbstractItemView>
#include <QCompleter>
#include <QItemSelectionModel>
#include <QEasingCurve>
#include <QEvent>
#include <QFocusEvent>
#include <QGraphicsDropShadowEffect>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QRectF>
#include <QResizeEvent>
#include <QStyle>
#include <QStandardItemModel>
#include <QTimer>
#include <QToolButton>
#include <QVariantAnimation>

namespace ServoQ {

namespace {

constexpr int LocationTrailingEdgeMargin  = 12;
constexpr int LocationTrailingTextGap     = 4;
constexpr int LocationTrailingItemGap     = 6;
constexpr int LocationTrailingActionWidth = 24;
constexpr int LocationTrailingActionHeight= 23;
constexpr int LocationPillHeight          = 22;
constexpr int LocationPillHorizontalPad   = 18;

// Custom trailing action button with rounded hover background.
class LocationActionButton final : public QToolButton {
public:
    explicit LocationActionButton(QWidget* parent) : QToolButton(parent) {}
private:
    void paintEvent(QPaintEvent*) override
    {
        static constexpr int hover_size = 23;
        static constexpr int icon_y_offset = -1;
        static constexpr qreal hover_y_offset = 1.0;

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        if (isDown() || underMouse()) {
            auto background = isDown()
                ? ChromeStyle::mix(ChromeStyle::chrome_surface_pressed(palette()), ChromeStyle::chrome_button_text(palette()), 0.04)
                : ChromeStyle::mix(ChromeStyle::chrome_surface_hover(palette()), ChromeStyle::chrome_button_text(palette()), 0.04);
            painter.setPen(Qt::NoPen);
            painter.setBrush(background);
            QRectF hover_rect(0, 0, hover_size, hover_size);
            hover_rect.moveCenter(QPointF(rect().center().x(), rect().center().y() + hover_y_offset));
            painter.drawRoundedRect(hover_rect, 10, 10);
        }

        QRect icon_rect(
            (width() - iconSize().width()) / 2,
            (height() - iconSize().height()) / 2 + icon_y_offset,
            iconSize().width(),
            iconSize().height());
        icon().paint(&painter, icon_rect, Qt::AlignCenter,
            isEnabled() ? QIcon::Normal : QIcon::Disabled,
            isDown() ? QIcon::On : QIcon::Off);
    }
};

} // namespace

LocationEdit::LocationEdit(QWidget* parent)
    : QLineEdit(parent)
    , m_history_completer(new QCompleter(this))
    , m_history_completion_model(new QStandardItemModel(this))
    , m_leading_icon(new QToolButton(this))
    , m_trailing_action(new LocationActionButton(this))
{
    setObjectName("LadybirdLocationEdit");
    setMinimumHeight(32);
    setPlaceholderText("Enter web address");
    setClearButtonEnabled(false);

    // Focus glow effect
    m_focus_glow_effect = new QGraphicsDropShadowEffect(this);
    m_focus_glow_effect->setBlurRadius(10);
    m_focus_glow_effect->setOffset(0, 0);
    updateFocusGlow(0);
    setGraphicsEffect(m_focus_glow_effect);

    m_focus_glow_animation = new QVariantAnimation(this);
    m_focus_glow_animation->setDuration(130);
    m_focus_glow_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_focus_glow_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
        updateFocusGlow(value.toInt());
    });

    m_leading_icon->setObjectName("LadybirdLocationIcon");
    m_leading_icon->setIconSize({ 18, 18 });
    m_leading_icon->setFixedSize(22, 22);
    m_leading_icon->setAutoRaise(true);
    m_leading_icon->setFocusPolicy(Qt::NoFocus);
    m_leading_icon->setCursor(Qt::ArrowCursor);
    m_leading_icon->hide();

    m_trailing_action->setObjectName("LadybirdLocationAction");
    m_trailing_action->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_trailing_action->setIconSize({ 17, 17 });
    m_trailing_action->setFixedSize(LocationTrailingActionWidth, LocationTrailingActionHeight);
    m_trailing_action->setAutoRaise(true);
    m_trailing_action->setFocusPolicy(Qt::NoFocus);
    m_trailing_action->setCursor(Qt::ArrowCursor);
    m_trailing_action->hide();

    m_history_completer->setModel(m_history_completion_model);
    m_history_completer->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
    m_history_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_history_completer->setWidget(this);
    if (auto* popup = m_history_completer->popup()) {
        popup->setObjectName("LadybirdAutocompletePopup");
    }
    connect(this, &QLineEdit::textEdited, this, &LocationEdit::updateHistorySuggestions);
    connect(m_history_completer, qOverload<QModelIndex const&>(&QCompleter::activated), this, [this](QModelIndex const& index) {
        activateHistorySuggestion(index);
    });

    // Zoom indicator pill
    m_zoom_indicator_button = new QToolButton(this);
    m_zoom_indicator_button->setObjectName("LadybirdLocationZoomIndicator");
    m_zoom_indicator_button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_zoom_indicator_button->setFixedHeight(LocationPillHeight);
    m_zoom_indicator_button->setAutoRaise(true);
    m_zoom_indicator_button->setFocusPolicy(Qt::NoFocus);
    m_zoom_indicator_button->setCursor(Qt::ArrowCursor);
    m_zoom_indicator_button->hide();
    connect(m_zoom_indicator_button, &QToolButton::clicked, this, [this] {
        if (m_zoom_action)
            m_zoom_action->trigger();
    });

    updateChromeStyle();
    setUrl(m_url);
}

void LocationEdit::setUrl(QString const& url)
{
    setUrl(std::optional<QString> { url.isEmpty() ? QStringLiteral("about:blank") : url });
}

void LocationEdit::setUrl(std::optional<QString> url)
{
    m_url = std::move(url);
    if (!hasFocus()) {
        setText(m_url.has_value() ? WebViewURL::url_for_display(*m_url) : text());
        setCursorPosition(0);
    }
    updateLocationIcon();
}

void LocationEdit::setTrailingAction(QAction* action)
{
    m_trailing_action->setDefaultAction(action);
    m_trailing_action->setVisible(action != nullptr);
    updateButtonPositions();
}

void LocationEdit::setZoomAction(QAction* action)
{
    if (m_zoom_action == action)
        return;

    if (m_zoom_action)
        QObject::disconnect(m_zoom_action, nullptr, this, nullptr);

    m_zoom_action = action;

    if (m_zoom_action)
        connect(m_zoom_action, &QAction::changed, this, &LocationEdit::updateZoomIndicator);

    updateZoomIndicator();
}

void LocationEdit::changeEvent(QEvent* event)
{
    QLineEdit::changeEvent(event);
    auto type = event->type();
    if (type == QEvent::PaletteChange || type == QEvent::ApplicationPaletteChange || type == QEvent::ThemeChange)
        updateChromeStyle();
}

void LocationEdit::focusInEvent(QFocusEvent* event)
{
    auto display_text = m_url.has_value() ? WebViewURL::url_for_display(*m_url) : QString();
    auto should_defer_full_url = event->reason() == Qt::MouseFocusReason
        && m_url.has_value()
        && text() == display_text;

    QLineEdit::focusInEvent(event);

    m_should_show_full_url_on_mouse_release = should_defer_full_url;

    if (!should_defer_full_url && m_url.has_value() && text() == display_text)
        setText(m_url.has_value() && *m_url != QStringLiteral("about:blank") ? *m_url : QString());

    updateLocationIcon(); // hide indicator while editing
    animateFocusGlow(58);

    if (event->reason() != Qt::PopupFocusReason && !should_defer_full_url) {
        QTimer::singleShot(0, this, [this] {
            if (hasFocus())
                selectAll();
        });
    }
}

void LocationEdit::focusOutEvent(QFocusEvent* event)
{
    QLineEdit::focusOutEvent(event);

    if (event->reason() == Qt::PopupFocusReason)
        return;

    animateFocusGlow(0);
    m_should_show_full_url_on_mouse_release = false;

    if (m_url.has_value() && text() == *m_url)
        setText(WebViewURL::url_for_display(*m_url));

    deselect();
    setCursorPosition(0);
    updateLocationIcon(); // restore indicator after editing
}

void LocationEdit::keyPressEvent(QKeyEvent* event)
{
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        && m_history_completer
        && m_history_completer->popup()
        && m_history_completer->popup()->isVisible()) {
        auto* popup = m_history_completer->popup();
        // Only open a history suggestion when the user has explicitly moved into
        // the popup (Up/Down). A freshly shown popup has no selection, so Enter
        // falls through to returnPressed and uses the typed text / default search
        // — the popup never silently hijacks the first result.
        bool user_selected = popup->selectionModel() && popup->selectionModel()->hasSelection();
        auto index = popup->currentIndex();
        if (user_selected && index.isValid() && activateHistorySuggestion(index)) {
            event->accept();
            return;
        }
        popup->hide();
    }

    if (event->key() == Qt::Key_Escape) {
        if (m_history_completer && m_history_completer->popup() && m_history_completer->popup()->isVisible()) {
            m_history_completer->popup()->hide();
            return;
        }
        if (m_url.has_value())
            setText(*m_url != QStringLiteral("about:blank") ? *m_url : QString());
        clearFocus();
        return;
    }
    QLineEdit::keyPressEvent(event);
}

bool LocationEdit::activateHistorySuggestion(QModelIndex const& index)
{
    auto url = index.data(Qt::UserRole).toString();
    if (url.isEmpty())
        return false;

    setText(url);
    if (m_history_completer && m_history_completer->popup())
        m_history_completer->popup()->hide();
    emit returnPressed();
    return true;
}

void LocationEdit::mouseReleaseEvent(QMouseEvent* event)
{
    QLineEdit::mouseReleaseEvent(event);
    if (event->button() == Qt::LeftButton && m_should_show_full_url_on_mouse_release) {
        m_should_show_full_url_on_mouse_release = false;
        setText(m_url.has_value() && *m_url != QStringLiteral("about:blank") ? *m_url : QString());
        selectAll();
    }
}

void LocationEdit::resizeEvent(QResizeEvent* event)
{
    QLineEdit::resizeEvent(event);
    updateButtonPositions();
}

void LocationEdit::updateFocusGlow(int alpha)
{
    m_focus_glow_alpha = alpha;
    if (m_focus_glow_effect) {
        auto color = ChromeStyle::chrome_accent(palette());
        color.setAlpha(alpha);
        m_focus_glow_effect->setColor(color);
    }
}

void LocationEdit::updateHistorySuggestions(QString const& query)
{
    m_history_completion_model->clear();

    auto suggestions = HistoryStore::the()->autocompleteSuggestions(query, 8);
    for (auto const& suggestion : suggestions) {
        auto display_url = WebViewURL::url_for_display(suggestion.url);
        auto label = suggestion.title.isEmpty()
            ? display_url
            : QStringLiteral("%1 — %2").arg(suggestion.title, display_url);
        auto* item = new QStandardItem(label);
        item->setData(suggestion.url, Qt::UserRole);
        item->setToolTip(suggestion.url);
        auto icon = FaviconStore::the()->iconForUrl(suggestion.url);
        item->setIcon(icon.isNull() ? create_chrome_icon(ChromeIcon::Globe, palette()) : icon);
        m_history_completion_model->appendRow(item);
    }

    if (suggestions.isEmpty() || !hasFocus()) {
        if (m_history_completer->popup())
            m_history_completer->popup()->hide();
        return;
    }

    m_history_completer->complete();
    // QCompleter preselects (and highlights) the first row when the popup opens,
    // which made Enter open that result instead of submitting the typed query.
    // Clear it so nothing is selected until the user presses Up/Down — matching
    // Ladybird's Autocomplete::clear_selection().
    if (auto* popup = m_history_completer->popup()) {
        popup->setCurrentIndex(QModelIndex());
        popup->clearSelection();
    }
}

void LocationEdit::animateFocusGlow(int target_alpha)
{
    if (!m_focus_glow_animation)
        return;
    m_focus_glow_animation->stop();
    m_focus_glow_animation->setStartValue(m_focus_glow_alpha);
    m_focus_glow_animation->setEndValue(target_alpha);
    m_focus_glow_animation->start();
}

void LocationEdit::updateChromeStyle()
{
    if (m_is_updating_chrome_style)
        return;
    m_is_updating_chrome_style = true;
    setStyleSheet(ChromeStyle::location_edit_style_sheet(palette()));
    m_is_updating_chrome_style = false;
    updateZoomIndicator();
}

int LocationEdit::trailingTextMargin() const
{
    auto margin = LocationTrailingEdgeMargin + LocationTrailingActionWidth + LocationTrailingTextGap;
    if (m_zoom_indicator_button && !m_zoom_indicator_button->isHidden())
        margin += m_zoom_indicator_button->width() + LocationTrailingItemGap;
    return margin;
}

void LocationEdit::updateButtonPositions()
{
    // Leading icon
    auto button_size = m_leading_icon->size();
    bool not_secure = m_leading_icon->property("notSecure").toBool();
    auto leading_y = (height() - button_size.height()) / 2 + (not_secure ? 0 : 1);
    m_leading_icon->move(12, leading_y);

    // Trailing action button
    auto trailing_x = width() - m_trailing_action->width() - LocationTrailingEdgeMargin;
    auto trailing_y = (height() - m_trailing_action->height()) / 2;
    m_trailing_action->move(trailing_x, trailing_y);
    m_trailing_action->raise();

    // Zoom indicator
    if (m_zoom_indicator_button) {
        auto zoom_button_size = m_zoom_indicator_button->size();
        auto zoom_y = (height() - zoom_button_size.height()) / 2;
        m_zoom_indicator_button->move(trailing_x - LocationTrailingItemGap - zoom_button_size.width(), zoom_y);
        m_zoom_indicator_button->raise();
    }

    setTextMargins(m_text_leading_margin, 0, trailingTextMargin(), 0);
}

void LocationEdit::updateLocationIcon()
{
    if (!m_leading_icon)
        return;

    auto update_indicator_style = [this](bool not_secure) {
        if (m_leading_icon->property("notSecure").toBool() == not_secure)
            return;
        m_leading_icon->setProperty("notSecure", not_secure);
        m_leading_icon->style()->unpolish(m_leading_icon);
        m_leading_icon->style()->polish(m_leading_icon);
    };

    auto hide_indicator = [&] {
        update_indicator_style(false);
        m_leading_icon->hide();
        m_leading_icon->setText({});
        m_leading_icon->setIcon({});
        m_leading_icon->setToolTip({});
        m_text_leading_margin = 0;
        setTextMargins(0, 0, trailingTextMargin(), 0);
    };

    auto show_not_secure_indicator = [&] {
        update_indicator_style(true);
        m_leading_icon->setToolButtonStyle(Qt::ToolButtonTextOnly);
        m_leading_icon->setIcon({});
        m_leading_icon->setText(QStringLiteral("Not secure"));
        auto icon_width = m_leading_icon->fontMetrics().horizontalAdvance(QStringLiteral("Not secure")) + 18;
        m_leading_icon->setFixedSize(icon_width, 22);
        m_leading_icon->setToolTip(QStringLiteral("Not secure"));
        m_leading_icon->show();
        auto button_size = m_leading_icon->size();
        m_leading_icon->move(12, (height() - button_size.height()) / 2);
        m_text_leading_margin = icon_width;
        setTextMargins(m_text_leading_margin, 0, trailingTextMargin(), 0);
    };

    // Determine if we are displaying the current URL (not focused / editing)
    bool is_showing_current_url = !hasFocus()
        && m_url.has_value()
        && text() == WebViewURL::url_for_display(*m_url);

    if (is_showing_current_url) {
        if (m_url->startsWith(QStringLiteral("http://")) && !m_url->startsWith(QStringLiteral("https://")))
            show_not_secure_indicator();
        else
            hide_indicator();
        return;
    }

    hide_indicator();
}

void LocationEdit::updateZoomIndicator()
{
    if (!m_zoom_indicator_button)
        return;

    auto visible = m_zoom_action && m_zoom_action->isVisible() && !m_zoom_action->text().isEmpty();
    if (!visible) {
        m_zoom_indicator_button->hide();
        setTextMargins(m_text_leading_margin, 0, trailingTextMargin(), 0);
        updateButtonPositions();
        return;
    }

    m_zoom_indicator_button->setText(m_zoom_action->text());
    m_zoom_indicator_button->setToolTip(m_zoom_action->toolTip());

    auto pill_width = m_zoom_indicator_button->fontMetrics().horizontalAdvance(m_zoom_indicator_button->text()) + LocationPillHorizontalPad;
    m_zoom_indicator_button->setFixedSize(pill_width, LocationPillHeight);
    m_zoom_indicator_button->show();

    setTextMargins(m_text_leading_margin, 0, trailingTextMargin(), 0);
    updateButtonPositions();
}

}
