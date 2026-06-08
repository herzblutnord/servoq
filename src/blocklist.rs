// Copyright (c) 2026-present, the Ladybird developers.
// SPDX-License-Identifier: BSD-2-Clause
//
// Derived from Ladybird:
//   Libraries/LibWeb/Loader/ContentBlocker.cpp
// EasyList-compatible ad/tracker blocking engine backed by the `adblock` crate.
// The bundled list at data/blocklist.txt is loaded at first use.
// A user-supplied list at $XDG_CONFIG_HOME/servoq/blocklist.txt is merged on top.

#[cfg(feature = "servo-engine")]
mod inner {
    use adblock::{
        Engine,
        lists::{FilterSet, ParseOptions},
        request::Request,
    };
    use std::cell::RefCell;

    // adblock::Engine contains Rc<> and RefCell<> so it is !Send + !Sync.
    // ServoQ is single-threaded (Qt main thread), so thread_local is correct.
    thread_local! {
        static ENGINE: RefCell<Option<Engine>> = const { RefCell::new(None) };
    }

    const BUILTIN_RULES: &str = include_str!("../data/blocklist.txt");

    fn build_engine() -> Engine {
        let opts = ParseOptions::default();
        let mut filter_set = FilterSet::new(false);
        filter_set.add_filter_list(BUILTIN_RULES, opts.clone());
        if let Some(user_rules) = load_user_rules() {
            filter_set.add_filter_list(&user_rules, opts);
        }
        Engine::from_filter_set(filter_set, true)
    }

    fn load_user_rules() -> Option<String> {
        let path = user_blocklist_path();
        std::fs::read_to_string(path).ok()
    }

    pub fn user_blocklist_path() -> String {
        let base = std::env::var("XDG_CONFIG_HOME")
            .unwrap_or_else(|_| {
                let home = std::env::var("HOME").unwrap_or_else(|_| ".".to_string());
                format!("{home}/.config")
            });
        format!("{base}/servoq/blocklist.txt")
    }

    fn debug_enabled() -> bool {
        std::env::var_os("SERVOQ_CONTENT_BLOCKING_DEBUG").is_some()
            || std::env::var_os("SERVOQ_PERF").is_some()
    }

    pub fn reload_blocklists() -> bool {
        ENGINE.with(|cell| {
            *cell.borrow_mut() = Some(build_engine());
        });
        true
    }

    pub fn should_block(url: &url::Url, source_url: &url::Url, request_type: &str) -> bool {
        ENGINE.with(|cell| {
            let mut guard = cell.borrow_mut();
            if guard.is_none() {
                *guard = Some(build_engine());
            }
            let engine = guard.as_ref().unwrap();
            match Request::new(url.as_str(), source_url.as_str(), request_type) {
                Ok(req) => {
                    let result = engine.check_network_request(&req);
                    if result.matched && debug_enabled() {
                        eprintln!(
                            "[servoq blocklist] blocked url={} source={} type={} rule={}",
                            url,
                            source_url,
                            request_type,
                            result.filter.as_deref().unwrap_or("<unknown>")
                        );
                    }
                    result.matched
                }
                Err(_) => false,
            }
        })
    }
}

#[cfg(feature = "servo-engine")]
pub use inner::{reload_blocklists, should_block, user_blocklist_path};

#[cfg(not(feature = "servo-engine"))]
pub fn reload_blocklists() -> bool { false }

#[cfg(not(feature = "servo-engine"))]
pub fn user_blocklist_path() -> String { String::new() }
