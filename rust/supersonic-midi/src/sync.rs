//! Clock-IN transport mapping: turn the *position-bearing* MIDI messages into
//! SuperClock intents. Tempo comes from [`crate::clock::ClockEstimator`]; this
//! module covers musical position, which the anonymous clock pulses cannot.
//!
//! MIDI clock ticks carry no phase, so beat/bar alignment is established only by
//! Start (→ position 0), Continue (resume), Stop, and Song Position Pointer.

use crate::message::MidiMessage;

/// A transport intent the host applies to SuperClock.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum TransportEvent {
    /// Reset to the start (beat 0) and begin playing.
    Start,
    /// Resume playing from the current position.
    Continue,
    /// Stop playing.
    Stop,
    /// Jump to a position given in MIDI beats (1/16 notes), from a Song Position
    /// Pointer. Use [`TransportEvent::beat`] for the quarter-note beat.
    Position { sixteenths: u16 },
}

impl TransportEvent {
    /// The quarter-note beat this event sets, if any (Start → 0, Position →
    /// sixteenths/4). Continue/Stop don't move the beat origin.
    pub fn beat(&self) -> Option<f64> {
        match self {
            TransportEvent::Start => Some(0.0),
            TransportEvent::Position { sixteenths } => Some(*sixteenths as f64 / 4.0),
            _ => None,
        }
    }
}

/// Map a position-bearing MIDI message to a [`TransportEvent`]. Returns `None`
/// for everything else (including clock pulses, which drive the tempo estimator).
pub fn transport_event(msg: &MidiMessage) -> Option<TransportEvent> {
    match msg {
        MidiMessage::Start => Some(TransportEvent::Start),
        MidiMessage::Continue => Some(TransportEvent::Continue),
        MidiMessage::Stop => Some(TransportEvent::Stop),
        MidiMessage::SongPosition(p) => Some(TransportEvent::Position { sixteenths: *p }),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn maps_transport_messages() {
        assert_eq!(transport_event(&MidiMessage::Start), Some(TransportEvent::Start));
        assert_eq!(transport_event(&MidiMessage::Stop), Some(TransportEvent::Stop));
        assert_eq!(
            transport_event(&MidiMessage::Continue),
            Some(TransportEvent::Continue)
        );
        assert_eq!(
            transport_event(&MidiMessage::SongPosition(16)),
            Some(TransportEvent::Position { sixteenths: 16 })
        );
    }

    #[test]
    fn ignores_non_transport() {
        assert_eq!(transport_event(&MidiMessage::Clock), None);
        assert_eq!(
            transport_event(&MidiMessage::NoteOn { channel: 1, note: 1, velocity: 1 }),
            None
        );
    }

    #[test]
    fn beat_conversion() {
        // 16 sixteenths = beat 4.0
        assert_eq!(
            TransportEvent::Position { sixteenths: 16 }.beat(),
            Some(4.0)
        );
        assert_eq!(TransportEvent::Start.beat(), Some(0.0));
        assert_eq!(TransportEvent::Stop.beat(), None);
    }
}
