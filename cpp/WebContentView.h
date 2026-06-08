// WebContentView.h
//
// Widget that hosts one Servo WebView per tab. Shape mirrors Ladybird's
// WebContentView (vendor/reference-ladybird/UI/Qt/WebContentView.h:47-78):
// identical event-surface, set_zoom_level, show/hideEvent, focus events.
//
// Engine calls live behind the bridge:
//   - before first frame  → black background; no placeholder widget
//   - frame received      → blits the RGBA QImage in paintEvent
//   - engine crashed      → paints inline crash message in paintEvent

#pragma once

#include <QImage>
#include <QSize>
#include <QString>
#include <QWidget>

class QKeyEvent;
class QMouseEvent;
class QTimer;
class QWheelEvent;
class QWindow;

namespace ServoQ {

class Tab;
class ServoWaylandContentWindow;

class WebContentView final : public QWidget {
    Q_OBJECT
    friend class ServoWaylandContentWindow;
public:
    explicit WebContentView(QWidget* parent = nullptr);
    ~WebContentView() override;

    // Called by Tab after construction so the widget knows which tab owns it.
    void setTab(Tab* tab);
    void setTabId(int tab_id);
    int tabId() const { return m_tab_id; }
    Tab* tab() const { return m_tab; }

    // No-ops kept for Tab.cpp call-site compatibility; no placeholder is shown.
    void setUrl(QString const& url);
    void setStatus(QString const& status);

    // Queue the URL to navigate when the engine WebView is first created.
    void setInitialUrl(QString const& url);

    // Called from C++ callback (servoq::deliver_frame) to push a frame.
    void receiveFrame(QImage const& frame);
    void receiveFrameBytes(uint8_t const* bytes, int width, int height);
    bool hasPendingFrameRepaint() const { return m_pending_frame_repaint; }
    void requestWaylandRepaint();
    bool takeWaylandPresentPending();

    // Called when Servo panics. Renders inline crash message. [ladybird: WebContentView crash signal]
    void receiveWebViewCrash(QString const& reason);
    void receiveRequestBlocked(QString const& url);
    void notifyThemeChange();

    // Matches Ladybird WebContentView::set_zoom_level (WebContentView.h:81).
    void set_zoom_level(double zoom_level);

signals:
    void request_blocked(QString const& url_string);

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
    void forwardWindowMouseButton(int action, int button, QMouseEvent* ev);
    void startEngineIfNeeded();
    void forwardResizeToEngine();
    bool startWaylandRendererIfPossible(int physical_width, int physical_height, qreal dpr, bool allow_software_gl);
    bool attachSharedWaylandWindow();
    bool waylandRendererRequested() const;
    bool waylandRendererActive() const { return m_wayland_renderer_active; }
    ServoWaylandContentWindow* waylandWindow() const { return m_wayland_window; }

    Tab* m_tab { nullptr };
    int m_tab_id { 0 };
    QString m_initial_url { QStringLiteral("about:blank") };
    bool m_webview_created { false };

    bool m_crashed { false };
    bool m_pending_frame_repaint { false };
    QString m_crash_reason;

    QImage m_frame {};
    QSize m_last_forwarded_physical_size {};
    qreal m_last_forwarded_dpr { 0.0 };
    QTimer* m_engine_tick_timer { nullptr };
    ServoWaylandContentWindow* m_wayland_window { nullptr };
    QWidget* m_wayland_container { nullptr };
    bool m_wayland_renderer_active { false };
    bool m_wayland_present_pending { false };
    bool m_wayland_present_in_progress { false };
    bool m_wayland_dirty_after_present { false };
};

} // namespace ServoQ
