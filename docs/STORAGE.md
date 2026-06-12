# ServoQ — Profile Storage

How ServoQ persists browser data, modeled on how Chromium and Firefox lay out
their profiles (SQLite for high-churn/queryable data, JSON for small trees,
a dedicated session file). Everything lives in the Qt `AppDataLocation`
directory (`~/.local/share/ServoQ/ServoQ/` on Linux); settings stay in the
QSettings INI (`~/.config/ServoQ/ServoQ.ini`).

| Data | File | Format | Reference model |
|---|---|---|---|
| Browsing history | `history.db` | SQLite (`urls` + `visits`) | Chromium `History`, Firefox `places.sqlite` |
| Favicons | `favicons.db` | SQLite (PNG blobs keyed by host) | Chromium `Favicons` |
| Bookmarks | `bookmarks.json` | JSON tree | Chromium `Bookmarks` |
| Session (open tabs, active tab, pinned flags, recently closed) | `session.json` | JSON | Firefox `sessionstore.jsonlz4` (uncompressed; it is tiny) |
| Settings, window geometry, search engines | QSettings INI | INI | — |
| Content-blocking lists | `blocklists/` | EasyList text | — |

## Rules (learned the hard way — see DEVIATIONS.md §0e)

- **No synchronous disk I/O on the UI thread per navigation event.** Both
  SQLite stores batch writes in memory and flush in one transaction behind a
  1 s single-shot timer, plus a final flush on `aboutToQuit`. The databases
  run WAL + `synchronous=NORMAL`, so a commit appends to the log without an
  fsync of the main file.
- **Per-keystroke work never touches the database.** URL-bar autocomplete
  scans an in-memory index of the most recent 4000 URLs with precomputed
  lowercase search fields; the DB being 100k+ URLs doesn't change typing
  latency.
- **Privacy actions are synchronous.** "Clear History" deletes immediately
  and truncates the WAL (`wal_checkpoint(TRUNCATE)`) so cleared rows don't
  linger in the log file.
- **Session state is not QSettings.** It changes on every tab event; keeping
  it in the INI rewrote the whole settings file once per debounce window.
- **Bookmarks hold no icon payloads.** Embedded base64 favicons used to force
  a full `bookmarks.json` rewrite whenever a bookmarked site refreshed its
  icon; icons live in `favicons.db` and are looked up by host.

## history.db

```sql
urls   (id, url UNIQUE, title, visit_count, last_visit_time)   -- one row per known URL
visits (id, url_id → urls, visit_time)                         -- one row per visit
```

`urls` rows are kept indefinitely (they are what autocomplete and the history
menu read). `visits` rows older than 90 days are expired at startup, matching
Chromium's history retention. SPA title churn updates the existing `urls` row
without inflating `visit_count`.

Migration: a legacy `history.json` (capped 1000-entry store) is imported on
first run and renamed to `history.json.imported`.

## favicons.db

```sql
icons (host PRIMARY KEY, png BLOB, updated_at)
```

Written by the favicon probe (`cpp/Favicon.cpp` → `FaviconStore`); read by
restored session tabs, the bookmarks bar, history/recently-closed menus, and
URL-bar autocomplete. Mapping is per host (not per page URL) because ServoQ
re-probes on every load; the cache only feeds chrome UI. Legacy base64 icons
embedded in `bookmarks.json` are imported on first load.

## session.json

```json
{ "tabs": [ { "url": "…", "pinned": true } ],
  "active_tab": 0,
  "closed_tabs": [ { "url": "…", "title": "…", "pinned": false } ] }
```

`tabs` is only written while "continue where you left off" is enabled, and is
cleared when the setting is turned off. `closed_tabs` (capped at 25) is always
written so "Reopen Closed Tab" / Ctrl+Shift+T works across restarts, like
Chrome's `Tabs_*` log and Firefox's sessionstore. A legacy QSettings session
blob (`session/tabs_json`) is imported once and removed.
