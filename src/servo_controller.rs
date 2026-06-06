use std::sync::{Mutex, OnceLock};

#[derive(Clone, Debug)]
pub struct ServoControllerState {
    pub current_url: String,
    pub title: String,
    pub loading: bool,
    pub can_go_back: bool,
    pub can_go_forward: bool,
    pub status_text: String,
    pub bookmarks_bar_visible: bool,
    pub find_bar_visible: bool,
}

impl Default for ServoControllerState {
    fn default() -> Self {
        Self {
            current_url: "about:blank".to_string(),
            title: "New Tab".to_string(),
            loading: false,
            can_go_back: false,
            can_go_forward: false,
            status_text: "Servo renderer placeholder is idle".to_string(),
            bookmarks_bar_visible: true,
            find_bar_visible: false,
        }
    }
}

static CONTROLLER: OnceLock<Mutex<ServoControllerState>> = OnceLock::new();

fn controller() -> &'static Mutex<ServoControllerState> {
    CONTROLLER.get_or_init(|| Mutex::new(ServoControllerState::default()))
}

fn with_controller<R>(f: impl FnOnce(&mut ServoControllerState) -> R) -> R {
    let mut state = controller()
        .lock()
        .expect("Servo controller placeholder mutex was poisoned");
    f(&mut state)
}

pub fn load_url(url: &str) {
    with_controller(|state| {
        state.current_url = if url.trim().is_empty() {
            "about:blank".to_string()
        } else {
            url.trim().to_string()
        };
        state.title = title_for_url(&state.current_url);
        state.loading = false;
        state.can_go_back = true;
        state.status_text = format!("Queued placeholder navigation to {}", state.current_url);
    });
}

pub fn go_back() {
    with_controller(|state| {
        state.status_text = "Back navigation will be handled by Servo later".to_string();
    });
}

pub fn go_forward() {
    with_controller(|state| {
        state.status_text = "Forward navigation will be handled by Servo later".to_string();
    });
}

pub fn reload() {
    with_controller(|state| {
        state.status_text = format!("Reload placeholder for {}", state.current_url);
    });
}

pub fn new_tab() {
    with_controller(|state| {
        *state = ServoControllerState::default();
        state.status_text = "New tab placeholder created".to_string();
    });
}

pub fn close_tab(index: i32) {
    with_controller(|state| {
        state.status_text = format!("Close tab placeholder for index {index}");
    });
}

pub fn toggle_bookmark() {
    with_controller(|state| {
        state.status_text = format!("Bookmark toggle placeholder for {}", state.current_url);
    });
}

pub fn show_find_in_page() {
    with_controller(|state| {
        state.find_bar_visible = true;
        state.status_text = "Find in page placeholder shown".to_string();
    });
}

pub fn hide_find_in_page() {
    with_controller(|state| {
        state.find_bar_visible = false;
        state.status_text = "Find in page placeholder hidden".to_string();
    });
}

pub fn current_url() -> String {
    with_controller(|state| state.current_url.clone())
}

pub fn title() -> String {
    with_controller(|state| state.title.clone())
}

pub fn loading() -> bool {
    with_controller(|state| state.loading)
}

pub fn can_go_back() -> bool {
    with_controller(|state| state.can_go_back)
}

pub fn can_go_forward() -> bool {
    with_controller(|state| state.can_go_forward)
}

pub fn status_text() -> String {
    with_controller(|state| state.status_text.clone())
}

pub fn bookmarks_bar_visible() -> bool {
    with_controller(|state| state.bookmarks_bar_visible)
}

pub fn find_bar_visible() -> bool {
    with_controller(|state| state.find_bar_visible)
}

fn title_for_url(url: &str) -> String {
    if url == "about:blank" {
        "New Tab".to_string()
    } else {
        format!("ServoQ - {url}")
    }
}
