/*
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Ladybird:
 *   UI/Qt/FindInPageWidget.h
 */
#pragma once

#include <QWidget>
#include <functional>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace ServoQ {

class FindInPageWidget final : public QWidget {
public:
    explicit FindInPageWidget(QWidget* parent = nullptr);
    QString query() const;
    std::function<void()> onShown;
    std::function<void()> onHidden;

protected:
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void updateChromeStyle();
    void updateResultLabel();

    QLineEdit* m_find_text { nullptr };
    QPushButton* m_previous_button { nullptr };
    QPushButton* m_next_button { nullptr };
    QPushButton* m_exit_button { nullptr };
    QCheckBox* m_match_case { nullptr };
    QLabel* m_result_label { nullptr };
    bool m_is_updating_chrome_style { false };
};

}
