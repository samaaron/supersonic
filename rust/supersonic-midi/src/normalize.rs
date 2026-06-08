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
/// unique. The Nth occurrence (N≥2) of a normalised name gets `_N`, matching
/// sp_midi's duplicate handling.
pub fn normalize_ports(raw_names: &[String]) -> Vec<PortInfo> {
    let mut out: Vec<PortInfo> = Vec::with_capacity(raw_names.len());
    for raw in raw_names {
        let base = safe_osc_name(raw);
        let count = out.iter().filter(|p| base_of(&p.normalized) == base).count();
        let normalized = if count == 0 {
            base
        } else {
            format!("{base}_{}", count + 1)
        };
        out.push(PortInfo {
            raw: raw.clone(),
            normalized,
        });
    }
    out
}

/// Strip a trailing `_<n>` dedup suffix to recover the base name, so duplicate
/// counting groups `foo`, `foo_2`, `foo_3` together.
fn base_of(name: &str) -> String {
    if let Some(idx) = name.rfind('_') {
        if name[idx + 1..].chars().all(|c| c.is_ascii_digit()) && idx + 1 < name.len() {
            return name[..idx].to_string();
        }
    }
    name.to_string()
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
    fn base_of_strips_numeric_suffix_only() {
        assert_eq!(base_of("launchpad_2"), "launchpad");
        assert_eq!(base_of("launchpad"), "launchpad");
        assert_eq!(base_of("model_d"), "model_d"); // non-numeric suffix kept
    }
}
