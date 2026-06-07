#pragma once

#include <QLineEdit>
#include <QToolButton>
#include <optional>

namespace ServoQ {

class LocationEdit final : public QLineEdit {
public:
    explicit LocationEdit(QWidget* parent = nullptr);

    void setUrl(QString const& url);
    void setUrl(std::optional<QString> url);
    std::optional<QString> url() const { return m_url; }
    void setTrailingAction(QAction* action);

protected:
    void focusInEvent(QFocusEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override;

private:
    void updateChromeStyle();
    void updateButtonPositions();

    QToolButton* m_leading_icon { nullptr };
    QToolButton* m_trailing_action { nullptr };
    std::optional<QString> m_url { QStringLiteral("about:blank") };
    bool m_is_updating_chrome_style { false };
};

}
