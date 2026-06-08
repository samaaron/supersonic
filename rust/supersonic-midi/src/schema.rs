//! The `/midi/*` OSC schema — the single API surface, identical on native and
//! web. Inbound hardware MIDI is encoded to `/midi/in/*`; client/VM requests
//! arrive as `/midi/out/*`, `/midi/clock/*` and device-management verbs and are
//! decoded to an [`OutCommand`].
//!
//! Channels are 1-based (1..=16); `channel == -1` on the wire means "all 16",
//! and `port == "*"` means "all enabled ports". Pitch bend is 14-bit (0..=16383).

use crate::message::MidiMessage;
use crate::osc::{self, OscArg};

/// A decoded client/VM request.
#[derive(Clone, Debug, PartialEq)]
pub enum OutCommand {
    /// Send a parsed channel-voice message to `port` (`"*"` = all).
    Send { port: String, msg: MidiMessage },
    /// Send arbitrary/raw or sysex bytes to `port`.
    SendRaw { port: String, bytes: Vec<u8> },
    /// One immediate clock pulse. Continuous clock + transport + beat-bursts are
    /// generated engine-side (MidiClockOut); generated pulses also arrive here.
    ClockTick { port: String },
    /// Enable/disable using incoming MIDI clock on `port` as the SuperClock tempo source.
    ClockSync { port: String, enabled: bool },
    /// Open/close a specific port (`input` selects the in vs out list).
    Enable { port: String, input: bool, enabled: bool },
    PortsList,
    Refresh,
    Subscribe,
    Unsubscribe,
}

/// `channel == -1` expands to all channels for outbound messages.
pub const ALL_CHANNELS: i32 = -1;

fn port(args: &[OscArg]) -> Option<String> {
    args.first()?.as_str().map(|s| s.to_string())
}
fn i_at(args: &[OscArg], i: usize) -> Option<i32> {
    args.get(i)?.as_i32()
}

/// Channel arg for a channel-voice message: `-1` ([`ALL_CHANNELS`]) maps to the
/// `0` sentinel (fanned out to all 16 by the send path); 1..=16 pass through.
fn ch_at(args: &[OscArg], i: usize) -> Option<u8> {
    Some(match i_at(args, i)? {
        ALL_CHANNELS => 0,
        c => c as u8,
    })
}

/// Decode an outbound `/midi/*` OSC message into an [`OutCommand`].
/// Returns `None` for non-`/midi/*` addresses or malformed args.
pub fn decode_out(data: &[u8]) -> Option<OutCommand> {
    let m = osc::decode(data)?;
    let a = &m.args;
    let p = || port(a);

    match m.addr.as_str() {
        "/midi/out/note_on" => Some(OutCommand::Send {
            port: p()?,
            msg: MidiMessage::NoteOn {
                channel: ch_at(a, 1)?,
                note: i_at(a, 2)? as u8,
                velocity: i_at(a, 3)? as u8,
            },
        }),
        "/midi/out/note_off" => Some(OutCommand::Send {
            port: p()?,
            msg: MidiMessage::NoteOff {
                channel: ch_at(a, 1)?,
                note: i_at(a, 2)? as u8,
                velocity: i_at(a, 3)? as u8,
            },
        }),
        "/midi/out/control_change" => Some(OutCommand::Send {
            port: p()?,
            msg: MidiMessage::ControlChange {
                channel: ch_at(a, 1)?,
                controller: i_at(a, 2)? as u8,
                value: i_at(a, 3)? as u8,
            },
        }),
        "/midi/out/program_change" => Some(OutCommand::Send {
            port: p()?,
            msg: MidiMessage::ProgramChange {
                channel: ch_at(a, 1)?,
                program: i_at(a, 2)? as u8,
            },
        }),
        "/midi/out/channel_pressure" => Some(OutCommand::Send {
            port: p()?,
            msg: MidiMessage::ChannelPressure {
                channel: ch_at(a, 1)?,
                pressure: i_at(a, 2)? as u8,
            },
        }),
        "/midi/out/poly_pressure" => Some(OutCommand::Send {
            port: p()?,
            msg: MidiMessage::PolyPressure {
                channel: ch_at(a, 1)?,
                note: i_at(a, 2)? as u8,
                pressure: i_at(a, 3)? as u8,
            },
        }),
        "/midi/out/pitch_bend" => Some(OutCommand::Send {
            port: p()?,
            msg: MidiMessage::PitchBend {
                channel: ch_at(a, 1)?,
                value: (i_at(a, 2)?.clamp(0, 16383)) as u16,
            },
        }),
        // Single-byte system real-time sends (Sonic Pi's midi_clock_tick /
        // midi_start / midi_stop / midi_continue): port only.
        "/midi/out/clock" => Some(OutCommand::Send { port: p()?, msg: MidiMessage::Clock }),
        "/midi/out/start" => Some(OutCommand::Send { port: p()?, msg: MidiMessage::Start }),
        "/midi/out/continue" => Some(OutCommand::Send { port: p()?, msg: MidiMessage::Continue }),
        "/midi/out/stop" => Some(OutCommand::Send { port: p()?, msg: MidiMessage::Stop }),

        // raw/sysex: port followed by either int bytes or a single blob.
        "/midi/out/raw" | "/midi/out/sysex" => {
            let bytes = if let Some(OscArg::Blob(b)) = a.get(1) {
                b.clone()
            } else {
                a[1..]
                    .iter()
                    .filter_map(|x| x.as_i32())
                    .map(|v| (v & 0xff) as u8)
                    .collect()
            };
            Some(OutCommand::SendRaw { port: p()?, bytes })
        }

        // Continuous clock (start/stop/continue) + beat-bursts are handled
        // engine-side by MidiClockOut; the subsystem only sends single pulses.
        "/midi/clock/tick" => Some(OutCommand::ClockTick { port: p()? }),
        "/midi/clock/sync" => Some(OutCommand::ClockSync {
            port: p()?,
            enabled: i_at(a, 1)? != 0,
        }),

        "/midi/in/enable" => Some(OutCommand::Enable {
            port: p()?,
            input: true,
            enabled: i_at(a, 1)? != 0,
        }),
        "/midi/out/enable" => Some(OutCommand::Enable {
            port: p()?,
            input: false,
            enabled: i_at(a, 1)? != 0,
        }),

        "/midi/ports/list" | "/midi/ports/get" => Some(OutCommand::PortsList),
        "/midi/refresh" => Some(OutCommand::Refresh),
        "/midi/notify/subscribe" => Some(OutCommand::Subscribe),
        "/midi/notify/unsubscribe" => Some(OutCommand::Unsubscribe),
        _ => None,
    }
}

/// Encode an inbound hardware message as a `/midi/in/*` OSC packet for injection
/// into the engine ingress / broadcast to subscribers. Returns `None` for clock
/// pulses (`0xF8`), which are consumed by the BPM estimator instead.
pub fn encode_in(port: &str, msg: &MidiMessage) -> Option<Vec<u8>> {
    let s = |v: &str| OscArg::Str(v.to_string());
    let i = |v: i32| OscArg::Int(v);
    let (addr, args): (&str, Vec<OscArg>) = match msg {
        MidiMessage::NoteOn { channel, note, velocity } => (
            "/midi/in/note_on",
            vec![s(port), i(*channel as i32), i(*note as i32), i(*velocity as i32)],
        ),
        MidiMessage::NoteOff { channel, note, velocity } => (
            "/midi/in/note_off",
            vec![s(port), i(*channel as i32), i(*note as i32), i(*velocity as i32)],
        ),
        MidiMessage::ControlChange { channel, controller, value } => (
            "/midi/in/control_change",
            vec![s(port), i(*channel as i32), i(*controller as i32), i(*value as i32)],
        ),
        MidiMessage::ProgramChange { channel, program } => (
            "/midi/in/program_change",
            vec![s(port), i(*channel as i32), i(*program as i32)],
        ),
        MidiMessage::ChannelPressure { channel, pressure } => (
            "/midi/in/channel_pressure",
            vec![s(port), i(*channel as i32), i(*pressure as i32)],
        ),
        MidiMessage::PolyPressure { channel, note, pressure } => (
            "/midi/in/poly_pressure",
            vec![s(port), i(*channel as i32), i(*note as i32), i(*pressure as i32)],
        ),
        MidiMessage::PitchBend { channel, value } => (
            "/midi/in/pitch_bend",
            vec![s(port), i(*channel as i32), i(*value as i32)],
        ),
        MidiMessage::SysEx(bytes) => (
            "/midi/in/sysex",
            vec![s(port), OscArg::Blob(bytes.clone())],
        ),
        MidiMessage::SongPosition(pos) => (
            "/midi/in/song_position",
            vec![s(port), i(*pos as i32)],
        ),
        MidiMessage::TimeCodeQuarterFrame(d) => (
            "/midi/in/time_code",
            vec![s(port), i(*d as i32)],
        ),
        MidiMessage::SongSelect(n) => ("/midi/in/song_select", vec![s(port), i(*n as i32)]),
        MidiMessage::TuneRequest => ("/midi/in/tune_request", vec![s(port)]),
        MidiMessage::Start => ("/midi/in/start", vec![s(port)]),
        MidiMessage::Continue => ("/midi/in/continue", vec![s(port)]),
        MidiMessage::Stop => ("/midi/in/stop", vec![s(port)]),
        MidiMessage::ActiveSensing => ("/midi/in/active_sensing", vec![s(port)]),
        MidiMessage::Reset => ("/midi/in/reset", vec![s(port)]),
        MidiMessage::Clock => return None, // estimator side-channel
    };
    Some(osc::encode(addr, &args))
}

/// `/midi/in/clock_bpm <port:s> <bpm:f>` — the distilled tempo from the estimator.
pub fn encode_clock_bpm(port: &str, bpm: f64) -> Vec<u8> {
    osc::encode(
        "/midi/in/clock_bpm",
        &[OscArg::Str(port.to_string()), OscArg::Float(bpm as f32)],
    )
}

fn ports_args(ins: &[(String, bool)], outs: &[(String, bool)]) -> Vec<OscArg> {
    let mut args = Vec::with_capacity(2 + (ins.len() + outs.len()) * 2);
    args.push(OscArg::Int(ins.len() as i32));
    for (name, enabled) in ins {
        args.push(OscArg::Str(name.clone()));
        args.push(OscArg::Int(*enabled as i32));
    }
    args.push(OscArg::Int(outs.len() as i32));
    for (name, enabled) in outs {
        args.push(OscArg::Str(name.clone()));
        args.push(OscArg::Int(*enabled as i32));
    }
    args
}

/// `/midi/ports.reply <nIn:i> [name:s enabled:i]* <nOut:i> [name:s enabled:i]*`
/// — the RPC reply to `/midi/ports/list`, sent to the caller.
pub fn encode_ports_reply(ins: &[(String, bool)], outs: &[(String, bool)]) -> Vec<u8> {
    osc::encode("/midi/ports.reply", &ports_args(ins, outs))
}

/// `/midi/ports …` — same payload, broadcast to subscribers on a hotplug change.
pub fn encode_ports(ins: &[(String, bool)], outs: &[(String, bool)]) -> Vec<u8> {
    osc::encode("/midi/ports", &ports_args(ins, outs))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::osc;

    #[test]
    fn decodes_note_on_out() {
        let bytes = osc::encode(
            "/midi/out/note_on",
            &[
                OscArg::Str("kbd".into()),
                OscArg::Int(1),
                OscArg::Int(60),
                OscArg::Int(100),
            ],
        );
        assert_eq!(
            decode_out(&bytes),
            Some(OutCommand::Send {
                port: "kbd".into(),
                msg: MidiMessage::NoteOn { channel: 1, note: 60, velocity: 100 },
            })
        );
    }

    #[test]
    fn decodes_clock_and_management_verbs() {
        let tick = osc::encode("/midi/clock/tick", &[OscArg::Str("synth".into())]);
        assert_eq!(
            decode_out(&tick),
            Some(OutCommand::ClockTick { port: "synth".into() })
        );

        let sync = osc::encode(
            "/midi/clock/sync",
            &[OscArg::Str("in".into()), OscArg::Int(1)],
        );
        assert_eq!(
            decode_out(&sync),
            Some(OutCommand::ClockSync { port: "in".into(), enabled: true })
        );

        let en = osc::encode(
            "/midi/in/enable",
            &[OscArg::Str("*".into()), OscArg::Int(0)],
        );
        assert_eq!(
            decode_out(&en),
            Some(OutCommand::Enable { port: "*".into(), input: true, enabled: false })
        );

        assert_eq!(
            decode_out(&osc::encode("/midi/refresh", &[])),
            Some(OutCommand::Refresh)
        );
    }

    #[test]
    fn decodes_single_byte_realtime_out() {
        for (addr, msg) in [
            ("/midi/out/clock", MidiMessage::Clock),
            ("/midi/out/start", MidiMessage::Start),
            ("/midi/out/continue", MidiMessage::Continue),
            ("/midi/out/stop", MidiMessage::Stop),
        ] {
            let bytes = osc::encode(addr, &[OscArg::Str("p".into())]);
            assert_eq!(
                decode_out(&bytes),
                Some(OutCommand::Send { port: "p".into(), msg })
            );
        }
    }

    #[test]
    fn decodes_raw_from_ints() {
        let bytes = osc::encode(
            "/midi/out/raw",
            &[OscArg::Str("p".into()), OscArg::Int(0x90), OscArg::Int(60), OscArg::Int(64)],
        );
        assert_eq!(
            decode_out(&bytes),
            Some(OutCommand::SendRaw { port: "p".into(), bytes: vec![0x90, 60, 64] })
        );
    }

    #[test]
    fn encodes_inbound_events() {
        let bytes = encode_in("kbd", &MidiMessage::NoteOn { channel: 2, note: 64, velocity: 99 })
            .unwrap();
        let m = osc::decode(&bytes).unwrap();
        assert_eq!(m.addr, "/midi/in/note_on");
        assert_eq!(m.args[0], OscArg::Str("kbd".into()));
        assert_eq!(m.args[1], OscArg::Int(2));
        // clock pulses are not encoded as events
        assert!(encode_in("kbd", &MidiMessage::Clock).is_none());
    }

    #[test]
    fn kind_and_data_args_agree_with_encoded_osc() {
        // The structured web seam (MidiMessage::kind / data_args) must produce
        // exactly the address suffix + integer args that encode_in emits, so the
        // OSC and structured paths can never diverge.
        let cases = [
            MidiMessage::NoteOn { channel: 2, note: 64, velocity: 99 },
            MidiMessage::NoteOff { channel: 1, note: 60, velocity: 0 },
            MidiMessage::ControlChange { channel: 10, controller: 74, value: 64 },
            MidiMessage::ProgramChange { channel: 8, program: 40 },
            MidiMessage::ChannelPressure { channel: 2, pressure: 99 },
            MidiMessage::PolyPressure { channel: 3, note: 50, pressure: 7 },
            MidiMessage::PitchBend { channel: 1, value: 8192 },
            MidiMessage::SongPosition(12345),
            MidiMessage::SongSelect(7),
            MidiMessage::Start,
            MidiMessage::Stop,
        ];
        for m in cases {
            let bytes = encode_in("kbd", &m).unwrap();
            let decoded = osc::decode(&bytes).unwrap();
            assert_eq!(decoded.addr, format!("/midi/in/{}", m.kind()), "addr for {m:?}");
            assert_eq!(decoded.args[0], OscArg::Str("kbd".into()), "port for {m:?}");
            // Int args after the port must match data_args, in order.
            let osc_ints: Vec<i32> =
                decoded.args[1..].iter().filter_map(|a| a.as_i32()).collect();
            let mut buf = [0i32; 3];
            let n = m.data_args(&mut buf);
            assert_eq!(osc_ints, buf[..n].to_vec(), "data_args for {m:?}");
        }
    }

    #[test]
    fn ports_reply_roundtrips() {
        let bytes = encode_ports_reply(
            &[("a".into(), true), ("b".into(), false)],
            &[("c".into(), true)],
        );
        let m = osc::decode(&bytes).unwrap();
        assert_eq!(m.addr, "/midi/ports.reply");
        assert_eq!(m.args[0], OscArg::Int(2));
        assert_eq!(m.args[1], OscArg::Str("a".into()));
        assert_eq!(m.args[5], OscArg::Int(1)); // nOut
    }
}
