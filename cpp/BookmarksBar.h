#pragma once

#include <QToolBar>

namespace ServoQ {

class BookmarksBar final : public QToolBar {
public:
    explicit BookmarksBar(QWidget* parent = nullptr);
    void rebuild();

protected:
    bool event(QEvent* event) override;

private:
    void updateChromeStyle();
    bool m_is_updating_chrome_style { false };
};

}
