//! Proves the native hot-swap [`Watcher`] actually delivers OS device-change
//! notifications — i.e. that the per-OS backend's event source is wired to the
//! coalescer and fires `on_change`.
//!
//! On macOS this specifically exercises the dedicated run-loop pump: the
//! CoreMIDI client is created on the watcher's own thread and that thread runs
//! the CFRunLoop, so delivery does NOT depend on the caller having a live run
//! loop. Creating/dropping a virtual port is the device-set change the OS
//! reports.
//!
//! Ignored by default: needs CoreMIDI/ALSA virtual ports (not Windows) and the
//! delivery is asynchronous, so it is timing-dependent and unsuitable for CI.
//! Run on a dev machine with:
//!     cargo test --test hotswap_watcher -- --ignored --nocapture
#![cfg(not(target_arch = "wasm32"))]

use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::Duration;

use midir::os::unix::VirtualOutput;
use midir::MidiOutput;

use supersonic_midi::watcher::{OnChange, Watcher};

#[test]
#[ignore = "needs CoreMIDI/ALSA virtual ports; run with --ignored on a dev box"]
fn watcher_fires_on_virtual_port_appearance() {
    let fired = Arc::new(AtomicUsize::new(0));
    let counter = fired.clone();
    let on_change: OnChange = Arc::new(move || {
        counter.fetch_add(1, Ordering::SeqCst);
    });

    let _watcher = Watcher::new(on_change);
    // Let the watcher thread create its client and attach its event source
    // before we generate the change it should observe.
    std::thread::sleep(Duration::from_millis(400));

    // Appearance of a virtual source is a device-set change the OS reports.
    let vout = MidiOutput::new("ss-hotswap-test").unwrap();
    let vconn = vout.create_virtual("ss-hotswap-probe").unwrap();

    // Coalescer waits out DEBOUNCE (200ms) then fires, with a SETTLE (450ms)
    // follow-up. Allow margin for both plus OS notification latency.
    std::thread::sleep(Duration::from_millis(1200));
    let after_add = fired.load(Ordering::SeqCst);
    assert!(
        after_add >= 1,
        "watcher should have fired on virtual port appearance, got {after_add}"
    );

    // Removal is also a change; confirm the source keeps delivering.
    drop(vconn);
    std::thread::sleep(Duration::from_millis(1200));
    let after_remove = fired.load(Ordering::SeqCst);
    assert!(
        after_remove > after_add,
        "watcher should have fired again on virtual port removal, got {after_remove} (was {after_add})"
    );
}
