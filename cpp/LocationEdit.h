#pragma once

#include <QLineEdit>
#include <optional>

class QAction;
class QEvent;
class QFocusEvent;
class QGraphicsDropShadowEffect;
class QKeyEvent;
class QMouseEvent;
class QResizeEvent;
class QToolButton;
class QVariantAnimation;

namespace ServoQ {

class LocationEdit final : public QLineEdit {
public:
    explicit LocationEdit(QWidget* parent = nullptr);

    void setUrl(QString const& url);
    void setUrl(std::optional<QString> url);
    std::optional<QString> url() const { return m_url; }
    void setTrailingAction(QAction* action);
    void setZoomAction(QAction* action); // [ladybird: LocationEdit.cpp:353]

protected:
    void changeEvent(QEvent* event) override;     // [ladybird: LocationEdit.cpp:381]
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override; // [ladybird: LocationEdit.cpp:461]
    void mouseReleaseEvent(QMouseEvent* event) override; // [ladybird: LocationEdit.cpp:501]
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateChromeStyle();
    void updateButtonPositions();
    void updateLocationIcon();                 // [ladybird: LocationEdit.cpp:594]
    void updateZoomIndicator();                // [ladybird: LocationEdit.cpp:679]
    int trailingTextMargin() const;            // [ladybird: LocationEdit.cpp:545]
    void animateFocusGlow(int target_alpha);   // [ladybird: LocationEdit.cpp:450]
    void updateFocusGlow(int alpha);           // [ladybird: LocationEdit.cpp:443]

    QToolButton* m_leading_icon { nullptr };
    QToolButton* m_trailing_action { nullptr };
    QToolButton* m_zoom_indicator_button { nullptr }; // [ladybird: LocationEdit.h:84]
    QVariantAnimation* m_focus_glow_animation { nullptr }; // [ladybird: LocationEdit.h:85]
    QGraphicsDropShadowEffect* m_focus_glow_effect { nullptr }; // [ladybird: LocationEdit.h:86]
    QAction* m_zoom_action { nullptr };        // [ladybird: LocationEdit.h:87]
    std::optional<QString> m_url { QStringLiteral("about:blank") };
    bool m_is_updating_chrome_style { false };
    bool m_should_show_full_url_on_mouse_release { false }; // [ladybird: LocationEdit.h:94]
    int m_text_leading_margin { 0 };           // [ladybird: LocationEdit.h:95]
    int m_focus_glow_alpha { 0 };              // [ladybird: LocationEdit.h:96]
};

}
