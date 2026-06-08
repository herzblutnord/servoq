/*
 * Copyright (c) 2024-2025, Valentin Gusel
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

#include <QWidget>

class QLabel;

namespace ServoQ {

class WebContentPlaceholder final : public QWidget {
public:
    explicit WebContentPlaceholder(QWidget* parent = nullptr);
    void setUrl(QString const& url);
    void setStatus(QString const& status);

protected:
    bool event(QEvent* event) override;

private:
    void updateChromeStyle();

    QLabel* m_title { nullptr };
    QLabel* m_url { nullptr };
    QLabel* m_status { nullptr };
    bool m_is_updating_chrome_style { false };
};

}
