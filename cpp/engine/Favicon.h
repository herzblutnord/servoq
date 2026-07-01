/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020, Paul Roukema <roukemap@gmail.com>
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2023, Srikavin Ramkumar <me@srikavin.me>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Ladybird:
 *   Libraries/LibWeb/DOM/Document.cpp (check_favicon_after_loading_link_resource)
 *   Libraries/LibWeb/HTML/HTMLLinkElement.cpp (process_icon_resource, decode_favicon,
 *     load_fallback_favicon_if_needed)
 *   Libraries/LibGfx/ImageFormats/ICOLoader.cpp (find_largest_image)
 */
// Favicon.h
//
// Favicon fetching, decoding, and selection for a tab.

#pragma once

class QImage;

namespace ServoQ {

class WebContentView;

// Fetch the current page's icon candidates (<link rel="icon"> in tree order,
// /favicon.ico fallback), decode them (largest ICO frame wins), and apply the
// candidate with the largest pixel area (ties -> last declared in tree order).
void start_favicon_probe(WebContentView* view);

// Apply an already-decoded favicon to the view's tab and persist it to the
// bookmark store (also used by the Servo-native notify_favicon_changed path).
void apply_favicon_bitmap(WebContentView* view, QImage const& image);

// Drop per-tab probe bookkeeping when a tab closes.
void favicon_tab_closed(int tab_id);

}
