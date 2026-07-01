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
| Cookies, HSTS | `servo-profile/cookie_jar.json`, `servo-profile/hsts_list.json` | Servo persistent profile files | Browser profile network state |
| Bookmarks | `bookmarks.json` | JSON tree | Chromium `Bookmarks` |
| Session (open tabs, active tab, pinned flags, recently closed) | `session.json` | JSON | Firefox `sessionstore.jsonlz4` (uncompressed; it is tiny) |
| Web permission decisions | `permissions.json` | JSON (origin → feature → allow/block) | Chromium content-settings exceptions in `Preferences` |
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

## Servo profile network state

ServoQ passes a stable Servo profile directory to `ServoBuilder`
(`AppDataLocation/servo-profile`) with non-temporary storage enabled. Servo's
network stack owns cookie parsing and matching for `Set-Cookie`,
`document.cookie`, Domain, Path, Expires/Max-Age, Secure, HttpOnly, and SameSite.
That keeps cookie behavior in the engine instead of duplicating it in Qt.

Servo persists its cookie jar as `cookie_jar.json` rather than the
Firefox/Chromium SQLite cookie schemas. ServoQ treats that as Servo's profile
store for now and keeps it under the same browser profile root as the SQLite
stores above. Session cookies are cleared at startup and again before Servo
shutdown so cookies without `Expires`/`Max-Age` do not become restart-persistent
just because the public cookie jar is now saved on disk.

Audited mechanics (Servo 0.2 `servo-net` `resource_thread.rs`; re-audited
unchanged in 0.3.0 — the cookie-clearing APIs grew an optional completion
callback, ServoQ passes `None` to stay on the synchronous ordered path, see
DEVIATIONS.md §0p):

- The jar/HSTS files are read once at startup and written **only** on the
  resource thread's `Exit` message, which `Servo`'s `Drop` impl triggers and
  waits for — so ServoQ's drop-based `shutdown_servo` does persist them. A
  crash loses cookies acquired that session (Chromium batches every ~30 s; we
  lose more on a crash) but never corrupts the previous jar, because no write
  happens mid-run. A corrupt file is logged and replaced with an empty jar,
  not a crash. The shutdown-side `clear_session_cookies()` is synchronous on
  the same channel as `Exit`, so it is ordered before the write.
- The private-browsing cookie jar is in-memory only and never persisted.
- **HTTP auth is session-only by policy.** Setting a `config_dir` also makes
  Servo persist `auth_cache.json` — plaintext Basic/Digest usernames and
  passwords. Chrome and Firefox keep HTTP auth in memory (saved passwords go
  to the encrypted password store instead), so ServoQ deletes the file before
  Servo loads it and again after the shutdown write
  (`remove_persisted_http_auth_cache` in `src/servo_engine.rs`).
- **Profile directories are chmod 0700** (`AppDataLocation` root and
  `servo-profile`) like a Firefox profile; the cookie jar itself is written
  0644 by Servo, so the directory permission is the protection boundary.
  Cookie values are plaintext on disk — the Firefox model. Chromium-style
  OS-keychain encryption is not possible from the embedder: Servo owns the
  jar serialization and exposes no crypto delegate.
- **Known limitation:** two ServoQ instances sharing the profile clobber each
  other's jar on exit (last writer wins). Firefox/Chromium prevent this with
  a profile lock; ServoQ has no single-instance guard yet.

## permissions.json

```json
{ "https://example.com": { "notifications": "allow", "geolocation": "block" } }
```

Written by `cpp/PermissionStore.cpp` for the M3.3 permission prompt
(`request_permission_sync` in `cpp/WebDialogs.cpp`). Only explicit Allow/Block
answers are stored — dismissing the prompt ("Not Now") denies once and stores
nothing, matching Chrome. Saves are synchronous: permission decisions are
rare, explicit user actions, not navigation-event traffic, so the
no-sync-I/O rule above does not apply. Settings → "Clear Site Permissions"
wipes the store.

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
