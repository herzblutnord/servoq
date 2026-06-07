#include "LocationEdit.h"
#include "ChromeStyle.h"
#include "Icon.h"
#include "WebViewURL.h"

#include <QAction>
#include <QEvent>
#include <QFocusEvent>
#include <QResizeEvent>

namespace ServoQ {

LocationEdit::LocationEdit(QWidget* parent)
    : QLineEdit(parent)
    , m_leading_icon(new QToolButton(this))
    , m_trailing_action(new QToolButton(this))
{
    setObjectName("LadybirdLocationEdit");
    setMinimumHeight(32);
    setPlaceholderText("Enter web address");
    setClearButtonEnabled(false);

    m_leading_icon->setObjectName("LadybirdLocationIcon");
    m_leading_icon->setToolTip("Site information placeholder");
    m_leading_icon->setIcon(create_chrome_icon(ChromeIcon::Globe, palette()));
    m_leading_icon->setIconSize({ 16, 16 });
    m_leading_icon->setFixedSize(22, 22);
    m_leading_icon->setFocusPolicy(Qt::NoFocus);
    m_leading_icon->setAutoRaise(true);

    m_trailing_action->setObjectName("LadybirdLocationAction");
    m_trailing_action->setFixedSize(24, 23);
    m_trailing_action->setFocusPolicy(Qt::NoFocus);
    m_trailing_action->setAutoRaise(true);
    m_trailing_action->hide();

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
    if (!hasFocus())
        setText(m_url.has_value() ? WebViewURL::url_for_display(*m_url) : text());
}

void LocationEdit::setTrailingAction(QAction* action)
{
    m_trailing_action->setDefaultAction(action);
    m_trailing_action->setVisible(action != nullptr);
    updateButtonPositions();
}

void LocationEdit::focusInEvent(QFocusEvent* event)
{
    QLineEdit::focusInEvent(event);
    setText(m_url.has_value() && *m_url != QStringLiteral("about:blank") ? *m_url : QString());
    selectAll();
}

void LocationEdit::resizeEvent(QResizeEvent* event)
{
    QLineEdit::resizeEvent(event);
    updateButtonPositions();
}

bool LocationEdit::event(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange)
        updateChromeStyle();
    return QLineEdit::event(event);
}

void LocationEdit::updateChromeStyle()
{
    if (m_is_updating_chrome_style)
        return;
    m_is_updating_chrome_style = true;
    setStyleSheet(ChromeStyle::location_edit_style_sheet(palette()));
    m_is_updating_chrome_style = false;
}

void LocationEdit::updateButtonPositions()
{
    auto y = (height() - m_leading_icon->height()) / 2;
    m_leading_icon->move(10, y);
    m_trailing_action->move(width() - m_trailing_action->width() - 9, (height() - m_trailing_action->height()) / 2);
}

}
