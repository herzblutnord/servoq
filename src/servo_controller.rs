use std::sync::{Mutex, OnceLock};

#[derive(Clone, Debug)]
pub struct ServoControllerState {
    pub id: i32,
    pub history: Vec<String>,
    pub history_index: usize,
    pub title: String,
    pub loading: bool,
    pub status_text: String,
    pub find_bar_visible: bool,
}

impl ServoControllerState {
    fn new(id: i32) -> Self {
        Self {
            id,
            history: vec!["about:blank".to_string()],
            history_index: 0,
            title: "New Tab".to_string(),
            loading: false,
            status_text: "Servo renderer placeholder is idle".to_string(),
            find_bar_visible: false,
        }
    }

    fn current_url(&self) -> &str {
        self.history
            .get(self.history_index)
            .map(String::as_str)
            .unwrap_or("about:blank")
    }

    fn set_url(&mut self, url: &str) {
        let url = normalize_url(url);
        self.history.truncate(self.history_index + 1);
        self.history.push(url);
        self.history_index = self.history.len() - 1;
        self.title = title_for_url(self.current_url());
        self.loading = false;
        self.status_text = format!("Loaded placeholder page {}", self.current_url());
    }
}

#[derive(Debug)]
struct ControllerStore {
    next_id: i32,
    tabs: Vec<ServoControllerState>,
}

impl Default for ControllerStore {
    fn default() -> Self {
        Self {
            next_id: 1,
            tabs: Vec::new(),
        }
    }
}

static CONTROLLER: OnceLock<Mutex<ControllerStore>> = OnceLock::new();

fn controller() -> &'static Mutex<ControllerStore> {
    CONTROLLER.get_or_init(|| Mutex::new(ControllerStore::default()))
}

fn with_store<R>(f: impl FnOnce(&mut ControllerStore) -> R) -> R {
    let mut store = controller()
        .lock()
        .expect("Servo controller placeholder mutex was poisoned");
    f(&mut store)
}

fn with_tab<R>(id: i32, fallback: R, f: impl FnOnce(&mut ServoControllerState) -> R) -> R {
    with_store(|store| {
        let Some(tab) = store.tabs.iter_mut().find(|tab| tab.id == id) else {
            return fallback;
        };
        f(tab)
    })
}

pub fn create_tab() -> i32 {
    with_store(|store| {
        let id = store.next_id;
        store.next_id += 1;
        store.tabs.push(ServoControllerState::new(id));
        id
    })
}

pub fn close_tab(id: i32) {
    with_store(|store| {
        store.tabs.retain(|tab| tab.id != id);
    });
}

pub fn load_url(id: i32, url: &str) {
    with_tab(id, (), |state| {
        let url = normalize_url(url);
        state.loading = true;
        state.status_text = format!("Started placeholder navigation to {url}");
        state.set_url(&url);
    });
}

pub fn go_back(id: i32) {
    with_tab(id, (), |state| {
        if state.history_index > 0 {
            state.history_index -= 1;
            state.title = title_for_url(state.current_url());
            state.status_text = format!("Went back to {}", state.current_url());
        } else {
            state.status_text = "Back history is empty".to_string();
        }
    });
}

pub fn go_forward(id: i32) {
    with_tab(id, (), |state| {
        if state.history_index + 1 < state.history.len() {
            state.history_index += 1;
            state.title = title_for_url(state.current_url());
            state.status_text = format!("Went forward to {}", state.current_url());
        } else {
            state.status_text = "Forward history is empty".to_string();
        }
    });
}

pub fn reload(id: i32) {
    with_tab(id, (), |state| {
        state.loading = true;
        state.status_text = format!("Reloaded placeholder page {}", state.current_url());
        state.loading = false;
    });
}

pub fn toggle_bookmark(id: i32) {
    with_tab(id, (), |state| {
        state.status_text = format!("Bookmark toggle placeholder for {}", state.current_url());
    });
}

pub fn show_find_in_page(id: i32) {
    with_tab(id, (), |state| {
        state.find_bar_visible = true;
        state.status_text = "Find in page placeholder shown".to_string();
    });
}

pub fn hide_find_in_page(id: i32) {
    with_tab(id, (), |state| {
        state.find_bar_visible = false;
        state.status_text = "Find in page placeholder hidden".to_string();
    });
}

pub fn current_url(id: i32) -> String {
    with_tab(id, "about:blank".to_string(), |state| {
        state.current_url().to_string()
    })
}

pub fn title(id: i32) -> String {
    with_tab(id, "New Tab".to_string(), |state| state.title.clone())
}

pub fn loading(id: i32) -> bool {
    with_tab(id, false, |state| state.loading)
}

pub fn can_go_back(id: i32) -> bool {
    with_tab(id, false, |state| state.history_index > 0)
}

pub fn can_go_forward(id: i32) -> bool {
    with_tab(id, false, |state| {
        state.history_index + 1 < state.history.len()
    })
}

pub fn status_text(id: i32) -> String {
    with_tab(
        id,
        "Servo renderer placeholder is idle".to_string(),
        |state| state.status_text.clone(),
    )
}

pub fn find_bar_visible(id: i32) -> bool {
    with_tab(id, false, |state| state.find_bar_visible)
}

fn normalize_url(url: &str) -> String {
    let trimmed = url.trim();
    if trimmed.is_empty() {
        "about:blank".to_string()
    } else {
        trimmed.to_string()
    }
}

fn title_for_url(url: &str) -> String {
    if url == "about:blank" {
        return "New Tab".to_string();
    }

    let without_scheme = url.split_once("://").map(|(_, rest)| rest).unwrap_or(url);
    let host = without_scheme
        .split(['/', '?', '#'])
        .next()
        .filter(|host| !host.is_empty())
        .unwrap_or(url);
    host.to_string()
}
