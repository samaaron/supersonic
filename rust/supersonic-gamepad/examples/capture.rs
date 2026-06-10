//! Diagnostic: print device connects/disconnects and every translated event,
//! through the same backend the engine uses (gilrs, or GCController on
//! macOS). Run with: cargo run -p supersonic-gamepad --example capture
use std::sync::{Arc, Mutex};
use std::time::Duration;

use supersonic_gamepad::backend::GamepadIo;
use supersonic_gamepad::io::Registry;

fn main() {
    let registry = Arc::new(Mutex::new(Registry::default()));
    let reg = registry.clone();
    std::thread::spawn(move || {
        let mut io = GamepadIo::new(reg).expect("gamepad backend init");
        println!("polling — press buttons (ctrl-c to quit)…");
        loop {
            for out in io.poll(Duration::from_millis(4)) {
                if let supersonic_gamepad::io::Out::DevicesChanged = out {
                    println!("devices: {:?}", registry.lock().unwrap().snapshot());
                } else {
                    println!("{out:?}");
                }
            }
        }
    });

    // GCController discovery only delivers while the main run loop is pumped
    // (the supersonic standalone does the same — src/native/Main.cpp). The
    // once-per-second probe prints raw profile reads, bypassing the diffing,
    // to make "no input" vs "no presses" distinguishable.
    #[cfg(target_os = "macos")]
    {
        use objc2_game_controller::GCController;
        let mut ticks = 0u32;
        loop {
            objc2_core_foundation::CFRunLoop::run_in_mode(
                unsafe { objc2_core_foundation::kCFRunLoopDefaultMode },
                0.1,
                false,
            );
            ticks += 1;
            if ticks % 10 == 0 {
                let pads = unsafe { GCController::controllers() }.to_vec();
                if let Some(ext) = pads.first().and_then(|c| unsafe { c.extendedGamepad() }) {
                    unsafe {
                        println!(
                            "probe: A={}({:.2}) leftX={:.2} leftY={:.2} rt={:.2}",
                            ext.buttonA().isPressed(),
                            ext.buttonA().value(),
                            ext.leftThumbstick().xAxis().value(),
                            ext.leftThumbstick().yAxis().value(),
                            ext.rightTrigger().value(),
                        );
                    }
                }
            }
        }
    }
    #[cfg(not(target_os = "macos"))]
    loop {
        std::thread::sleep(Duration::from_secs(1));
    }
}
