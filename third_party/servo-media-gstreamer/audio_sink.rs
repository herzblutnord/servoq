/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

use std::cell::{Cell, RefCell};
use std::sync::Arc;
use std::sync::mpsc::Sender;
use std::thread::Builder;

use byte_slice_cast::*;
use gstreamer::prelude::*;
use gstreamer_app::{AppSrc, AppSrcCallbacks};
use servo_media_audio::block::{Chunk, FRAMES_PER_BLOCK};
use servo_media_audio::render_thread::AudioRenderThreadMsg;
use servo_media_audio::sink::{AudioSink, AudioSinkError};
use servo_media_streams::MediaSocket;
use {gstreamer, gstreamer_audio};

use crate::media_stream::GstreamerMediaSocket;

const DEFAULT_SAMPLE_RATE: f32 = 44100.;

pub struct GStreamerAudioSink {
    pipeline: gstreamer::Pipeline,
    appsrc: Arc<AppSrc>,
    sample_rate: Cell<f32>,
    audio_info: RefCell<Option<gstreamer_audio::AudioInfo>>,
    sample_offset: Cell<u64>,
}

impl GStreamerAudioSink {
    pub fn new() -> Result<Self, AudioSinkError> {
        if let Some(category) = gstreamer::DebugCategory::get("openslessink") {
            category.set_threshold(gstreamer::DebugLevel::Trace);
        }
        gstreamer::init().map_err(|error| {
            AudioSinkError::Backend(format!("GStreamer init failed: {error:?}"))
        })?;

        let appsrc = gstreamer::ElementFactory::make("appsrc")
            .build()
            .map_err(|error| {
                AudioSinkError::Backend(format!("appsrc creation failed: {error:?}"))
            })?;
        let appsrc = appsrc
            .downcast::<AppSrc>()
            .map_err(|_| AudioSinkError::Backend("appsrc is not an AppSrc element".to_owned()))?;

        Ok(Self {
            pipeline: gstreamer::Pipeline::new(),
            appsrc: Arc::new(appsrc),
            sample_rate: Cell::new(DEFAULT_SAMPLE_RATE),
            audio_info: RefCell::new(None),
            sample_offset: Cell::new(0),
        })
    }
}

impl GStreamerAudioSink {
    fn set_audio_info(&self, sample_rate: f32, channels: u8) -> Result<(), AudioSinkError> {
        let audio_info = gstreamer_audio::AudioInfo::builder(
            gstreamer_audio::AUDIO_FORMAT_F32,
            sample_rate as u32,
            channels.into(),
        )
        .build()
        .map_err(|error| AudioSinkError::Backend(format!("AudioInfo failed: {error:?}")))?;
        self.appsrc.set_caps(audio_info.to_caps().ok().as_ref());
        *self.audio_info.borrow_mut() = Some(audio_info);
        Ok(())
    }

    fn set_channels_if_changed(&self, channels: u8) -> Result<(), AudioSinkError> {
        let curr_channels = match self.audio_info.borrow().as_ref() {
            Some(ch) => ch.channels(),
            _ => {
                return Ok(());
            },
        };
        if channels != curr_channels as u8 {
            self.set_audio_info(self.sample_rate.get(), channels)?;
        }
        Ok(())
    }
}

impl AudioSink for GStreamerAudioSink {
    fn init(
        &self,
        sample_rate: f32,
        graph_thread_channel: Sender<AudioRenderThreadMsg>,
    ) -> Result<(), AudioSinkError> {
        self.sample_rate.set(sample_rate);
        self.set_audio_info(sample_rate, 2)?;
        self.appsrc.set_format(gstreamer::Format::Time);

        // Allow only a single chunk.
        self.appsrc.set_max_bytes(1);

        // ServoQ patch: do not `.unwrap()` the thread spawn. If the OS refuses a
        // new thread we wire the `need-data` callback synchronously on this
        // thread instead of aborting the process. (`Sender` is `Clone`, so the
        // fallback keeps the original channel while the thread gets a clone.)
        let appsrc_for_thread = self.appsrc.clone();
        let sender_for_thread = graph_thread_channel.clone();
        let spawn_result = Builder::new()
            .name("GstAppSrcCallbacks".to_owned())
            .spawn(move || {
                appsrc_for_thread.set_callbacks(
                    AppSrcCallbacks::builder()
                        .need_data(move |_: &AppSrc, _: u32| {
                            if let Err(e) =
                                sender_for_thread.send(AudioRenderThreadMsg::SinkNeedData)
                            {
                                log::warn!("Error sending need data event: {:?}", e);
                            }
                        })
                        .build(),
                );
            });
        if spawn_result.is_err() {
            log::warn!("[servoq] GstAppSrcCallbacks thread spawn failed; wiring callback inline");
            let appsrc_inline = self.appsrc.clone();
            appsrc_inline.set_callbacks(
                AppSrcCallbacks::builder()
                    .need_data(move |_: &AppSrc, _: u32| {
                        if let Err(e) = graph_thread_channel.send(AudioRenderThreadMsg::SinkNeedData)
                        {
                            log::warn!("Error sending need data event: {:?}", e);
                        }
                    })
                    .build(),
            );
        }

        let appsrc = self.appsrc.as_ref().clone().upcast();
        let resample = gstreamer::ElementFactory::make("audioresample")
            .build()
            .map_err(|error| {
                AudioSinkError::Backend(format!("audioresample creation failed: {error:?}"))
            })?;
        let convert = gstreamer::ElementFactory::make("audioconvert")
            .build()
            .map_err(|error| {
                AudioSinkError::Backend(format!("audioconvert creation failed: {error:?}"))
            })?;
        // ServoQ patch: prefer native pipewiresink with graceful fallback
        // instead of always using autoaudiosink (see servoq_audio.rs).
        let sink = crate::servoq_audio::pick_audio_sink().map_err(AudioSinkError::Backend)?;
        self.pipeline
            .add_many([&appsrc, &resample, &convert, &sink])
            .map_err(|error| AudioSinkError::Backend(error.to_string()))?;
        gstreamer::Element::link_many([&appsrc, &resample, &convert, &sink])
            .map_err(|error| AudioSinkError::Backend(error.to_string()))?;

        Ok(())
    }

    fn init_stream(
        &self,
        channels: u8,
        sample_rate: f32,
        socket: Box<dyn MediaSocket>,
    ) -> Result<(), AudioSinkError> {
        self.sample_rate.set(sample_rate);
        self.set_audio_info(sample_rate, channels)?;
        self.appsrc.set_format(gstreamer::Format::Time);

        // Do not set max bytes or callback, we will push as needed

        let appsrc = self.appsrc.as_ref().clone().upcast();
        let convert = gstreamer::ElementFactory::make("audioconvert")
            .build()
            .map_err(|error| {
                AudioSinkError::Backend(format!("audioconvert creation failed: {error:?}"))
            })?;
        let sink = socket
            .as_any()
            .downcast_ref::<GstreamerMediaSocket>()
            .ok_or_else(|| {
                AudioSinkError::Backend("media socket is not a GstreamerMediaSocket".to_owned())
            })?
            .proxy_sink()
            .clone();

        self.pipeline
            .add_many([&appsrc, &convert, &sink])
            .map_err(|error| AudioSinkError::Backend(error.to_string()))?;
        gstreamer::Element::link_many([&appsrc, &convert, &sink])
            .map_err(|error| AudioSinkError::Backend(error.to_string()))?;

        Ok(())
    }

    fn play(&self) -> Result<(), AudioSinkError> {
        self.pipeline
            .set_state(gstreamer::State::Playing)
            .map(|_| ())
            .map_err(|_| AudioSinkError::StateChangeFailed)
    }

    fn stop(&self) -> Result<(), AudioSinkError> {
        self.pipeline
            .set_state(gstreamer::State::Paused)
            .map(|_| ())
            .map_err(|_| AudioSinkError::StateChangeFailed)
    }

    fn has_enough_data(&self) -> bool {
        self.appsrc.current_level_bytes() >= self.appsrc.max_bytes()
    }

    fn push_data(&self, mut chunk: Chunk) -> Result<(), AudioSinkError> {
        if let Some(block) = chunk.blocks.first() {
            self.set_channels_if_changed(block.chan_count())?;
        }

        // ServoQ patch: this runs on servo-media's audio render thread for every
        // block. None of the steps below may panic — a panic here would either
        // poison the render thread (killing all audio) or, with panic=abort,
        // take down the whole browser. Every former unwrap/expect/assert now
        // returns an AudioSinkError, which servo-media handles as a soft failure.
        let sample_rate = self.sample_rate.get() as u64;
        let audio_info_ref = self.audio_info.borrow();
        let Some(audio_info) = audio_info_ref.as_ref() else {
            return Err(AudioSinkError::Backend(
                "push_data called before audio info was set".to_owned(),
            ));
        };
        let channels = audio_info.channels();
        let bpf = audio_info.bpf() as usize;
        if bpf != 4 * channels as usize {
            return Err(AudioSinkError::Backend(format!(
                "unexpected bytes-per-frame {bpf} for {channels} f32 channels"
            )));
        }
        let n_samples = FRAMES_PER_BLOCK.0;
        let buf_size = (n_samples as usize) * (bpf);
        let mut buffer = gstreamer::Buffer::with_size(buf_size)
            .map_err(|_| AudioSinkError::Backend("audio buffer allocation failed".to_owned()))?;
        {
            let Some(buffer) = buffer.get_mut() else {
                return Err(AudioSinkError::BufferPushFailed);
            };
            let mut sample_offset = self.sample_offset.get();
            // Calculate the current timestamp (PTS) and the next one,
            // and calculate the duration from the difference instead of
            // simply the number of samples to prevent rounding errors.
            // `mul_div_floor` returns None on overflow or a zero sample rate;
            // fall back to 0 rather than panicking.
            let pts = gstreamer::ClockTime::from_nseconds(
                sample_offset
                    .mul_div_floor(gstreamer::ClockTime::SECOND.nseconds(), sample_rate)
                    .unwrap_or(0),
            );
            let next_pts: gstreamer::ClockTime = gstreamer::ClockTime::from_nseconds(
                (sample_offset + n_samples)
                    .mul_div_floor(gstreamer::ClockTime::SECOND.nseconds(), sample_rate)
                    .unwrap_or(0),
            );
            buffer.set_pts(Some(pts));
            buffer.set_duration(next_pts.checked_sub(pts).unwrap_or(gstreamer::ClockTime::ZERO));

            // sometimes nothing reaches the output
            if chunk.is_empty() {
                chunk.blocks.push(Default::default());
                chunk.blocks[0].repeat(channels as u8);
            }
            debug_assert!(chunk.len() == 1);
            let Some(block) = chunk.blocks.first_mut() else {
                return Err(AudioSinkError::BufferPushFailed);
            };
            let mut data = block.interleave();
            let data = data.as_mut_byte_slice();

            // XXXManishearth if we have a safe way to convert
            // from Box<[f32]> to Box<[u8]> (similarly for Vec)
            // we can use Buffer::from_slice instead
            buffer
                .copy_from_slice(0, data)
                .map_err(|_| AudioSinkError::BufferPushFailed)?;

            sample_offset += n_samples;
            self.sample_offset.set(sample_offset);
        }

        self.appsrc
            .push_buffer(buffer)
            .map(|_| ())
            .map_err(|_| AudioSinkError::BufferPushFailed)
    }

    fn set_eos_callback(&self, _: Box<dyn Fn(Box<dyn AsRef<[f32]>>) + Send + Sync + 'static>) {}
}

impl Drop for GStreamerAudioSink {
    fn drop(&mut self) {
        let _ = self.stop();
    }
}
