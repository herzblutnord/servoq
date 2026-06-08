#pragma once

#include <QString>
#include <optional>

namespace ServoQ::WebViewURL {

enum class AppendTLD {
    No,
    Yes,
};

std::optional<QString> sanitize_url(QString const& location, AppendTLD append_tld = AppendTLD::No);
bool location_looks_like_url(QString const& location, AppendTLD append_tld = AppendTLD::No);
QString url_for_display(QString const& url);

}
