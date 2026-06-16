#pragma once

#include <QWidget>

class QLineEdit;
class QListWidget;
class QListWidgetItem;

namespace ServoQ {

class BrowserWindow;
class Tab;
class TabWidget;

// Chrome-style tab search (Ctrl+Shift+A): a filtered popup of open tabs. A
// Qt::Popup window so it renders above the native Servo subsurface.
class TabSearchPopup final : public QWidget {
public:
    TabSearchPopup(BrowserWindow* window, TabWidget* tabs);

    // Rebuilds the tab list and shows the popup anchored below the toolbar at
    // the right edge of the browser window.
    void open();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void rebuildList();
    void applyFilter(QString const& query);
    void activateItem(QListWidgetItem* item);
    void closeTabForItem(QListWidgetItem* item);
    void moveSelection(int delta);
    Tab* tabForItem(QListWidgetItem* item) const;

    BrowserWindow* m_window { nullptr };
    TabWidget* m_tabs { nullptr };
    QLineEdit* m_search_edit { nullptr };
    QListWidget* m_list { nullptr };
};

}
