//! Native MIDI device hot-swap detection.
//!
//! midir is enumeration-only on every backend — it can tell you *what* devices
//! exist when asked, but never *that* the set just changed. This module supplies
//! the missing edge: a platform-native device-arrival/removal notification that
//! pokes a re-enumerate. It is deliberately ignorant of the rest of the MIDI
//! subsystem — it just calls the [`OnChange`] closure the caller supplied, which
//! (in [`crate::ffi`]) re-enumerates via midir, diffs, and broadcasts
//! `/midi/ports`. No JUCE, no audio thread, no polling.
//!
//! Each backend feeds raw OS events into a shared [`Coalescer`], which collapses
//! the burst a single hot-plug produces into one `OnChange` call after a short
//! quiet period, then fires a second "settle" call a beat later to beat the
//! race where the OS announces a device before its driver finishes registering
//! the port (so the first enumeration would miss it).
//!
//! * **Windows** — two `Windows.Devices.Enumeration.DeviceWatcher`s (MIDI in/out
//!   selectors). Pairs with the `winrt` feature; needs a process MTA apartment.
//! * **macOS** — a CoreMIDI client notification callback (`SetupChanged` etc.).
//! * **Linux** — a subscription to the ALSA sequencer `System:announce` port.

use std::sync::mpsc::{self, Receiver, RecvTimeoutError, Sender};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::Duration;

/// Called when the OS reports the MIDI device set may have changed. Runs on the
/// watcher's own thread; the implementation must be thread-safe.
pub type OnChange = Arc<dyn Fn() + Send + Sync + 'static>;

/// Quiet period that ends a burst of device events before the first refresh.
const DEBOUNCE: Duration = Duration::from_millis(200);
/// Delay before the follow-up "settle" refresh that catches a port which wasn't
/// enumerable yet when the OS first announced its device.
const SETTLE: Duration = Duration::from_millis(450);

/// Owns the worker thread that turns OS event pokes into [`OnChange`] calls.
/// Backends obtain a [`Sender`] via [`Coalescer::poker`] and send `()` per event.
struct Coalescer {
    tx: Sender<()>,
    stop: Arc<AtomicBool>,
    handle: Option<JoinHandle<()>>,
}

impl Coalescer {
    fn new(on_change: OnChange) -> Self {
        let (tx, rx) = mpsc::channel::<()>();
        let stop = Arc::new(AtomicBool::new(false));
        let stop_thread = stop.clone();
        let handle = thread::Builder::new()
            .name("ss-midi-hotswap".into())
            .spawn(move || coalesce_loop(rx, stop_thread, on_change))
            .expect("spawn ss-midi-hotswap thread");
        Coalescer { tx, stop, handle: Some(handle) }
    }

    /// A sender a backend can clone into its event handlers to poke a refresh.
    fn poker(&self) -> Sender<()> {
        self.tx.clone()
    }
}

impl Drop for Coalescer {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::SeqCst);
        // Unblock a thread parked in recv(); it re-checks `stop` and exits.
        let _ = self.tx.send(());
        if let Some(h) = self.handle.take() {
            let _ = h.join();
        }
    }
}

fn coalesce_loop(rx: Receiver<()>, stop: Arc<AtomicBool>, on_change: OnChange) {
    loop {
        // Block until the first event of a burst (or the stop poke).
        if rx.recv().is_err() || stop.load(Ordering::SeqCst) {
            return;
        }
        // Drain the rest of the burst: keep extending until it goes quiet.
        loop {
            match rx.recv_timeout(DEBOUNCE) {
                Ok(()) => {
                    if stop.load(Ordering::SeqCst) {
                        return;
                    }
                }
                Err(RecvTimeoutError::Timeout) => break,
                Err(RecvTimeoutError::Disconnected) => return,
            }
        }
        if stop.load(Ordering::SeqCst) {
            return;
        }
        on_change();
        // Settle pass — diffed downstream, so a no-op unless a port became
        // enumerable in the gap.
        thread::sleep(SETTLE);
        if stop.load(Ordering::SeqCst) {
            return;
        }
        on_change();
    }
}

/// A live MIDI hot-swap watcher. Drop to stop it.
pub struct Watcher {
    _imp: imp::PlatformWatcher,
}

impl Watcher {
    /// Start watching for MIDI device changes, calling `on_change` on each.
    pub fn new(on_change: OnChange) -> Self {
        Watcher {
            _imp: imp::PlatformWatcher::new(on_change),
        }
    }
}

// ── Windows: WinRT DeviceWatcher ──────────────────────────────────────────────
#[cfg(windows)]
mod imp {
    use super::{Coalescer, OnChange};
    use std::sync::mpsc::Sender;
    use std::sync::Once;

    use windows::core::HSTRING;
    use windows::Devices::Enumeration::{
        DeviceInformation, DeviceInformationUpdate, DeviceWatcher,
    };
    use windows::Devices::Midi::{MidiInPort, MidiOutPort};
    use windows::Foundation::TypedEventHandler;

    /// midir's WinRT backend (and our enumeration) activate WinRT objects and
    /// block on their async results, which requires an apartment. Establish a
    /// process-wide implicit MTA once so every MIDI thread — including this
    /// watcher's — is served without each having to initialize COM itself.
    fn ensure_mta() {
        static MTA: Once = Once::new();
        MTA.call_once(|| {
            // Never decremented: the cookie is simply dropped, leaving the
            // implicit MTA alive for the process lifetime so all MIDI threads
            // (this watcher, enumeration, the OSC handler) can activate WinRT.
            let _ = unsafe { windows::Win32::System::Com::CoIncrementMTAUsage() };
        });
    }

    /// One MIDI device-class watcher (input or output) plus its handler tokens.
    struct ClassWatcher {
        watcher: DeviceWatcher,
        added: i64,
        removed: i64,
        updated: i64,
    }

    impl ClassWatcher {
        fn new(selector: &HSTRING, poke: Sender<()>) -> windows::core::Result<Self> {
            let watcher = DeviceInformation::CreateWatcherAqsFilter(selector)?;

            let p = poke.clone();
            let added = watcher.Added(&TypedEventHandler::<DeviceWatcher, DeviceInformation>::new(
                move |_, _| {
                    let _ = p.send(());
                    Ok(())
                },
            ))?;

            let p = poke.clone();
            let removed = watcher.Removed(&TypedEventHandler::<
                DeviceWatcher,
                DeviceInformationUpdate,
            >::new(move |_, _| {
                let _ = p.send(());
                Ok(())
            }))?;

            let p = poke;
            let updated = watcher.Updated(&TypedEventHandler::<
                DeviceWatcher,
                DeviceInformationUpdate,
            >::new(move |_, _| {
                let _ = p.send(());
                Ok(())
            }))?;

            watcher.Start()?;
            Ok(ClassWatcher { watcher, added, removed, updated })
        }
    }

    impl Drop for ClassWatcher {
        fn drop(&mut self) {
            let _ = self.watcher.Stop();
            let _ = self.watcher.RemoveAdded(self.added);
            let _ = self.watcher.RemoveRemoved(self.removed);
            let _ = self.watcher.RemoveUpdated(self.updated);
        }
    }

    pub struct PlatformWatcher {
        // Drop order: class watchers first (stop OS callbacks, releasing their
        // sender clones), then the coalescer joins its now-idle thread.
        _ins: Option<ClassWatcher>,
        _outs: Option<ClassWatcher>,
        _coalescer: Coalescer,
    }

    impl PlatformWatcher {
        pub fn new(on_change: OnChange) -> Self {
            ensure_mta();
            let coalescer = Coalescer::new(on_change);
            let ins = MidiInPort::GetDeviceSelector()
                .and_then(|sel| ClassWatcher::new(&sel, coalescer.poker()))
                .ok();
            let outs = MidiOutPort::GetDeviceSelector()
                .and_then(|sel| ClassWatcher::new(&sel, coalescer.poker()))
                .ok();
            PlatformWatcher { _ins: ins, _outs: outs, _coalescer: coalescer }
        }
    }
}

// ── macOS: CoreMIDI client notifications ──────────────────────────────────────
#[cfg(target_os = "macos")]
mod imp {
    use super::{Coalescer, OnChange};
    use coremidi::{Client, Notification};

    pub struct PlatformWatcher {
        // The client must stay alive to keep delivering notifications. Dropped
        // before the coalescer so no poke arrives mid-teardown.
        _client: Option<Client>,
        _coalescer: Coalescer,
    }

    impl PlatformWatcher {
        pub fn new(on_change: OnChange) -> Self {
            let coalescer = Coalescer::new(on_change);
            let poke = coalescer.poker();
            // Any setup change (object added/removed, property changed) coalesces
            // into a single re-enumerate.
            let client = Client::new_with_notifications("supersonic-midi-hotswap", move |_n: &Notification| {
                let _ = poke.send(());
            })
            .ok();
            PlatformWatcher { _client: client, _coalescer: coalescer }
        }
    }
}

// ── Linux: ALSA sequencer System:announce ─────────────────────────────────────
#[cfg(target_os = "linux")]
mod imp {
    use super::{Coalescer, OnChange};
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::Arc;
    use std::thread::{self, JoinHandle};
    use std::time::Duration;

    use alsa::seq::{Addr, EventType, PortCap, PortSubscribe, PortType, Seq};

    /// The well-known ALSA sequencer System:announce source (client 0, port 1):
    /// it emits PortStart/PortExit/ClientStart/ClientExit as devices come and go.
    const SYSTEM_CLIENT: i32 = 0;
    const SYSTEM_ANNOUNCE_PORT: i32 = 1;

    pub struct PlatformWatcher {
        stop: Arc<AtomicBool>,
        handle: Option<JoinHandle<()>>,
    }

    impl PlatformWatcher {
        pub fn new(on_change: OnChange) -> Self {
            let coalescer = Coalescer::new(on_change);
            let stop = Arc::new(AtomicBool::new(false));
            let stop_thread = stop.clone();
            // The blocking-read loop owns the Seq and the coalescer so both live
            // exactly as long as the thread; stopping is cooperative via `stop`.
            let handle = thread::Builder::new()
                .name("ss-midi-announce".into())
                .spawn(move || announce_loop(stop_thread, coalescer))
                .ok();
            PlatformWatcher { stop, handle }
        }
    }

    impl Drop for PlatformWatcher {
        fn drop(&mut self) {
            self.stop.store(true, Ordering::SeqCst);
            if let Some(h) = self.handle.take() {
                let _ = h.join();
            }
        }
    }

    fn announce_loop(stop: Arc<AtomicBool>, coalescer: Coalescer) {
        let seq = match Seq::open(None, Some(alsa::Direction::Capture), true) {
            Ok(s) => s,
            Err(_) => return,
        };
        let _ = seq.set_client_name(&std::ffi::CString::new("supersonic-midi-hotswap").unwrap());

        // A local port to receive announcements on.
        let port = {
            use alsa::seq::PortInfo;
            let mut info = PortInfo::empty().unwrap();
            info.set_capability(PortCap::WRITE | PortCap::SUBS_WRITE);
            info.set_type(PortType::MIDI_GENERIC | PortType::APPLICATION);
            info.set_name(&std::ffi::CString::new("hotswap").unwrap());
            if seq.create_port(&info).is_err() {
                return;
            }
            info.get_port()
        };

        // Subscribe our port to System:announce.
        let sub = PortSubscribe::empty().unwrap();
        sub.set_sender(Addr { client: SYSTEM_CLIENT, port: SYSTEM_ANNOUNCE_PORT });
        sub.set_dest(Addr { client: seq.client_id().unwrap_or(0), port });
        if seq.subscribe_port(&sub).is_err() {
            return;
        }

        let poke = coalescer.poker();
        let mut input = seq.input();
        loop {
            if stop.load(Ordering::SeqCst) {
                return;
            }
            // Poll with a timeout so the stop flag is honoured promptly.
            match seq.event_input_pending(true) {
                Ok(0) => {
                    thread::sleep(Duration::from_millis(100));
                    continue;
                }
                Err(_) => return,
                Ok(_) => {}
            }
            match input.event_input() {
                Ok(ev) => match ev.get_type() {
                    EventType::PortStart
                    | EventType::PortExit
                    | EventType::ClientStart
                    | EventType::ClientExit => {
                        let _ = poke.send(());
                    }
                    _ => {}
                },
                Err(_) => return,
            }
        }
    }
}

// ── Other targets: no native hot-swap (manual /midi/refresh still works) ───────
#[cfg(not(any(windows, target_os = "macos", target_os = "linux")))]
mod imp {
    use super::{Coalescer, OnChange};

    pub struct PlatformWatcher {
        _coalescer: Coalescer,
    }

    impl PlatformWatcher {
        pub fn new(on_change: OnChange) -> Self {
            PlatformWatcher { _coalescer: Coalescer::new(on_change) }
        }
    }
}
