#include "BookmarksBar.h"
#include "ChromeStyle.h"
#include "Icon.h"

#include <QAction>
#include <QEvent>
#include <QToolButton>

namespace ServoQ {

BookmarksBar::BookmarksBar(QWidget* parent)
    : QToolBar(parent)
{
    setObjectName("LadybirdBookmarksBar");
    setMovable(false);
    setFloatable(false);
    setIconSize({ 16, 16 });
    updateChromeStyle();
    rebuild();
}

void BookmarksBar::rebuild()
{
    clear();
    auto add_bookmark = [this](QString const& text, QString const& url) {
        auto* action = addAction(create_chrome_icon(ChromeIcon::Globe, palette()), text);
        action->setToolTip(url);
    };
    add_bookmark("Ladybird", "https://ladybird.org/");
    add_bookmark("Servo", "https://servo.org/");
    add_bookmark("Qt", "https://www.qt.io/");

    for (auto* action : actions()) {
        if (auto* button = qobject_cast<QToolButton*>(widgetForAction(action))) {
            button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            button->setIconSize({ 16, 16 });
            button->setAutoRaise(true);
            button->setMaximumWidth(150);
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

}
