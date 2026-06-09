//! Device-name normalisation + de-duplication.
//!
//! Port names come from the OS (or the browser) with spaces and characters that
//! are unsafe inside an OSC address. We normalise them to a stable, OSC-safe,
//! lowercase token matching sp_midi's `safeOscString`, then suffix
//! duplicates (`_2`, `_3`, …) so every device has a unique handle. The handle is
//! the identifier clients use in `/midi/*` addresses, so it must be stable and
//! reproducible across enumerations.

/// Characters that are unsafe in an OSC address (or reserved by Sonic Pi's
/// `:`-delimited paths) are replaced with `_`. Mirrors sp_midi/src/utils.cpp.
const UNSAFE: &[char] = &[' ', '#', '*', ',', '/', '?', '[', ']', '{', '}', ':'];

/// Normalise one raw port name: replace unsafe chars with `_`, lowercase.
pub fn safe_osc_name(raw: &str) -> String {
    raw.chars()
        .map(|c| if UNSAFE.contains(&c) { '_' } else { c })
        .flat_map(|c| c.to_lowercase())
        .collect()
}

/// A raw OS port name paired with its normalised, de-duplicated handle.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PortInfo {
    pub raw: String,
    pub normalized: String,
}

/// Normalise a list of raw port names, suffixing duplicates so each handle is
/// unique. A name first claims its bare normalised form; later collisions take
/// the lowest free `_2`, `_3`, … suffix.
///
/// Uniqueness is checked against the set of handles already issued — NOT by
/// stripping `_<n>` off prior names. Stripping conflates a device whose real
/// name normalises to e.g. `midi_2` with the dedup suffix of `midi`, which can
/// hand two physical devices the same handle (the wrong device then gets
/// addressed by `/midi/*`, and their per-port clock timelines merge).
pub fn normalize_ports(raw_names: &[String]) -> Vec<PortInfo> {
    let mut out: Vec<PortInfo> = Vec::with_capacity(raw_names.len());
    let mut used: std::collections::HashSet<String> = std::collections::HashSet::new();
    for raw in raw_names {
        let base = safe_osc_name(raw);
        let mut handle = base.clone();
        let mut n = 2;
        while used.contains(&handle) {
            handle = format!("{base}_{n}");
            n += 1;
        }
        used.insert(handle.clone());
        out.push(PortInfo {
            raw: raw.clone(),
            normalized: handle,
        });
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn normalises_unsafe_chars() {
        assert_eq!(safe_osc_name("USB Keyboard 1"), "usb_keyboard_1");
        assert_eq!(safe_osc_name("Arturia: BeatStep/Pro"), "arturia__beatstep_pro");
        assert_eq!(safe_osc_name("MIDI*Port#3"), "midi_port_3");
    }

    #[test]
    fn dedups_duplicates() {
        let raw = vec![
            "Launchpad".to_string(),
            "Launchpad".to_string(),
            "Launch pad".to_string(), // normalises to "launch_pad"
            "Launchpad".to_string(),
        ];
        let got = normalize_ports(&raw);
        assert_eq!(got[0].normalized, "launchpad");
        assert_eq!(got[1].normalized, "launchpad_2");
        assert_eq!(got[2].normalized, "launch_pad");
        assert_eq!(got[3].normalized, "launchpad_3");
    }

    #[test]
    fn a_real_numeric_name_does_not_collide_with_a_dedup_suffix() {
        // "MIDI 2" normalises to "midi_2". A naive dedup that strips "_2" would
        // treat the next "MIDI" as a duplicate of "midi" and also hand it
        // "midi_2" — two devices, one handle. Each handle must be unique.
        let raw = vec![
            "MIDI 2".to_string(), // -> midi_2
            "MIDI".to_string(),   // -> midi   (NOT midi_2)
            "MIDI".to_string(),   // -> midi_3 (midi and midi_2 both taken)
        ];
        let got = normalize_ports(&raw);
        assert_eq!(got[0].normalized, "midi_2");
        assert_eq!(got[1].normalized, "midi");
        assert_eq!(got[2].normalized, "midi_3");
    }

    #[test]
    fn handles_are_always_unique() {
        // Whatever the inputs (including names that look like dedup suffixes),
        // no two devices ever share a handle.
        let raw = vec![
            "Port".to_string(),
            "Port".to_string(),
            "Port 2".to_string(),
            "Port_2".to_string(),
            "Port".to_string(),
            "Port 3".to_string(),
        ];
        let got = normalize_ports(&raw);
        let handles: std::collections::HashSet<_> =
            got.iter().map(|p| p.normalized.clone()).collect();
        assert_eq!(handles.len(), raw.len(), "every handle must be distinct");
    }
}
