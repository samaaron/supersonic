//! MIDI message model + byte parse/encode.
//!
//! Raw status/data bytes become named events, and named events become bytes.
//! Channels are exposed **1-based** (1..=16) to match Sonic Pi's user-facing
//! convention; the wire bytes are 0-based and converted here.
//!
//! midir (CoreMIDI / ALSA / WinMM / Web MIDI) delivers one complete message per
//! input callback, so the parser does not implement running status.

/// A decoded MIDI message. Pressures and pitch-bend follow the MIDI spec
/// (7-bit data, 14-bit bend 0..=16383, centre 8192).
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MidiMessage {
    NoteOff { channel: u8, note: u8, velocity: u8 },
    NoteOn { channel: u8, note: u8, velocity: u8 },
    /// Polyphonic key pressure (0xA0) — Sonic Pi calls this aftertouch.
    PolyPressure { channel: u8, note: u8, pressure: u8 },
    ControlChange { channel: u8, controller: u8, value: u8 },
    ProgramChange { channel: u8, program: u8 },
    /// Channel pressure (0xD0).
    ChannelPressure { channel: u8, pressure: u8 },
    /// 14-bit pitch bend, 0..=16383 (centre 8192).
    PitchBend { channel: u8, value: u16 },
    SysEx(Vec<u8>),
    /// MIDI Time Code quarter frame (0xF1): one 7-bit data byte.
    TimeCodeQuarterFrame(u8),
    /// Song Position Pointer (0xF2): position in MIDI beats (1/16 notes), 14-bit.
    /// The only clock-related message that carries musical position.
    SongPosition(u16),
    /// Song Select (0xF3): one 7-bit song number.
    SongSelect(u8),
    /// Tune Request (0xF6).
    TuneRequest,
    // System real-time.
    Clock,
    Start,
    Continue,
    Stop,
    ActiveSensing,
    Reset,
}

#[inline]
fn d7(b: u8) -> u8 {
    b & 0x7f
}

impl MidiMessage {
    /// Parse a single complete MIDI message. Returns `None` for an empty slice,
    /// a leading non-status byte (running status is not expected from midir), or
    /// a system-common message we don't model yet (MTC quarter-frame, song
    /// position/select, tune request).
    pub fn parse(bytes: &[u8]) -> Option<MidiMessage> {
        let status = *bytes.first()?;
        if status < 0x80 {
            return None;
        }

        // System messages (0xF0..0xFF) are not channel-addressed.
        if status >= 0xF0 {
            return match status {
                0xF0 => Some(MidiMessage::SysEx(bytes.to_vec())),
                0xF1 => Some(MidiMessage::TimeCodeQuarterFrame(d7(*bytes.get(1)?))),
                0xF2 => {
                    let lsb = d7(*bytes.get(1)?) as u16;
                    let msb = d7(*bytes.get(2)?) as u16;
                    Some(MidiMessage::SongPosition((msb << 7) | lsb))
                }
                0xF3 => Some(MidiMessage::SongSelect(d7(*bytes.get(1)?))),
                0xF6 => Some(MidiMessage::TuneRequest),
                0xF8 => Some(MidiMessage::Clock),
                0xFA => Some(MidiMessage::Start),
                0xFB => Some(MidiMessage::Continue),
                0xFC => Some(MidiMessage::Stop),
                0xFE => Some(MidiMessage::ActiveSensing),
                0xFF => Some(MidiMessage::Reset),
                _ => None,
            };
        }

        let channel = (status & 0x0f) + 1; // 0-based wire → 1-based API
        match status & 0xf0 {
            0x80 => Some(MidiMessage::NoteOff {
                channel,
                note: d7(*bytes.get(1)?),
                velocity: d7(*bytes.get(2)?),
            }),
            0x90 => {
                let note = d7(*bytes.get(1)?);
                let velocity = d7(*bytes.get(2)?);
                // Note-on with velocity 0 is the universal "release" idiom (it
                // lets a stream stay in running status). Fold it to note_off so
                // consumers get a single, predictable release event.
                if velocity == 0 {
                    Some(MidiMessage::NoteOff { channel, note, velocity: 0 })
                } else {
                    Some(MidiMessage::NoteOn { channel, note, velocity })
                }
            }
            0xA0 => Some(MidiMessage::PolyPressure {
                channel,
                note: d7(*bytes.get(1)?),
                pressure: d7(*bytes.get(2)?),
            }),
            0xB0 => Some(MidiMessage::ControlChange {
                channel,
                controller: d7(*bytes.get(1)?),
                value: d7(*bytes.get(2)?),
            }),
            0xC0 => Some(MidiMessage::ProgramChange {
                channel,
                program: d7(*bytes.get(1)?),
            }),
            0xD0 => Some(MidiMessage::ChannelPressure {
                channel,
                pressure: d7(*bytes.get(1)?),
            }),
            0xE0 => {
                let lsb = d7(*bytes.get(1)?) as u16;
                let msb = d7(*bytes.get(2)?) as u16;
                Some(MidiMessage::PitchBend {
                    channel,
                    value: (msb << 7) | lsb,
                })
            }
            _ => None,
        }
    }

    /// Channel (1..=16) for channel-voice messages, the `0` sentinel meaning
    /// "all channels" (wire channel -1), or `None` for system messages.
    pub fn channel(&self) -> Option<u8> {
        match *self {
            MidiMessage::NoteOff { channel, .. }
            | MidiMessage::NoteOn { channel, .. }
            | MidiMessage::PolyPressure { channel, .. }
            | MidiMessage::ControlChange { channel, .. }
            | MidiMessage::ProgramChange { channel, .. }
            | MidiMessage::ChannelPressure { channel, .. }
            | MidiMessage::PitchBend { channel, .. } => Some(channel),
            _ => None,
        }
    }

    /// Clone with the channel replaced (channel-voice messages only; system
    /// messages are returned unchanged). Used to fan an "all channels" send out.
    pub fn with_channel(&self, ch: u8) -> MidiMessage {
        match *self {
            MidiMessage::NoteOff { note, velocity, .. } => {
                MidiMessage::NoteOff { channel: ch, note, velocity }
            }
            MidiMessage::NoteOn { note, velocity, .. } => {
                MidiMessage::NoteOn { channel: ch, note, velocity }
            }
            MidiMessage::PolyPressure { note, pressure, .. } => {
                MidiMessage::PolyPressure { channel: ch, note, pressure }
            }
            MidiMessage::ControlChange { controller, value, .. } => {
                MidiMessage::ControlChange { channel: ch, controller, value }
            }
            MidiMessage::ProgramChange { program, .. } => {
                MidiMessage::ProgramChange { channel: ch, program }
            }
            MidiMessage::ChannelPressure { pressure, .. } => {
                MidiMessage::ChannelPressure { channel: ch, pressure }
            }
            MidiMessage::PitchBend { value, .. } => {
                MidiMessage::PitchBend { channel: ch, value }
            }
            ref other => other.clone(),
        }
    }

    /// Encode to wire bytes. `channel` is clamped to 1..=16 then written 0-based.
    pub fn encode(&self) -> Vec<u8> {
        let ch = |c: u8| ((c.clamp(1, 16)) - 1) & 0x0f;
        match self {
            MidiMessage::NoteOff { channel, note, velocity } => {
                vec![0x80 | ch(*channel), d7(*note), d7(*velocity)]
            }
            MidiMessage::NoteOn { channel, note, velocity } => {
                vec![0x90 | ch(*channel), d7(*note), d7(*velocity)]
            }
            MidiMessage::PolyPressure { channel, note, pressure } => {
                vec![0xA0 | ch(*channel), d7(*note), d7(*pressure)]
            }
            MidiMessage::ControlChange { channel, controller, value } => {
                vec![0xB0 | ch(*channel), d7(*controller), d7(*value)]
            }
            MidiMessage::ProgramChange { channel, program } => {
                vec![0xC0 | ch(*channel), d7(*program)]
            }
            MidiMessage::ChannelPressure { channel, pressure } => {
                vec![0xD0 | ch(*channel), d7(*pressure)]
            }
            MidiMessage::PitchBend { channel, value } => {
                let v = value & 0x3fff;
                vec![0xE0 | ch(*channel), (v & 0x7f) as u8, (v >> 7) as u8]
            }
            MidiMessage::SysEx(bytes) => bytes.clone(),
            MidiMessage::TimeCodeQuarterFrame(d) => vec![0xF1, d7(*d)],
            MidiMessage::SongPosition(pos) => {
                let v = pos & 0x3fff;
                vec![0xF2, (v & 0x7f) as u8, (v >> 7) as u8]
            }
            MidiMessage::SongSelect(n) => vec![0xF3, d7(*n)],
            MidiMessage::TuneRequest => vec![0xF6],
            MidiMessage::Clock => vec![0xF8],
            MidiMessage::Start => vec![0xFA],
            MidiMessage::Continue => vec![0xFB],
            MidiMessage::Stop => vec![0xFC],
            MidiMessage::ActiveSensing => vec![0xFE],
            MidiMessage::Reset => vec![0xFF],
        }
    }

    /// True for system real-time clock pulses, which are handled by the BPM
    /// estimator side-channel rather than injected into the RT event ring.
    pub fn is_clock_pulse(&self) -> bool {
        matches!(self, MidiMessage::Clock)
    }

    /// The `/midi/in/*` address suffix for this message (e.g. `"note_on"`).
    /// This is the single source of truth for the message kind — both the OSC
    /// schema ([`crate::schema::encode_in`]) and the structured web seam
    /// ([`crate::wasm::midi_in_fields`]) name messages through it, so the two
    /// can't drift. (`Clock` has a name but is never emitted — it feeds the
    /// estimator instead.)
    pub fn kind(&self) -> &'static str {
        match self {
            MidiMessage::NoteOff { .. } => "note_off",
            MidiMessage::NoteOn { .. } => "note_on",
            MidiMessage::PolyPressure { .. } => "poly_pressure",
            MidiMessage::ControlChange { .. } => "control_change",
            MidiMessage::ProgramChange { .. } => "program_change",
            MidiMessage::ChannelPressure { .. } => "channel_pressure",
            MidiMessage::PitchBend { .. } => "pitch_bend",
            MidiMessage::SysEx(_) => "sysex",
            MidiMessage::TimeCodeQuarterFrame(_) => "time_code",
            MidiMessage::SongPosition(_) => "song_position",
            MidiMessage::SongSelect(_) => "song_select",
            MidiMessage::TuneRequest => "tune_request",
            MidiMessage::Clock => "clock",
            MidiMessage::Start => "start",
            MidiMessage::Continue => "continue",
            MidiMessage::Stop => "stop",
            MidiMessage::ActiveSensing => "active_sensing",
            MidiMessage::Reset => "reset",
        }
    }

    /// Write this message's numeric payload into `out` (channel first where
    /// applicable) and return how many slots were filled. These are exactly the
    /// integer args [`crate::schema::encode_in`] appends after the port string,
    /// so a structured consumer gets the same data without the OSC round-trip.
    /// Messages with no inline numbers (transport, sysex, tune request) fill 0.
    pub fn data_args(&self, out: &mut [i32; 3]) -> usize {
        match *self {
            MidiMessage::NoteOff { channel, note, velocity }
            | MidiMessage::NoteOn { channel, note, velocity } => {
                out[0] = channel as i32;
                out[1] = note as i32;
                out[2] = velocity as i32;
                3
            }
            MidiMessage::PolyPressure { channel, note, pressure } => {
                out[0] = channel as i32;
                out[1] = note as i32;
                out[2] = pressure as i32;
                3
            }
            MidiMessage::ControlChange { channel, controller, value } => {
                out[0] = channel as i32;
                out[1] = controller as i32;
                out[2] = value as i32;
                3
            }
            MidiMessage::ProgramChange { channel, program } => {
                out[0] = channel as i32;
                out[1] = program as i32;
                2
            }
            MidiMessage::ChannelPressure { channel, pressure } => {
                out[0] = channel as i32;
                out[1] = pressure as i32;
                2
            }
            MidiMessage::PitchBend { channel, value } => {
                out[0] = channel as i32;
                out[1] = value as i32;
                2
            }
            MidiMessage::SongPosition(pos) => {
                out[0] = pos as i32;
                1
            }
            MidiMessage::SongSelect(n) => {
                out[0] = n as i32;
                1
            }
            MidiMessage::TimeCodeQuarterFrame(d) => {
                out[0] = d as i32;
                1
            }
            MidiMessage::SysEx(_)
            | MidiMessage::TuneRequest
            | MidiMessage::Clock
            | MidiMessage::Start
            | MidiMessage::Continue
            | MidiMessage::Stop
            | MidiMessage::ActiveSensing
            | MidiMessage::Reset => 0,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_channel_voice() {
        assert_eq!(
            MidiMessage::parse(&[0x90, 60, 100]),
            Some(MidiMessage::NoteOn { channel: 1, note: 60, velocity: 100 })
        );
        assert_eq!(
            MidiMessage::parse(&[0x8F, 60, 0]),
            Some(MidiMessage::NoteOff { channel: 16, note: 60, velocity: 0 })
        );
        assert_eq!(
            MidiMessage::parse(&[0xB0, 7, 127]),
            Some(MidiMessage::ControlChange { channel: 1, controller: 7, value: 127 })
        );
        assert_eq!(
            MidiMessage::parse(&[0xC2, 5]),
            Some(MidiMessage::ProgramChange { channel: 3, program: 5 })
        );
        assert_eq!(
            MidiMessage::parse(&[0xA1, 64, 32]),
            Some(MidiMessage::PolyPressure { channel: 2, note: 64, pressure: 32 })
        );
        assert_eq!(
            MidiMessage::parse(&[0xD0, 50]),
            Some(MidiMessage::ChannelPressure { channel: 1, pressure: 50 })
        );
    }

    #[test]
    fn parses_pitch_bend_14bit() {
        // centre = 8192 → lsb 0, msb 64
        assert_eq!(
            MidiMessage::parse(&[0xE0, 0x00, 0x40]),
            Some(MidiMessage::PitchBend { channel: 1, value: 8192 })
        );
        // max
        assert_eq!(
            MidiMessage::parse(&[0xE0, 0x7f, 0x7f]),
            Some(MidiMessage::PitchBend { channel: 1, value: 16383 })
        );
    }

    #[test]
    fn parses_realtime_and_sysex() {
        assert_eq!(MidiMessage::parse(&[0xF8]), Some(MidiMessage::Clock));
        assert_eq!(MidiMessage::parse(&[0xFA]), Some(MidiMessage::Start));
        assert_eq!(MidiMessage::parse(&[0xFC]), Some(MidiMessage::Stop));
        assert_eq!(
            MidiMessage::parse(&[0xF0, 0x7e, 0x00, 0xF7]),
            Some(MidiMessage::SysEx(vec![0xF0, 0x7e, 0x00, 0xF7]))
        );
    }

    #[test]
    fn song_position_roundtrips() {
        // 16 sixteenth-notes = 4 quarter-note beats.
        assert_eq!(
            MidiMessage::parse(&[0xF2, 16, 0]),
            Some(MidiMessage::SongPosition(16))
        );
        let m = MidiMessage::SongPosition(12345);
        assert_eq!(MidiMessage::parse(&m.encode()), Some(m));
    }

    #[test]
    fn system_common_roundtrips() {
        for m in [
            MidiMessage::TimeCodeQuarterFrame(42),
            MidiMessage::SongSelect(7),
            MidiMessage::TuneRequest,
        ] {
            assert_eq!(MidiMessage::parse(&m.encode()), Some(m.clone()), "{m:?}");
        }
    }

    #[test]
    fn rejects_garbage() {
        assert_eq!(MidiMessage::parse(&[]), None);
        assert_eq!(MidiMessage::parse(&[0x40, 0x00]), None); // not a status byte
        assert_eq!(MidiMessage::parse(&[0x90, 60]), None); // truncated note-on
    }

    #[test]
    fn encode_roundtrips() {
        let msgs = [
            MidiMessage::NoteOn { channel: 1, note: 60, velocity: 100 },
            MidiMessage::NoteOff { channel: 16, note: 1, velocity: 0 },
            MidiMessage::ControlChange { channel: 5, controller: 74, value: 12 },
            MidiMessage::ProgramChange { channel: 8, program: 40 },
            MidiMessage::ChannelPressure { channel: 2, pressure: 99 },
            MidiMessage::PolyPressure { channel: 3, note: 50, pressure: 7 },
            MidiMessage::PitchBend { channel: 1, value: 8192 },
            MidiMessage::PitchBend { channel: 9, value: 0 },
            MidiMessage::Clock,
            MidiMessage::Start,
        ];
        for m in msgs {
            assert_eq!(MidiMessage::parse(&m.encode()), Some(m.clone()), "{m:?}");
        }
    }

    #[test]
    fn channel_clamped_on_encode() {
        // channel 0 and 17 clamp into range rather than corrupting the status nibble.
        assert_eq!(MidiMessage::NoteOn { channel: 0, note: 1, velocity: 1 }.encode()[0], 0x90);
        assert_eq!(MidiMessage::NoteOn { channel: 17, note: 1, velocity: 1 }.encode()[0], 0x9F);
    }

    #[test]
    fn note_on_velocity_zero_folds_to_note_off() {
        // The running-status release convention: a great many keyboards send
        // note-on with velocity 0 to mean note-off. Fold it at parse time so a
        // patch listening on note_off actually sees the release.
        assert_eq!(
            MidiMessage::parse(&[0x90, 60, 0]),
            Some(MidiMessage::NoteOff { channel: 1, note: 60, velocity: 0 }),
            "note-on vel 0 should fold to note_off"
        );
        // A real note-on (velocity > 0) is unaffected.
        assert_eq!(
            MidiMessage::parse(&[0x95, 60, 1]),
            Some(MidiMessage::NoteOn { channel: 6, note: 60, velocity: 1 })
        );
        // An explicit note-off (0x80) is still a note-off, carrying its velocity.
        assert_eq!(
            MidiMessage::parse(&[0x80, 60, 64]),
            Some(MidiMessage::NoteOff { channel: 1, note: 60, velocity: 64 })
        );
    }

    #[test]
    fn kind_and_data_args_match_message_shape() {
        let mut buf = [0i32; 3];

        let m = MidiMessage::NoteOn { channel: 1, note: 60, velocity: 100 };
        assert_eq!(m.kind(), "note_on");
        assert_eq!(m.data_args(&mut buf), 3);
        assert_eq!(&buf[..3], &[1, 60, 100]);

        let m = MidiMessage::ControlChange { channel: 10, controller: 74, value: 64 };
        assert_eq!(m.kind(), "control_change");
        assert_eq!(m.data_args(&mut buf), 3);
        assert_eq!(&buf[..3], &[10, 74, 64]);

        let m = MidiMessage::PitchBend { channel: 2, value: 8192 };
        assert_eq!(m.kind(), "pitch_bend");
        assert_eq!(m.data_args(&mut buf), 2);
        assert_eq!(&buf[..2], &[2, 8192]);

        let m = MidiMessage::ProgramChange { channel: 8, program: 40 };
        assert_eq!(m.kind(), "program_change");
        assert_eq!(m.data_args(&mut buf), 2);
        assert_eq!(&buf[..2], &[8, 40]);

        let m = MidiMessage::Start;
        assert_eq!(m.kind(), "start");
        assert_eq!(m.data_args(&mut buf), 0);

        // SysEx carries a blob, not numeric args — no inline data.
        let m = MidiMessage::SysEx(vec![0xF0, 1, 0xF7]);
        assert_eq!(m.kind(), "sysex");
        assert_eq!(m.data_args(&mut buf), 0);
    }
}
