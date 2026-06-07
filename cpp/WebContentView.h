// WebContentView.h
//
// Widget that hosts one Servo WebView per tab. Shape mirrors Ladybird's
// WebContentView (vendor/reference-ladybird/UI/Qt/WebContentView.h:47-78):
// identical event-surface, set_zoom_level, show/hideEvent, focus events.
//
// Engine calls live behind the bridge; the widget is feature-agnostic:
//   - no frame received yet  → shows the placeholder (WebContentPlaceholder)
//   - frame received         → blits the RGBA QImage in paintEvent

#pragma once

#include <QImage>
#include <QWidget>

class QKeyEvent;
class QMouseEvent;
class QTimer;
class QWheelEvent;

namespace ServoQ {

class Tab;
class WebContentPlaceholder;

class WebContentView final : public QWidget {
public:
    explicit WebContentView(QWidget* parent = nullptr);
    ~WebContentView() override;

    // Called by Tab after construction so the widget knows which tab owns it.
    void setTab(Tab* tab);
    void setTabId(int tab_id);
    int tabId() const { return m_tab_id; }
    Tab* tab() const { return m_tab; }

    // Placeholder display (shown until first engine frame arrives).
    void setUrl(QString const& url);
    void setStatus(QString const& status);

    // Queue the URL to navigate when the engine WebView is first created.
    void setInitialUrl(QString const& url);

    // Called from C++ callback (servoq::deliver_frame) to push a frame.
    void receiveFrame(QImage const& frame);

    // Called when Servo panics. Shows an inline crash page. [ladybird: WebContentView crash signal]
    void receiveWebViewCrash(QString const& reason);

    // Matches Ladybird WebContentView::set_zoom_level (WebContentView.h:81).
    void set_zoom_level(double zoom_level);

    // Ladybird WebContentView.h:56-78 event surface
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void leaveEvent(QEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    // inputMethodEvent / inputMethodQuery: IME — out of scope this pass
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    void focusInEvent(QFocusEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
    bool event(QEvent*) override;

private:
    void forwardMouseButton(int action, int button, QMouseEvent* ev);
    void startEngineIfNeeded();
    void forwardResizeToEngine();

    Tab* m_tab { nullptr };
    int m_tab_id { 0 };
    QString m_initial_url { QStringLiteral("about:blank") };
    bool m_webview_created { false };

    WebContentPlaceholder* m_placeholder { nullptr };
    bool m_placeholder_visible { true };

    QImage m_frame {};
    QTimer* m_engine_tick_timer { nullptr };
};

} // namespace ServoQ
