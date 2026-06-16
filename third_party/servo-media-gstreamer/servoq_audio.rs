/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

//! ServoQ patch module: audio-output sink selection and a one-time codec
//! capability check. Kept in a single file (not scattered through the upstream
//! sources) so re-vendoring this crate against a newer servo-media stays cheap
//! — see docs/DEVIATIONS.md §0l. None of this exists upstream.

use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::OnceLock;

/// Run a GStreamer-thread callback body, catching any Rust panic so it cannot
/// unwind across the C FFI boundary (which is undefined behavior and in
/// practice aborts the whole process). On panic it logs and returns `fallback`,
/// so a single malformed sample at most drops audio for that element instead of
/// taking down the browser. Like Firefox/Chromium, ServoQ never lets a Rust
/// fault propagate into the media library's own threads.
pub fn guard<T>(what: &'static str, f: impl FnOnce() -> T, fallback: T) -> T {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(value) => value,
        Err(_) => {
            log::error!("[servoq] caught panic in {what}; recovering without aborting");
            fallback
        },
    }
}

/// Output sinks ServoQ will try, in order, when none is forced via
/// `SERVOQ_AUDIO_SINK`. `pipewiresink` first so a modern PipeWire system uses
/// the native element (lowest latency, real PipeWire integration);
/// `pulsesink` next (works through `pipewire-pulse` or a real PulseAudio);
/// `autoaudiosink` is GStreamer's own auto-selection; `alsasink` is the
/// last-resort raw backend.
const SINK_PRIORITY: &[&str] = &["pipewiresink", "pulsesink", "autoaudiosink", "alsasink"];

/// Web-platform audio/video codecs a "big browser" is expected to play, mapped
/// to the GStreamer decoder element that provides each. Used only to emit a
/// human-readable startup warning when a thin system is missing plugins; it
/// never fails playback (a missing codec already surfaces as PlayerEvent::Error
/// at decode time).
const WEB_CODECS: &[(&str, &[&str])] = &[
    ("AAC", &["avdec_aac", "faad"]),
    ("MP3", &["avdec_mp3", "mpg123audiodec", "mad"]),
    ("Opus", &["opusdec"]),
    ("Vorbis", &["vorbisdec"]),
    ("FLAC", &["flacdec"]),
    ("H.264", &["avdec_h264"]),
    ("VP8/VP9", &["vp8dec", "vp9dec", "avdec_vp8", "avdec_vp9"]),
    ("AV1", &["dav1ddec", "av1dec", "avdec_av1"]),
];

/// Build the audio output sink ServoQ should use, honoring a
/// `SERVOQ_AUDIO_SINK=<element>` override and otherwise walking [`SINK_PRIORITY`]
/// and returning the first element that instantiates. Logs the chosen sink once.
///
/// Returning a concrete element (rather than relying on `playbin`/`Play`'s
/// built-in `autoaudiosink`) is what lets ServoQ prefer native PipeWire while
/// still degrading gracefully on systems without it.
pub fn pick_audio_sink() -> Result<gstreamer::Element, String> {
    if let Ok(forced) = std::env::var("SERVOQ_AUDIO_SINK") {
        let forced = forced.trim();
        if !forced.is_empty() {
            return gstreamer::ElementFactory::make(forced)
                .build()
                .map(|e| {
                    log_chosen_sink(forced);
                    e
                })
                .map_err(|err| format!("SERVOQ_AUDIO_SINK={forced} could not be created: {err:?}"));
        }
    }

    for name in SINK_PRIORITY {
        if let Ok(element) = gstreamer::ElementFactory::make(name).build() {
            log_chosen_sink(name);
            return Ok(element);
        }
    }

    Err(format!(
        "no usable audio sink found (tried {})",
        SINK_PRIORITY.join(", ")
    ))
}

fn log_chosen_sink(name: &str) {
    static LOGGED: OnceLock<()> = OnceLock::new();
    LOGGED.get_or_init(|| {
        log::info!("[servoq] audio output sink: {name}");
    });
}

/// Warn once at startup about web codecs whose GStreamer decoder is missing, so
/// a stripped-down system surfaces "this build can't play AAC/H.264/…" in the
/// log instead of failing silently per page. GStreamer must already be
/// initialized before this is called.
pub fn log_codec_capabilities() {
    static LOGGED: OnceLock<()> = OnceLock::new();
    LOGGED.get_or_init(|| {
        let missing: Vec<&str> = WEB_CODECS
            .iter()
            .filter(|(_, elements)| {
                !elements
                    .iter()
                    .any(|el| gstreamer::ElementFactory::find(el).is_some())
            })
            .map(|(codec, _)| *codec)
            .collect();

        if missing.is_empty() {
            log::info!("[servoq] all common web audio/video codecs are available");
        } else {
            log::warn!(
                "[servoq] missing GStreamer decoders for: {} — pages using these formats will \
                 fail to play. Install the matching gst-plugins (e.g. gst-libav, \
                 gst-plugins-{{good,bad,ugly}}).",
                missing.join(", ")
            );
        }
    });
}
