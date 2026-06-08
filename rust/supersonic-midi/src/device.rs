//! midir-backed device IO: enumerate, open/close, send, and per-port input
//! callbacks. The same midir API serves native (CoreMIDI/ALSA/WinMM) and, later,
//! web (Web MIDI), so this module is platform-agnostic; threading and the FFI
//! shell live in the platform-specific [`crate::ffi`] layer.
//!
//! Ports are addressed by their normalized handle (see [`crate::normalize`]).
//! Devices are opened explicitly via `enable_*`.

use std::collections::{HashMap, HashSet};
use std::sync::Arc;

use midir::{Ignore, MidiInput, MidiInputConnection, MidiOutput, MidiOutputConnection};

use crate::normalize::{normalize_ports, PortInfo};

/// Invoked for each inbound message on midir's input thread:
/// `(normalized_port, timestamp_us, raw_bytes)`.
pub type InputCallback = Arc<dyn Fn(&str, u64, &[u8]) + Send + Sync + 'static>;

/// Owns all open MIDI connections plus the last-enumerated port lists.
pub struct MidiIo {
    client_name: String,
    on_input: InputCallback,
    inputs: HashMap<String, MidiInputConnection<()>>,
    outputs: HashMap<String, MidiOutputConnection>,
    in_ports: Vec<PortInfo>,
    out_ports: Vec<PortInfo>,
    // "Open everything" intent (set by enable_all). When set, refresh() re-opens
    // any newly-appeared ports so a re-plugged device resumes I/O on its own.
    want_all_in: bool,
    want_all_out: bool,
}

impl MidiIo {
    pub fn new(client_name: impl Into<String>, on_input: InputCallback) -> Self {
        let mut io = Self {
            client_name: client_name.into(),
            on_input,
            inputs: HashMap::new(),
            outputs: HashMap::new(),
            in_ports: Vec::new(),
            out_ports: Vec::new(),
            want_all_in: false,
            want_all_out: false,
        };
        io.refresh();
        io
    }

    /// Re-enumerate available ports (call on hotplug / refresh). Connections to
    /// vanished ports are dropped (closing the dead handle so a re-plug of the
    /// same name reconnects instead of being treated as already-open), and if an
    /// "open all" intent is set, newly-appeared ports are reopened.
    pub fn refresh(&mut self) {
        if let Ok(mi) = MidiInput::new(&self.client_name) {
            self.in_ports = normalize_ports(&port_names_in(&mi));
        }
        if let Ok(mo) = MidiOutput::new(&self.client_name) {
            self.out_ports = normalize_ports(&port_names_out(&mo));
        }
        {
            let live_in: HashSet<&str> =
                self.in_ports.iter().map(|p| p.normalized.as_str()).collect();
            self.inputs.retain(|k, _| live_in.contains(k.as_str()));
            let live_out: HashSet<&str> =
                self.out_ports.iter().map(|p| p.normalized.as_str()).collect();
            self.outputs.retain(|k, _| live_out.contains(k.as_str()));
        }
        if self.want_all_in {
            self.enable_all(true, true);
        }
        if self.want_all_out {
            self.enable_all(false, true);
        }
    }

    /// (inputs, outputs) as `(normalized_name, is_open)` pairs.
    pub fn port_lists(&self) -> (Vec<(String, bool)>, Vec<(String, bool)>) {
        let ins = self
            .in_ports
            .iter()
            .map(|p| (p.normalized.clone(), self.inputs.contains_key(&p.normalized)))
            .collect();
        let outs = self
            .out_ports
            .iter()
            .map(|p| (p.normalized.clone(), self.outputs.contains_key(&p.normalized)))
            .collect();
        (ins, outs)
    }

    /// Open (or close) an input port by normalized name. Returns true on success
    /// (or if already in the requested state).
    pub fn enable_input(&mut self, norm: &str, enable: bool) -> bool {
        if !enable {
            self.inputs.remove(norm); // dropping the connection closes the port
            return true;
        }
        if self.inputs.contains_key(norm) {
            return true;
        }
        let mut midi_in = match MidiInput::new(&self.client_name) {
            Ok(m) => m,
            Err(_) => return false,
        };
        midi_in.ignore(Ignore::None); // we want sysex, timing and active-sensing
        let ports = midi_in.ports();
        let infos = normalize_ports(&port_names_in(&midi_in));
        let idx = match infos.iter().position(|pi| pi.normalized == norm) {
            Some(i) => i,
            None => return false,
        };
        let cb = self.on_input.clone();
        let name = norm.to_string();
        match midi_in.connect(
            &ports[idx],
            "supersonic-midi-in",
            move |ts, bytes, _| cb(&name, ts, bytes),
            (),
        ) {
            Ok(conn) => {
                self.inputs.insert(norm.to_string(), conn);
                true
            }
            Err(_) => false,
        }
    }

    /// Open (or close) an output port by normalized name.
    pub fn enable_output(&mut self, norm: &str, enable: bool) -> bool {
        if !enable {
            self.outputs.remove(norm); // dropping the connection closes the port
            return true;
        }
        if self.outputs.contains_key(norm) {
            return true;
        }
        let midi_out = match MidiOutput::new(&self.client_name) {
            Ok(m) => m,
            Err(_) => return false,
        };
        let ports = midi_out.ports();
        let infos = normalize_ports(&port_names_out(&midi_out));
        let idx = match infos.iter().position(|pi| pi.normalized == norm) {
            Some(i) => i,
            None => return false,
        };
        match midi_out.connect(&ports[idx], "supersonic-midi-out") {
            Ok(conn) => {
                self.outputs.insert(norm.to_string(), conn);
                true
            }
            Err(_) => false,
        }
    }

    /// Enable (or disable) every available input/output port, and remember the
    /// intent so refresh() can reopen ports that appear later (e.g. on re-plug).
    pub fn enable_all(&mut self, input: bool, enable: bool) {
        if input {
            self.want_all_in = enable;
        } else {
            self.want_all_out = enable;
        }
        let names: Vec<String> = if input {
            self.in_ports.iter().map(|p| p.normalized.clone()).collect()
        } else {
            self.out_ports.iter().map(|p| p.normalized.clone()).collect()
        };
        for n in names {
            if input {
                self.enable_input(&n, enable);
            } else {
                self.enable_output(&n, enable);
            }
        }
    }

    /// Send raw bytes to an open output (`"*"` = all open outputs).
    pub fn send(&mut self, port: &str, bytes: &[u8]) {
        if port == "*" {
            for conn in self.outputs.values_mut() {
                let _ = conn.send(bytes);
            }
        } else if let Some(conn) = self.outputs.get_mut(port) {
            let _ = conn.send(bytes);
        }
    }

    /// Names of currently open output ports (used to fan out generated clock).
    pub fn open_outputs(&self) -> Vec<String> {
        self.outputs.keys().cloned().collect()
    }
}

fn port_names_in(mi: &MidiInput) -> Vec<String> {
    mi.ports()
        .iter()
        .map(|p| mi.port_name(p).unwrap_or_default())
        .collect()
}

fn port_names_out(mo: &MidiOutput) -> Vec<String> {
    mo.ports()
        .iter()
        .map(|p| mo.port_name(p).unwrap_or_default())
        .collect()
}
