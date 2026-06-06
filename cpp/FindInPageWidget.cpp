#include "FindInPageWidget.h"
#include "ChromeStyle.h"
#include "Icon.h"

#include <QCheckBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShowEvent>

namespace ServoQ {

FindInPageWidget::FindInPageWidget(QWidget* parent)
    : QWidget(parent)
    , m_find_text(new QLineEdit(this))
    , m_previous_button(new QPushButton("↑", this))
    , m_next_button(new QPushButton("↓", this))
    , m_exit_button(new QPushButton("×", this))
    , m_match_case(new QCheckBox("Match &Case", this))
    , m_result_label(new QLabel(this))
{
    setObjectName("LadybirdFindInPageBar");
    setAttribute(Qt::WA_StyledBackground);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(5, 5, 5, 5);
    layout->setSpacing(6);

    m_find_text->setPlaceholderText("Search");
    m_find_text->setMinimumWidth(50);
    m_find_text->setMaximumWidth(250);
    m_previous_button->setToolTip("Find Previous Match");
    m_next_button->setToolTip("Find Next Match");
    m_exit_button->setToolTip("Close Search Bar");
    m_previous_button->setIcon(create_chrome_icon(ChromeIcon::ChevronUp, palette()));
    m_next_button->setIcon(create_chrome_icon(ChromeIcon::ChevronDown, palette()));
    m_exit_button->setIcon(create_chrome_icon(ChromeIcon::Close, palette()));
    for (auto* button : { m_previous_button, m_next_button, m_exit_button }) {
        button->setText({});
        button->setIconSize({ 16, 16 });
        button->setFixedWidth(30);
        button->setFlat(true);
    }

    connect(m_find_text, &QLineEdit::textChanged, this, [this] { updateResultLabel(); });
    connect(m_previous_button, &QPushButton::clicked, this, [this] { updateResultLabel(); });
    connect(m_next_button, &QPushButton::clicked, this, [this] { updateResultLabel(); });
    connect(m_exit_button, &QPushButton::clicked, this, [this] { hide(); });

    layout->addWidget(m_find_text, 1);
    layout->addWidget(m_previous_button);
    layout->addWidget(m_next_button);
    layout->addWidget(m_match_case);
    layout->addWidget(m_result_label);
    layout->addStretch(1);
    layout->addWidget(m_exit_button);

    updateChromeStyle();
    updateResultLabel();
}

QString FindInPageWidget::query() const
{
    return m_find_text->text();
}

bool FindInPageWidget::event(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange)
        updateChromeStyle();
    return QWidget::event(event);
}

void FindInPageWidget::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        hide();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (event->modifiers().testFlag(Qt::ShiftModifier))
            m_previous_button->click();
        else
            m_next_button->click();
        return;
    }
    event->ignore();
}

void FindInPageWidget::focusInEvent(QFocusEvent* event)
{
    QWidget::focusInEvent(event);
    m_find_text->setFocus();
    m_find_text->selectAll();
}

void FindInPageWidget::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (onShown)
        onShown();
    m_find_text->setFocus();
}

void FindInPageWidget::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    if (onHidden)
        onHidden();
}

void FindInPageWidget::updateChromeStyle()
{
    if (m_is_updating_chrome_style)
        return;
    m_is_updating_chrome_style = true;
    setStyleSheet(ChromeStyle::find_in_page_style_sheet(palette()));
    m_is_updating_chrome_style = false;
}

void FindInPageWidget::updateResultLabel()
{
    if (m_find_text->text().isEmpty()) {
        m_result_label->clear();
        m_result_label->hide();
        return;
    }
    m_result_label->setText("Placeholder matches");
    m_result_label->show();
}

}
