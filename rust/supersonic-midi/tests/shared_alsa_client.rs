//! Linux/ALSA resource-usage contract for `device::MidiIo`.
//!
//! Regression test for sonic-pi#3543: the old midir path opened *one ALSA
//! sequencer client per port* (and one queue per input port), so enabling every
//! port at boot exhausted the kernel's seq client/queue tables and any later
//! `snd_seq_open` (e.g. `aplaymidi`) failed with "Cannot allocate memory". The
//! fix shares a single client across all inputs and a single client across all
//! outputs, so the count is constant regardless of how many ports are open.
//!
//! Ignored by default: needs a real ALSA sequencer and creates virtual ports, so
//! it is timing-dependent and unsuitable for CI. Run on a Linux dev box with:
//!     cargo test --test shared_alsa_client -- --ignored
#![cfg(all(target_os = "linux", not(target_arch = "wasm32")))]

use std::collections::HashSet;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use midir::os::unix::{VirtualInput, VirtualOutput};
use midir::{MidiInput, MidiOutput};

use supersonic_midi::device::MidiIo;
use supersonic_midi::message::MidiMessage;

/// IDs of all seq clients whose advertised name is exactly `name`.
fn client_ids_named(name: &str) -> Vec<i32> {
    let text = std::fs::read_to_string("/proc/asound/seq/clients").unwrap_or_default();
    let needle = format!("\"{name}\"");
    text.lines()
        .filter_map(|l| {
            let l = l.trim_start();
            let rest = l.strip_prefix("Client ")?;
            if !l.contains(&needle) {
                return None;
            }
            // "Client  128 : \"name\" [..]" -> parse 128
            rest.split(':').next()?.trim().parse::<i32>().ok()
        })
        .collect()
}

/// Number of seq queues owned by any client in `ids`.
fn queues_owned_by(ids: &HashSet<i32>) -> usize {
    let text = std::fs::read_to_string("/proc/asound/seq/queues").unwrap_or_default();
    text.lines()
        .filter_map(|l| {
            let l = l.trim();
            let rest = l.strip_prefix("owned by client")?;
            rest.trim_start_matches([' ', ':']).trim().parse::<i32>().ok()
        })
        .filter(|id| ids.contains(id))
        .count()
}

/// Normalised names of currently-open input/output ports whose label contains
/// `tag` (our unique virtual-port marker).
fn open_matching(io: &MidiIo, tag: &str) -> (Vec<String>, Vec<String>) {
    let (ins, outs) = io.port_lists();
    let pick = |v: Vec<(String, bool)>| {
        v.into_iter()
            .filter(|(n, open)| *open && n.contains(tag))
            .map(|(n, _)| n)
            .collect::<Vec<_>>()
    };
    (pick(ins), pick(outs))
}

#[test]
#[ignore = "needs a real ALSA sequencer; run with --ignored on a Linux dev box"]
fn one_client_per_side_regardless_of_port_count() {
    let tag = "ss3543";
    let client_name = "SuperSonic-shared-3543";

    // Three virtual *sources* (enumerated by MidiIo as inputs) and three virtual
    // *destinations* (enumerated as outputs). Held alive for the whole test.
    let mut src_conns = Vec::new();
    for i in 0..3 {
        let out = MidiOutput::new(&format!("{tag}-src-cli-{i}")).unwrap();
        src_conns.push(out.create_virtual(&format!("{tag}-src-{i}")).unwrap());
    }
    let recv_dst: Arc<Mutex<Vec<Vec<u8>>>> = Arc::new(Mutex::new(Vec::new()));
    let mut dst_conns = Vec::new();
    for i in 0..3 {
        let sink = recv_dst.clone();
        let inp = MidiInput::new(&format!("{tag}-dst-cli-{i}")).unwrap();
        dst_conns.push(
            inp.create_virtual(
                &format!("{tag}-dst-{i}"),
                move |_ts, bytes, _| sink.lock().unwrap().push(bytes.to_vec()),
                (),
            )
            .unwrap(),
        );
    }

    let got: Arc<Mutex<Vec<(String, Vec<u8>)>>> = Arc::new(Mutex::new(Vec::new()));
    let sink = got.clone();
    let mut io = MidiIo::new(
        client_name,
        Arc::new(move |port: &str, _raw: &str, _ts, bytes: &[u8]| {
            sink.lock().unwrap().push((port.to_string(), bytes.to_vec()));
        }),
    );
    io.refresh();
    // Open everything, exactly as midi_api.rb does at boot.
    io.enable_all(true, true);
    io.enable_all(false, true);
    std::thread::sleep(Duration::from_millis(200));

    // We must actually have opened several ports for the test to be meaningful.
    let (open_in, open_out) = open_matching(&io, tag);
    assert!(
        open_in.len() >= 3,
        "expected >=3 virtual inputs open, got {open_in:?}"
    );
    assert!(
        open_out.len() >= 3,
        "expected >=3 virtual outputs open, got {open_out:?}"
    );

    // The contract: at most one client per side (in + out), not one per port.
    let ids = client_ids_named(client_name);
    assert!(
        ids.len() <= 2,
        "expected <=2 ALSA seq clients for {client_name:?} (one in + one out), \
         found {} ({ids:?}) — one client per port regressed",
        ids.len()
    );

    // And at most one input queue total, not one per input port.
    let idset: HashSet<i32> = ids.iter().copied().collect();
    let qn = queues_owned_by(&idset);
    assert!(
        qn <= 1,
        "expected <=1 ALSA queue owned by our clients, found {qn} — one queue per input port regressed"
    );
}

#[test]
#[ignore = "needs a real ALSA sequencer; run with --ignored on a Linux dev box"]
fn input_from_multiple_ports_routes_with_correct_labels() {
    let tag = "ss3543b";
    let client_name = "SuperSonic-shared-3543b";

    // Lowercase markers: normalisation lowercases handles (see safe_osc_name).
    let out_a = MidiOutput::new(&format!("{tag}-a-cli")).unwrap();
    let mut conn_a = out_a.create_virtual(&format!("{tag}-a")).unwrap();
    let out_b = MidiOutput::new(&format!("{tag}-b-cli")).unwrap();
    let mut conn_b = out_b.create_virtual(&format!("{tag}-b")).unwrap();

    let got: Arc<Mutex<Vec<(String, Vec<u8>)>>> = Arc::new(Mutex::new(Vec::new()));
    let sink = got.clone();
    let mut io = MidiIo::new(
        client_name,
        Arc::new(move |port: &str, _raw: &str, _ts, bytes: &[u8]| {
            sink.lock().unwrap().push((port.to_string(), bytes.to_vec()));
        }),
    );
    io.refresh();
    io.enable_all(true, true);
    std::thread::sleep(Duration::from_millis(100));

    let (open_in, _) = open_matching(&io, tag);
    let name_a = open_in.iter().find(|n| n.contains(&format!("{tag}-a"))).cloned();
    let name_b = open_in.iter().find(|n| n.contains(&format!("{tag}-b"))).cloned();
    let (name_a, name_b) = (
        name_a.expect("virtual input A enumerated+open"),
        name_b.expect("virtual input B enumerated+open"),
    );

    let msg_a = MidiMessage::NoteOn { channel: 1, note: 60, velocity: 100 };
    let msg_b = MidiMessage::ControlChange { channel: 2, controller: 7, value: 42 };
    conn_a.send(&msg_a.encode()).unwrap();
    conn_b.send(&msg_b.encode()).unwrap();
    std::thread::sleep(Duration::from_millis(200));

    let got = got.lock().unwrap();
    assert!(
        got.iter()
            .any(|(p, b)| *p == name_a && MidiMessage::parse(b) == Some(msg_a.clone())),
        "note-on should arrive labelled {name_a:?}, got {got:?}"
    );
    assert!(
        got.iter()
            .any(|(p, b)| *p == name_b && MidiMessage::parse(b) == Some(msg_b.clone())),
        "control-change should arrive labelled {name_b:?}, got {got:?}"
    );
}
