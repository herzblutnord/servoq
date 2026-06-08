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

    pub fn should_block(url: &url::Url) -> bool {
        ENGINE.with(|cell| {
            let mut guard = cell.borrow_mut();
            if guard.is_none() {
                *guard = Some(build_engine());
            }
            let engine = guard.as_ref().unwrap();
            let source = url.origin().unicode_serialization();
            match Request::new(url.as_str(), &source, "other") {
                Ok(req) => engine.check_network_request(&req).matched,
                Err(_) => false,
            }
        })
    }
}

#[cfg(feature = "servo-engine")]
pub use inner::should_block;
