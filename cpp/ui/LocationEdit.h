/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Ladybird:
 *   UI/Qt/LocationEdit.h
 */
#pragma once

#include <QLineEdit>
#include <optional>

class QAction;
class QCompleter;
class QContextMenuEvent;
class QEvent;
class QFocusEvent;
class QModelIndex;
class QGraphicsDropShadowEffect;
class QKeyEvent;
class QMouseEvent;
class QResizeEvent;
class QStandardItemModel;
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
    void setZoomAction(QAction* action);

protected:
    void changeEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateChromeStyle();
    void updateButtonPositions();
    void updateLocationIcon();
    void updateZoomIndicator();
    int trailingTextMargin() const;
    void animateFocusGlow(int target_alpha);
    void updateFocusGlow(int alpha);
    void updateHistorySuggestions(QString const& query);
    bool activateHistorySuggestion(QModelIndex const& index);

    QCompleter* m_history_completer { nullptr };
    QStandardItemModel* m_history_completion_model { nullptr };
    QToolButton* m_leading_icon { nullptr };
    QToolButton* m_trailing_action { nullptr };
    QToolButton* m_zoom_indicator_button { nullptr };
    QVariantAnimation* m_focus_glow_animation { nullptr };
    QGraphicsDropShadowEffect* m_focus_glow_effect { nullptr };
    QAction* m_zoom_action { nullptr };
    std::optional<QString> m_url { QStringLiteral("about:blank") };
    bool m_is_updating_chrome_style { false };
    bool m_should_show_full_url_on_mouse_release { false };
    int m_text_leading_margin { 0 };
    int m_focus_glow_alpha { 0 };
};

}
