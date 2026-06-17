//! Shared-client ALSA sequencer I/O.
//!
//! The default midir model opens one ALSA sequencer client (`snd_seq_open`) per
//! connected port, and — for inputs — one timestamping queue per port. ALSA caps
//! both resources system-wide (clients run out of pool memory well before the
//! nominal 192, queues at 32), so an application that opens *every* port (as
//! Sonic Pi does) can exhaust them and make later `snd_seq_open` calls from other
//! programs (e.g. `aplaymidi`) fail with "Cannot allocate memory". See
//! sonic-pi#3543.
//!
//! This module mirrors RtMidi's model instead: a single client hosts many ports.
//! [`SharedInput`] owns one client, one queue and one poll thread, dispatching by
//! the destination port of each incoming event; [`SharedOutput`] owns one client
//! and selects the source port per `send`. Both grow only ALSA *ports* (cheap),
//! not clients/queues, as more devices are opened.

use std::collections::HashMap;
use std::ffi::{CStr, CString};
use std::sync::mpsc::{channel, sync_channel, Receiver, Sender, SyncSender};
use std::sync::Arc;
use std::thread::{Builder, JoinHandle};

use alsa::seq::{Addr, EventType, PortCap, PortInfo, PortSubscribe, PortType, QueueTempo};
use alsa::{Direction, PollDescriptors, Seq};

use crate::errors::InitError;
use crate::{Ignore, MidiMessage};

use super::helpers::{self, EventDecoder, EventEncoder};
use super::PipeFd;

const INVALID_POLLFD: libc::pollfd = libc::pollfd {
    fd: -1,
    events: 0,
    revents: 0,
};

/// Dispatch callback for [`SharedInput`]: `(label_a, label_b, timestamp_us,
/// bytes)`. The two labels are opaque strings supplied per port at
/// [`SharedInput::add_port`] time and handed back verbatim on every message
/// (SuperSonic passes the normalised handle and the raw OS name).
pub type SharedInputCallback = Arc<dyn Fn(&str, &str, u64, &[u8]) + Send + Sync + 'static>;

/// One open ALSA port on a shared client: our virtual port number and the
/// subscription linking it to the device.
struct PortEntry {
    vport: i32,
    sub: PortSubscribe,
}

/// Parse a midir port id (`"client:port"`, see `MidiInputPort::id`) into an ALSA
/// address. Decouples this module from the backend port structs.
fn parse_addr(id: &str) -> Option<Addr> {
    let (c, p) = id.split_once(':')?;
    Some(Addr {
        client: c.trim().parse().ok()?,
        port: p.trim().parse().ok()?,
    })
}

/// A blocking pipe used to wake the input poll thread when a control command is
/// queued. Returns `(read_end, write_end)`.
fn wake_pipe() -> Option<(PipeFd, PipeFd)> {
    let mut fds = [-1i32, -1];
    if unsafe { libc::pipe(fds.as_mut_ptr()) } == -1 {
        None
    } else {
        Some((PipeFd(fds[0]), PipeFd(fds[1])))
    }
}

// ── Shared input ───────────────────────────────────────────────────────────

/// Control messages from the owning thread to the input poll thread. All ALSA
/// `Seq` mutation happens on the poll thread, so the `Seq` never needs to be
/// shared or locked.
enum InCmd {
    Add {
        addr: Addr,
        port_name: CString,
        label_a: String,
        label_b: String,
        reply: SyncSender<bool>,
    },
    Remove {
        label_a: String,
        reply: SyncSender<bool>,
    },
    Stop,
}

/// A single ALSA input client multiplexing many device ports onto one queue and
/// one poll thread.
pub struct SharedInput {
    cmd_tx: Sender<InCmd>,
    wake_tx: PipeFd,
    thread: Option<JoinHandle<()>>,
}

impl SharedInput {
    /// Open the client (and its single timestamping queue) and start the poll
    /// thread. No device ports are subscribed until [`add_port`](Self::add_port).
    pub fn new(
        client_name: &str,
        ignore: Ignore,
        callback: SharedInputCallback,
    ) -> Result<Self, InitError> {
        let seq = Seq::open(None, None, true).map_err(|_| InitError)?;
        let cname = CString::new(client_name).map_err(|_| InitError)?;
        seq.set_client_name(&cname).map_err(|_| InitError)?;

        let queue_id = init_queue(&seq).ok_or(InitError)?;

        let (wake_rx, wake_tx) = wake_pipe().ok_or(InitError)?;
        let (cmd_tx, cmd_rx) = channel();

        let thread = Builder::new()
            .name("midir ALSA shared input".to_string())
            .spawn(move || run_input(seq, queue_id, ignore, callback, wake_rx, cmd_rx))
            .map_err(|_| InitError)?;

        Ok(SharedInput {
            cmd_tx,
            wake_tx,
            thread: Some(thread),
        })
    }

    /// Subscribe a device port (identified by its midir id `"client:port"`) to
    /// this client, labelling it `label_a`/`label_b` for dispatch. Idempotent on
    /// `label_a`. Returns false if the port can't be opened/subscribed.
    pub fn add_port(&self, port_id: &str, port_name: &str, label_a: &str, label_b: &str) -> bool {
        let addr = match parse_addr(port_id) {
            Some(a) => a,
            None => return false,
        };
        let port_name = match CString::new(port_name) {
            Ok(c) => c,
            Err(_) => return false,
        };
        let (reply, ack) = sync_channel(0);
        let cmd = InCmd::Add {
            addr,
            port_name,
            label_a: label_a.to_string(),
            label_b: label_b.to_string(),
            reply,
        };
        if self.cmd_tx.send(cmd).is_err() {
            return false;
        }
        self.wake();
        ack.recv().unwrap_or(false)
    }

    /// Unsubscribe and delete the port previously added with this `label_a`.
    pub fn remove_port(&self, label_a: &str) -> bool {
        let (reply, ack) = sync_channel(0);
        let cmd = InCmd::Remove {
            label_a: label_a.to_string(),
            reply,
        };
        if self.cmd_tx.send(cmd).is_err() {
            return false;
        }
        self.wake();
        ack.recv().unwrap_or(false)
    }

    fn wake(&self) {
        let byte: u8 = 1;
        unsafe {
            libc::write(self.wake_tx.get(), &byte as *const u8 as *const libc::c_void, 1);
        }
    }
}

impl Drop for SharedInput {
    fn drop(&mut self) {
        let _ = self.cmd_tx.send(InCmd::Stop);
        self.wake();
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

/// Allocate the single timestamping queue shared by all input ports. Mirrors the
/// per-connection queue midir's ALSA backend normally creates.
fn init_queue(seq: &Seq) -> Option<i32> {
    let queue_id = seq.alloc_named_queue(c"midir queue").ok()?;
    let tempo = QueueTempo::empty().ok()?;
    tempo.set_tempo(600_000);
    tempo.set_ppq(240);
    seq.set_queue_tempo(queue_id, &tempo).ok()?;
    let _ = seq.drain_output();
    Some(queue_id)
}

/// The input poll thread: owns the `Seq` for its whole life, so all port
/// mutation and event reading happen here single-threaded. Returns (dropping the
/// `Seq`, which closes the client and frees its ports/queue) on `Stop`.
fn run_input(
    seq: Seq,
    queue_id: i32,
    ignore: Ignore,
    callback: SharedInputCallback,
    wake_rx: PipeFd,
    cmd_rx: Receiver<InCmd>,
) {
    let _ = seq.control_queue(queue_id, EventType::Start, 0, None);
    let _ = seq.drain_output();

    let mut by_label: HashMap<String, PortEntry> = HashMap::new();
    let mut by_vport: HashMap<i32, (String, String)> = HashMap::new();

    // poll set: the wake pipe first, then the sequencer's capture descriptors.
    // Adding/removing ports on the same client does not change these fds.
    let poll_info = (&seq, Some(Direction::Capture));
    let mut poll_fds = vec![INVALID_POLLFD; poll_info.count() + 1];
    poll_fds[0] = libc::pollfd {
        fd: wake_rx.get(),
        events: libc::POLLIN,
        revents: 0,
    };
    if poll_info.fill(&mut poll_fds[1..]).is_err() {
        return;
    }

    let mut coder = EventDecoder::new(false);
    let mut continue_sysex = false;
    let mut buffer = [0u8; 12];
    let mut message = MidiMessage::new();

    'outer: loop {
        if helpers::poll(&mut poll_fds, -1) < 0 {
            continue;
        }

        // 1) Control commands (woken via the pipe).
        if poll_fds[0].revents & libc::POLLIN != 0 {
            let mut drain = [0u8; 64];
            unsafe {
                libc::read(
                    poll_fds[0].fd,
                    drain.as_mut_ptr() as *mut libc::c_void,
                    drain.len(),
                );
            }
            while let Ok(cmd) = cmd_rx.try_recv() {
                match cmd {
                    InCmd::Stop => break 'outer,
                    InCmd::Add {
                        addr,
                        port_name,
                        label_a,
                        label_b,
                        reply,
                    } => {
                        let ok = add_input_port(
                            &seq,
                            queue_id,
                            addr,
                            &port_name,
                            &mut by_label,
                            &mut by_vport,
                            label_a,
                            label_b,
                        );
                        let _ = reply.send(ok);
                    }
                    InCmd::Remove { label_a, reply } => {
                        remove_input_port(&seq, &mut by_label, &mut by_vport, &label_a);
                        let _ = reply.send(true);
                    }
                }
            }
        }

        // 2) Drain all pending MIDI, dispatching each message by its destination
        //    port back to the right label.
        let mut input = seq.input();
        loop {
            match input.event_input_pending(true) {
                Ok(0) => break,
                Ok(_) => {}
                Err(_) => break,
            }
            if !continue_sysex {
                message.bytes.clear();
            }
            let mut ev = match input.event_input() {
                Ok(ev) => ev,
                Err(_) => break,
            };
            let dest_port = ev.get_dest().port;

            let do_decode = match ev.get_type() {
                EventType::PortSubscribed | EventType::PortUnsubscribed => false,
                EventType::Qframe | EventType::Tick | EventType::Clock => {
                    !ignore.contains(Ignore::Time)
                }
                EventType::Sensing => !ignore.contains(Ignore::ActiveSense),
                EventType::Sysex => {
                    if !ignore.contains(Ignore::Sysex) {
                        message.bytes.extend_from_slice(ev.get_ext().unwrap());
                        continue_sysex = *message.bytes.last().unwrap() != 0xF7;
                    }
                    false
                }
                _ => true,
            };
            if do_decode {
                if let Ok(nbytes) = coder.get_wrapped().decode(&mut buffer, &mut ev) {
                    if nbytes > 0 {
                        message.bytes.extend_from_slice(&buffer[0..nbytes]);
                    }
                }
            }
            if message.bytes.is_empty() || continue_sysex {
                continue;
            }

            let timestamp = match ev.get_time() {
                Some(t) => (t.as_secs() * 1_000_000) + (t.subsec_nanos() as u64 / 1_000),
                None => 0,
            };
            if let Some((label_a, label_b)) = by_vport.get(&dest_port) {
                callback(label_a, label_b, timestamp, &message.bytes);
            }
        }
    }
    // `seq` drops here: the client, its ports, subscriptions and queue go away.
}

#[allow(clippy::too_many_arguments)]
fn add_input_port(
    seq: &Seq,
    queue_id: i32,
    addr: Addr,
    port_name: &CStr,
    by_label: &mut HashMap<String, PortEntry>,
    by_vport: &mut HashMap<i32, (String, String)>,
    label_a: String,
    label_b: String,
) -> bool {
    if by_label.contains_key(&label_a) {
        return true;
    }
    if seq.get_any_port_info(addr).is_err() {
        return false;
    }
    let mut pinfo = match PortInfo::empty() {
        Ok(p) => p,
        Err(_) => return false,
    };
    pinfo.set_capability(PortCap::WRITE | PortCap::SUBS_WRITE);
    pinfo.set_type(PortType::MIDI_GENERIC | PortType::APPLICATION);
    pinfo.set_midi_channels(16);
    pinfo.set_timestamping(true);
    pinfo.set_timestamp_real(true);
    pinfo.set_timestamp_queue(queue_id);
    pinfo.set_name(port_name);
    if seq.create_port(&pinfo).is_err() {
        return false;
    }
    let vport = pinfo.get_port();

    let sub = match PortSubscribe::empty() {
        Ok(s) => s,
        Err(_) => {
            let _ = seq.delete_port(vport);
            return false;
        }
    };
    let client_id = match seq.client_id() {
        Ok(c) => c,
        Err(_) => {
            let _ = seq.delete_port(vport);
            return false;
        }
    };
    sub.set_sender(addr);
    sub.set_dest(Addr {
        client: client_id,
        port: vport,
    });
    if seq.subscribe_port(&sub).is_err() {
        let _ = seq.delete_port(vport);
        return false;
    }

    by_label.insert(label_a.clone(), PortEntry { vport, sub });
    by_vport.insert(vport, (label_a, label_b));
    true
}

fn remove_input_port(
    seq: &Seq,
    by_label: &mut HashMap<String, PortEntry>,
    by_vport: &mut HashMap<i32, (String, String)>,
    label_a: &str,
) {
    if let Some(entry) = by_label.remove(label_a) {
        by_vport.remove(&entry.vport);
        let _ = seq.unsubscribe_port(entry.sub.get_sender(), entry.sub.get_dest());
        let _ = seq.delete_port(entry.vport);
    }
}

// ── Shared output ────────────────────────────────────────────────────────────

/// A single ALSA output client multiplexing many device ports. Accessed only
/// behind the engine's `MidiIo` mutex, so it needs no internal locking.
pub struct SharedOutput {
    seq: Seq,
    coder: EventEncoder,
    ports: HashMap<String, PortEntry>,
}

impl SharedOutput {
    /// Open the output client. No device ports are subscribed until
    /// [`add_port`](Self::add_port).
    pub fn new(client_name: &str) -> Result<Self, InitError> {
        let seq = Seq::open(None, Some(Direction::Playback), true).map_err(|_| InitError)?;
        let cname = CString::new(client_name).map_err(|_| InitError)?;
        seq.set_client_name(&cname).map_err(|_| InitError)?;
        Ok(SharedOutput {
            seq,
            coder: EventEncoder::new(super::INITIAL_CODER_BUFFER_SIZE as u32),
            ports: HashMap::new(),
        })
    }

    /// Subscribe a device output port (midir id `"client:port"`) to this client
    /// under `label`. Idempotent on `label`.
    pub fn add_port(&mut self, port_id: &str, port_name: &str, label: &str) -> bool {
        if self.ports.contains_key(label) {
            return true;
        }
        let addr = match parse_addr(port_id) {
            Some(a) => a,
            None => return false,
        };
        let port_name = match CString::new(port_name) {
            Ok(c) => c,
            Err(_) => return false,
        };
        let dest = match self.seq.get_any_port_info(addr) {
            Ok(p) => p.addr(),
            Err(_) => return false,
        };
        let vport = match self.seq.create_simple_port(
            &port_name,
            PortCap::READ | PortCap::SUBS_READ,
            PortType::MIDI_GENERIC | PortType::APPLICATION,
        ) {
            Ok(v) => v,
            Err(_) => return false,
        };
        let client_id = match self.seq.client_id() {
            Ok(c) => c,
            Err(_) => {
                let _ = self.seq.delete_port(vport);
                return false;
            }
        };
        let sub = match PortSubscribe::empty() {
            Ok(s) => s,
            Err(_) => {
                let _ = self.seq.delete_port(vport);
                return false;
            }
        };
        sub.set_sender(Addr {
            client: client_id,
            port: vport,
        });
        sub.set_dest(dest);
        sub.set_time_update(true);
        sub.set_time_real(true);
        if self.seq.subscribe_port(&sub).is_err() {
            let _ = self.seq.delete_port(vport);
            return false;
        }
        self.ports.insert(label.to_string(), PortEntry { vport, sub });
        true
    }

    /// Unsubscribe and delete the port previously added under `label`.
    pub fn remove_port(&mut self, label: &str) {
        if let Some(entry) = self.ports.remove(label) {
            let _ = self
                .seq
                .unsubscribe_port(entry.sub.get_sender(), entry.sub.get_dest());
            let _ = self.seq.delete_port(entry.vport);
        }
    }

    /// True if a port is currently open under `label`.
    pub fn contains(&self, label: &str) -> bool {
        self.ports.contains_key(label)
    }

    /// Labels of all currently open output ports.
    pub fn labels(&self) -> Vec<String> {
        self.ports.keys().cloned().collect()
    }

    /// Send raw MIDI bytes to the port open under `label`. No-op (returns false)
    /// if no such port is open or the message can't be encoded/sent.
    pub fn send(&mut self, label: &str, message: &[u8]) -> bool {
        let vport = match self.ports.get(label) {
            Some(e) => e.vport,
            None => return false,
        };
        let nbytes = message.len();
        if nbytes > self.coder.get_buffer_size() as usize
            && self.coder.resize_buffer(nbytes as u32).is_err()
        {
            return false;
        }
        let mut ev = match self.coder.get_wrapped().encode(message) {
            Ok((_, Some(ev))) => ev,
            _ => return false,
        };
        ev.set_source(vport);
        ev.set_subs();
        ev.set_direct();
        if self.seq.event_output_direct(&mut ev).is_err() {
            return false;
        }
        let _ = self.seq.drain_output();
        true
    }
}
