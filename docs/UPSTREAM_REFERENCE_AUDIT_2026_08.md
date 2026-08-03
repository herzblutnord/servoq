# Upstream reference audit — August 2026

ServoQ keeps full upstream checkouts under `vendor/` as read-only design and API
references. On 3 August 2026 they were updated to:

- Ladybird `master`: `ee8ae05c80f8` (`LibWeb: Resolve relative positions...`)
- Servo `origin/main`: `da291ca3c487` (`layout: Allow shaping across inline box boundaries`)

The Servo checkout is intentionally detached at `origin/main`; ServoQ still
builds the published Servo 0.4.0 crate, not an unreleased `main` revision.

## ServoShell changes applied

- File drops open local files, including one background tab per extra file
  (Servo #45454).
- `<select multiple>` returns all checked option IDs and permits an accepted
  empty selection (Servo #45419).
- Host or localhost input with an explicit port infers `http://`, matching the
  parser fix and avoiding Qt treating the host as a scheme (Servo #45729 and
  its custom-scheme follow-up #45832).
- Finger and tablet input use Servo 0.4's typed touch/pen events, including
  cancellation and stable multitouch IDs.

ServoQ already had the remaining applicable June shell behavior: horizontal tab
overflow controls, fullscreen on the current screen, synchronous resize of its
rendering context, and HTTPS inference for ordinary host names.

ServoShell's `servo:config` and `servo:newtab` protocol handler is shell-owned;
ServoQ uses its richer `servoq:` settings/new-tab pages. Android, OHOS, Windows
console, and winit-only changes do not apply to this Qt Linux frontend. The
optional native gamepad backend is not a Servo 0.4 release-post feature; ServoQ
handles the new permission variant but does not claim hardware gamepad input.

ServoQ also exposes Servo's release-improved remote DevTools protocol through a
disabled-by-default loopback server and a native permission prompt; no separate
Ladybird DevTools client is needed for that protocol-level functionality.

## Ladybird Qt changes applied

- `e3f871dd41`: add **Paste and Go** to the location context menu.
- `375da3e66d`: show refreshed history suggestions when the location editor is
  focused.
- `18612e74d2`: restore page focus after the find bar closes.
- `843554dd9e`: clear the pressed-tab state before a tab context menu opens.
- `c2312b5826`: use Qt's hover state so the new-tab highlight cannot stick.
- `1a31f61e74`: avoid forwarding the second native-window mouse press twice on
  a double click.
- `3fa482f5f8`: prefer precise pixel deltas for touchpads and angle deltas for
  physical mouse wheels.
- `b6ef019d97`: preserve the page cursor while the link-hover label overlays the
  content.
- Current Ladybird fullscreen behavior: hide browser chrome while content fills
  the window, then restore the configured chrome state on exit.

## Reviewed but not copied

- Ladybird download UI needs an engine download API that Servo 0.4 does not
  expose; ServoQ's existing milestone remains accurate.
- Private windows/profiles require true Servo storage isolation, not only a Qt
  chrome change.
- Tab previews need reliable asynchronous Servo snapshots; grabbing the native
  Wayland surface would be fragile.
- Ladybird's missing-find tint relies on result-state information that Servo's
  current public find API does not provide.
- Ladybird's AppKit, Vulkan, LibWeb, DevTools-client, and accessibility-tree
  changes are backend-specific or require APIs ServoQ does not have. They were
  used as design references only.

This audit is deliberately selective: it ports behavior with a clean equivalent
in ServoQ's Qt/Servo architecture and records why superficially similar changes
were not copied.
