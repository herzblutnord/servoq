#include "ui/WebContentPlaceholder.h"
#include "ui/ChromeStyle.h"

#include <QEvent>
#include <QFrame>
#include <QLabel>
#include <QVBoxLayout>

namespace ServoQ {

WebContentPlaceholder::WebContentPlaceholder(QWidget* parent)
    : QWidget(parent)
    , m_title(new QLabel("Servo WebView placeholder", this))
    , m_url(new QLabel("about:blank", this))
    , m_status(new QLabel("The Servo rendering surface will be inserted here.", this))
{
    setObjectName("ServoQWebContentPlaceholder");
    setAttribute(Qt::WA_StyledBackground);
    setMinimumSize(480, 320);

    auto* outer_layout = new QVBoxLayout(this);
    outer_layout->setContentsMargins(32, 32, 32, 32);
    outer_layout->addStretch(1);

    auto* card = new QFrame(this);
    card->setObjectName("ServoQPlaceholderCard");
    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(28, 24, 28, 24);
    card_layout->setSpacing(10);

    m_title->setObjectName("ServoQPlaceholderTitle");
    m_url->setObjectName("ServoQPlaceholderSubtitle");
    m_status->setObjectName("ServoQPlaceholderSubtitle");
    m_url->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->setWordWrap(true);

    card_layout->addWidget(m_title);
    card_layout->addWidget(m_url);
    card_layout->addSpacing(8);
    card_layout->addWidget(m_status);

    outer_layout->addWidget(card, 0, Qt::AlignCenter);
    outer_layout->addStretch(1);

    updateChromeStyle();
}

void WebContentPlaceholder::setUrl(QString const& url)
{
    m_url->setText(url.isEmpty() ? QStringLiteral("about:blank") : url);
}

void WebContentPlaceholder::setStatus(QString const& status)
{
    m_status->setText(status);
}

bool WebContentPlaceholder::event(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange)
        updateChromeStyle();
    return QWidget::event(event);
}

void WebContentPlaceholder::updateChromeStyle()
{
    if (m_is_updating_chrome_style)
        return;
    m_is_updating_chrome_style = true;
    setStyleSheet(ChromeStyle::web_placeholder_style_sheet(palette()));
    m_is_updating_chrome_style = false;
}

}
