#include "LocationEdit.h"
#include "ChromeStyle.h"
#include "HistoryStore.h"
#include "Icon.h"
#include "WebViewURL.h"

#include <QAction>
#include <QAbstractItemView>
#include <QCompleter>
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

constexpr int LocationTrailingEdgeMargin  = 12; // [ladybird: LocationEdit.cpp:190]
constexpr int LocationTrailingTextGap     = 4;  // [ladybird: LocationEdit.cpp:191]
constexpr int LocationTrailingItemGap     = 6;  // [ladybird: LocationEdit.cpp:192]
constexpr int LocationTrailingActionWidth = 24; // [ladybird: LocationEdit.cpp:193]
constexpr int LocationTrailingActionHeight= 23; // [ladybird: LocationEdit.cpp:194]
constexpr int LocationPillHeight          = 22; // [ladybird: LocationEdit.cpp:195]
constexpr int LocationPillHorizontalPad   = 18; // [ladybird: LocationEdit.cpp:196]

// Custom trailing action button with rounded hover background.
// [ladybird: LocationEdit.cpp:42-77]
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
    , m_trailing_action(new LocationActionButton(this)) // [ladybird: LocationEdit.cpp:228]
{
    setObjectName("LadybirdLocationEdit");
    setMinimumHeight(32);
    setPlaceholderText("Enter web address");
    setClearButtonEnabled(false);

    // Focus glow effect [ladybird: LocationEdit.cpp:206-217]
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

    m_leading_icon->setObjectName("LadybirdLocationIcon"); // [ladybird: LocationEdit.cpp:220]
    m_leading_icon->setIconSize({ 18, 18 });               // [ladybird: LocationEdit.cpp:221]
    m_leading_icon->setFixedSize(22, 22);                  // [ladybird: LocationEdit.cpp:222]
    m_leading_icon->setAutoRaise(true);                    // [ladybird: LocationEdit.cpp:223]
    m_leading_icon->setFocusPolicy(Qt::NoFocus);           // [ladybird: LocationEdit.cpp:224]
    m_leading_icon->setCursor(Qt::ArrowCursor);            // [ladybird: LocationEdit.cpp:225]
    m_leading_icon->hide();                                // [ladybird: LocationEdit.cpp:226]

    m_trailing_action->setObjectName("LadybirdLocationAction"); // [ladybird: LocationEdit.cpp:229]
    m_trailing_action->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_trailing_action->setIconSize({ 17, 17 }); // [ladybird: LocationEdit.cpp:231]
    m_trailing_action->setFixedSize(LocationTrailingActionWidth, LocationTrailingActionHeight);
    m_trailing_action->setAutoRaise(true);      // [ladybird: LocationEdit.cpp:233]
    m_trailing_action->setFocusPolicy(Qt::NoFocus); // [ladybird: LocationEdit.cpp:234]
    m_trailing_action->setCursor(Qt::ArrowCursor);  // [ladybird: LocationEdit.cpp:235]
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
        auto url = index.data(Qt::UserRole).toString();
        if (url.isEmpty())
            return;
        setText(url);
        m_history_completer->popup()->hide();
        emit returnPressed();
    });

    // Zoom indicator pill — [ladybird: LocationEdit.cpp:238-249]
    m_zoom_indicator_button = new QToolButton(this);
    m_zoom_indicator_button->setObjectName("LadybirdLocationZoomIndicator");    // [ladybird: LocationEdit.cpp:239]
    m_zoom_indicator_button->setToolButtonStyle(Qt::ToolButtonTextOnly);        // [ladybird: LocationEdit.cpp:240]
    m_zoom_indicator_button->setFixedHeight(LocationPillHeight);                // [ladybird: LocationEdit.cpp:241]
    m_zoom_indicator_button->setAutoRaise(true);                                // [ladybird: LocationEdit.cpp:242]
    m_zoom_indicator_button->setFocusPolicy(Qt::NoFocus);                       // [ladybird: LocationEdit.cpp:243]
    m_zoom_indicator_button->setCursor(Qt::ArrowCursor);                        // [ladybird: LocationEdit.cpp:244]
    m_zoom_indicator_button->hide();                                            // [ladybird: LocationEdit.cpp:245]
    connect(m_zoom_indicator_button, &QToolButton::clicked, this, [this] {      // [ladybird: LocationEdit.cpp:246-249]
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
        setCursorPosition(0); // [ladybird: LocationEdit.cpp:773] show URL from start when unfocused
    }
    updateLocationIcon(); // [ladybird: LocationEdit.cpp:776]
}

void LocationEdit::setTrailingAction(QAction* action)
{
    m_trailing_action->setDefaultAction(action);
    m_trailing_action->setVisible(action != nullptr);
    updateButtonPositions();
}

// [ladybird: LocationEdit.cpp:353-367]
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

void LocationEdit::changeEvent(QEvent* event) // [ladybird: LocationEdit.cpp:381-387]
{
    QLineEdit::changeEvent(event);
    auto type = event->type();
    if (type == QEvent::PaletteChange || type == QEvent::ApplicationPaletteChange || type == QEvent::ThemeChange)
        updateChromeStyle();
}

void LocationEdit::focusInEvent(QFocusEvent* event) // [ladybird: LocationEdit.cpp:389-411]
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

void LocationEdit::focusOutEvent(QFocusEvent* event) // [ladybird: LocationEdit.cpp:413-441]
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
    updateLocationIcon(); // restore indicator after editing [ladybird: LocationEdit.cpp:776]
}

void LocationEdit::keyPressEvent(QKeyEvent* event) // [ladybird: LocationEdit.cpp:461-499]
{
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

void LocationEdit::mouseReleaseEvent(QMouseEvent* event) // [ladybird: LocationEdit.cpp:501-507]
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

void LocationEdit::updateFocusGlow(int alpha) // [ladybird: LocationEdit.cpp:443-448]
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
        m_history_completion_model->appendRow(item);
    }

    if (suggestions.isEmpty() || !hasFocus()) {
        if (m_history_completer->popup())
            m_history_completer->popup()->hide();
        return;
    }

    m_history_completer->complete();
}

void LocationEdit::animateFocusGlow(int target_alpha) // [ladybird: LocationEdit.cpp:450-458]
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
    updateZoomIndicator(); // [ladybird: LocationEdit.cpp:563]
}

// [ladybird: LocationEdit.cpp:545-553] — trailing margin accounts for zoom indicator
int LocationEdit::trailingTextMargin() const
{
    auto margin = LocationTrailingEdgeMargin + LocationTrailingActionWidth + LocationTrailingTextGap;
    if (m_zoom_indicator_button && !m_zoom_indicator_button->isHidden())
        margin += m_zoom_indicator_button->width() + LocationTrailingItemGap;
    return margin;
}

// [ladybird: LocationEdit.cpp:518-531] — position leading icon, trailing action, zoom indicator
void LocationEdit::updateButtonPositions()
{
    // Leading icon — [ladybird: LocationEdit.cpp:518-520]
    auto button_size = m_leading_icon->size();
    bool not_secure = m_leading_icon->property("notSecure").toBool();
    auto leading_y = (height() - button_size.height()) / 2 + (not_secure ? 0 : 1);
    m_leading_icon->move(12, leading_y);    // [ladybird: LocationEdit.cpp:520]

    // Trailing action button — [ladybird: LocationEdit.cpp:522-525]
    auto trailing_x = width() - m_trailing_action->width() - LocationTrailingEdgeMargin;
    auto trailing_y = (height() - m_trailing_action->height()) / 2;
    m_trailing_action->move(trailing_x, trailing_y);
    m_trailing_action->raise();

    // Zoom indicator — [ladybird: LocationEdit.cpp:527-530]
    if (m_zoom_indicator_button) {
        auto zoom_button_size = m_zoom_indicator_button->size();
        auto zoom_y = (height() - zoom_button_size.height()) / 2;
        m_zoom_indicator_button->move(trailing_x - LocationTrailingItemGap - zoom_button_size.width(), zoom_y);
        m_zoom_indicator_button->raise();
    }

    setTextMargins(m_text_leading_margin, 0, trailingTextMargin(), 0);
}

// [ladybird: LocationEdit.cpp:594-677] — "Not secure" pill for http, icon for https (hidden)
void LocationEdit::updateLocationIcon()
{
    if (!m_leading_icon)
        return;

    auto update_indicator_style = [this](bool not_secure) { // [ladybird: LocationEdit.cpp:599-606]
        if (m_leading_icon->property("notSecure").toBool() == not_secure)
            return;
        m_leading_icon->setProperty("notSecure", not_secure);
        m_leading_icon->style()->unpolish(m_leading_icon);
        m_leading_icon->style()->polish(m_leading_icon);
    };

    auto hide_indicator = [&] { // [ladybird: LocationEdit.cpp:614-622]
        update_indicator_style(false);
        m_leading_icon->hide();
        m_leading_icon->setText({});
        m_leading_icon->setIcon({});
        m_leading_icon->setToolTip({});
        m_text_leading_margin = 0;
        setTextMargins(0, 0, trailingTextMargin(), 0);
    };

    auto show_not_secure_indicator = [&] { // [ladybird: LocationEdit.cpp:638-651]
        update_indicator_style(true);
        m_leading_icon->setToolButtonStyle(Qt::ToolButtonTextOnly);
        m_leading_icon->setIcon({});
        m_leading_icon->setText(QStringLiteral("Not secure")); // [ladybird: LocationEdit.cpp:642]
        auto icon_width = m_leading_icon->fontMetrics().horizontalAdvance(QStringLiteral("Not secure")) + 18; // [ladybird: LocationEdit.cpp:644]
        m_leading_icon->setFixedSize(icon_width, 22);          // [ladybird: LocationEdit.cpp:645]
        m_leading_icon->setToolTip(QStringLiteral("Not secure"));
        m_leading_icon->show();
        auto button_size = m_leading_icon->size();
        m_leading_icon->move(12, (height() - button_size.height()) / 2); // [ladybird: y_offset=0 for notSecure]
        m_text_leading_margin = icon_width;                    // [ladybird: LocationEdit.cpp:649]
        setTextMargins(m_text_leading_margin, 0, trailingTextMargin(), 0);
    };

    // Determine if we are displaying the current URL (not focused / editing) [ladybird: LocationEdit.cpp:653-655]
    bool is_showing_current_url = !hasFocus()
        && m_url.has_value()
        && text() == WebViewURL::url_for_display(*m_url);

    if (is_showing_current_url) {
        // [ladybird: LocationEdit.cpp:658-663]
        if (m_url->startsWith(QStringLiteral("http://")) && !m_url->startsWith(QStringLiteral("https://")))
            show_not_secure_indicator();
        else
            hide_indicator();
        return;
    }

    hide_indicator(); // [ladybird: LocationEdit.cpp:668-676] — no search engine / autocomplete in ServoQ
}

// [ladybird: LocationEdit.cpp:679-701]
void LocationEdit::updateZoomIndicator()
{
    if (!m_zoom_indicator_button)
        return;

    auto visible = m_zoom_action && m_zoom_action->isVisible() && !m_zoom_action->text().isEmpty(); // [ladybird: LocationEdit.cpp:684]
    if (!visible) {
        m_zoom_indicator_button->hide();
        setTextMargins(m_text_leading_margin, 0, trailingTextMargin(), 0);
        updateButtonPositions();
        return;
    }

    m_zoom_indicator_button->setText(m_zoom_action->text());          // [ladybird: LocationEdit.cpp:692]
    m_zoom_indicator_button->setToolTip(m_zoom_action->toolTip());    // [ladybird: LocationEdit.cpp:693]

    auto pill_width = m_zoom_indicator_button->fontMetrics().horizontalAdvance(m_zoom_indicator_button->text()) + LocationPillHorizontalPad; // [ladybird: LocationEdit.cpp:695]
    m_zoom_indicator_button->setFixedSize(pill_width, LocationPillHeight); // [ladybird: LocationEdit.cpp:696]
    m_zoom_indicator_button->show();                                    // [ladybird: LocationEdit.cpp:697]

    setTextMargins(m_text_leading_margin, 0, trailingTextMargin(), 0); // [ladybird: LocationEdit.cpp:699]
    updateButtonPositions();                                            // [ladybird: LocationEdit.cpp:700]
}

}
