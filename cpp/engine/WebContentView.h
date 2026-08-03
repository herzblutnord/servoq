/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Ladybird:
 *   UI/Qt/WebContentView.h
 */
// WebContentView.h
//
// Widget that hosts one Servo WebView per tab.

#pragma once

#include <QImage>
#include <QHash>
#include <QList>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QWidget>

class QKeyEvent;
class QMouseEvent;
class QTabletEvent;
class QTimer;
class QTouchEvent;
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

    // No-ops kept for Tab.cpp call-site compatibility; empty tabs paint in paintEvent.
    void setUrl(QString const& url);
    void setStatus(QString const& status);

    // Queue the URL to navigate when the engine WebView is first created.
    void setInitialUrl(QString const& url);
    void setEmptyNewTab(bool empty_new_tab);
    bool ensureEngineStarted();

    // Internal-page mode (servoq:// pages): while active, the tab shows a native
    // Qt page and the shared Servo surface stays released/unmapped.
    void setInternalPageActive(bool active);
    bool internalPageActive() const { return m_internal_page_active; }

    // Activation transaction — called by TabWidget::activateTab (always deferred via
    // QTimer::singleShot so the mouse event that triggered the switch has unwound).
    // These are the ONLY paths that change g_wayland_owner or touch the container.
    void onBecomeActiveTab();
    void onBecomeInactiveTab();
    bool isCurrentWaylandOwner() const;

    // Public so TabWidget::updateContainerGeometry can delegate to it.
    void updateContainerGeometry();

    // tab_id -> view lookup for async completions (Favicon.cpp probe callbacks,
    // the servo_callbacks.cpp FFI layer).
    static WebContentView* findByTabId(int tab_id);
    // All live views, for shutdown-time iteration (servoq::begin_servo_shutdown).
    static QList<WebContentView*> allViews();

    // State accessors used by TabWidget::dumpPresentationState.
    static WebContentView* currentWaylandOwner();
    static QWidget* sharedWaylandContainer();
    static QWindow* sharedWaylandWindow();
    bool webviewCreated() const { return m_webview_created; }
    bool waylandRendererActivePublic() const { return m_wayland_renderer_active; }
    bool waylandPresentPendingPublic() const { return m_wayland_present_pending; }
    bool isEmptyNewTab() const { return m_empty_new_tab; }

    // Called from C++ callback (servoq::deliver_frame) to push a frame.
    void receiveFrameBytes(uint8_t const* bytes, int width, int height);
    bool hasPendingFrameRepaint() const { return m_pending_frame_repaint; }
    // Why a present was requested — tracked per-second under SERVOQ_PERF so an
    // over-scheduling source is identifiable without per-present logging.
    enum class PresentRequestReason {
        FrameReady,  // Servo notify_new_frame_ready (a genuinely new frame)
        Expose,      // QWindow expose — re-present the existing frame
        Resize,      // viewport resize
        Activation,  // tab became active / container re-attached
        Retry,       // deferred retry of an earlier capped/deferred request
        Shutdown,
    };
    void requestWaylandRepaint(PresentRequestReason reason = PresentRequestReason::FrameReady);
    bool takeWaylandPresentPending();

    // Called when Servo panics. Renders inline crash message.
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
    bool forwardTouchEvent(QTouchEvent* event, qreal dpr);
    bool forwardTabletEvent(QTabletEvent* event, qreal dpr);
    void takeFocusFromContentClick();
    bool handleCtrlWheelZoom(QWheelEvent* event);
    bool handleMiddleClickLinkFallback(QMouseEvent* event);
    bool startEngineIfNeeded();
    void forwardResizeToEngine();
    bool startWaylandRendererIfPossible(int physical_width, int physical_height, qreal dpr, bool allow_software_gl);
    bool attachSharedWaylandWindow();
    bool waylandRendererRequested() const;
    bool waylandRendererActive() const { return m_wayland_renderer_active; }
    ServoWaylandContentWindow* waylandWindow() const { return m_wayland_window; }
    bool isCurrentlyActiveTab() const;

    Tab* m_tab { nullptr };
    int m_tab_id { 0 };
    QString m_initial_url { QStringLiteral("about:blank") };
    bool m_webview_created { false };
    bool m_empty_new_tab { true };

    bool m_crashed { false };
    bool m_internal_page_active { false };
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
    // Present duty-cycle cap (presents are self-paced; vsync is off at the swap):
    // a slow present stretches the admission gap, bounding presents to ~50% of
    // main-thread time (docs/DEVIATIONS.md §0d).
    qint64 m_last_present_request_ms { -1000 };
    qint64 m_last_present_duration_ms { 0 };
    bool m_present_throttle_scheduled { false };
    double m_ctrl_wheel_zoom_remainder { 0.0 };
    QHash<int, QPointF> m_active_touch_points;
    bool m_pen_active { false };
};

} // namespace ServoQ
