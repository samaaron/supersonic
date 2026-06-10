//! Minimal, dependency-free OSC 1.0 message codec.
//!
//! Only what the subsystem schemas (`/midi/*`, `/gamepad/*`) need: a flat
//! message (no bundles) with int32, float32, int64, string and blob arguments.
//! Kept tiny and `std`-only so it builds identically for the native staticlibs
//! and the wasm-bindgen modules without pulling an OSC crate into the wasm
//! bundles.

/// A single decoded OSC argument.
#[derive(Clone, Debug, PartialEq)]
pub enum OscArg {
    Int(i32),
    Float(f32),
    Long(i64),
    Str(String),
    Blob(Vec<u8>),
}

impl OscArg {
    pub fn as_i32(&self) -> Option<i32> {
        match self {
            OscArg::Int(v) => Some(*v),
            OscArg::Long(v) => Some(*v as i32),
            OscArg::Float(v) => Some(*v as i32),
            _ => None,
        }
    }
    pub fn as_f64(&self) -> Option<f64> {
        match self {
            OscArg::Float(v) => Some(*v as f64),
            OscArg::Int(v) => Some(*v as f64),
            OscArg::Long(v) => Some(*v as f64),
            _ => None,
        }
    }
    pub fn as_str(&self) -> Option<&str> {
        match self {
            OscArg::Str(s) => Some(s),
            _ => None,
        }
    }
    pub fn as_blob(&self) -> Option<&[u8]> {
        match self {
            OscArg::Blob(b) => Some(b),
            _ => None,
        }
    }
}

/// A decoded OSC message: address + ordered arguments.
#[derive(Clone, Debug, PartialEq)]
pub struct OscMessage {
    pub addr: String,
    pub args: Vec<OscArg>,
}

fn pad4(out: &mut Vec<u8>) {
    while out.len() % 4 != 0 {
        out.push(0);
    }
}

fn push_string(out: &mut Vec<u8>, s: &str) {
    out.extend_from_slice(s.as_bytes());
    out.push(0);
    pad4(out);
}

/// Encode a flat OSC message.
pub fn encode(addr: &str, args: &[OscArg]) -> Vec<u8> {
    let mut out = Vec::with_capacity(addr.len() + 16 + args.len() * 4);
    push_string(&mut out, addr);

    let mut tags = String::with_capacity(args.len() + 1);
    tags.push(',');
    for a in args {
        tags.push(match a {
            OscArg::Int(_) => 'i',
            OscArg::Float(_) => 'f',
            OscArg::Long(_) => 'h',
            OscArg::Str(_) => 's',
            OscArg::Blob(_) => 'b',
        });
    }
    push_string(&mut out, &tags);

    for a in args {
        match a {
            OscArg::Int(v) => out.extend_from_slice(&v.to_be_bytes()),
            OscArg::Float(v) => out.extend_from_slice(&v.to_be_bytes()),
            OscArg::Long(v) => out.extend_from_slice(&v.to_be_bytes()),
            OscArg::Str(s) => push_string(&mut out, s),
            OscArg::Blob(b) => {
                out.extend_from_slice(&(b.len() as i32).to_be_bytes());
                out.extend_from_slice(b);
                pad4(&mut out);
            }
        }
    }
    out
}

fn read_string(data: &[u8]) -> Option<(String, &[u8])> {
    let end = data.iter().position(|&b| b == 0)?;
    let s = core::str::from_utf8(&data[..end]).ok()?.to_string();
    let padded = (end + 4) & !3; // include NUL, round up to 4
    if padded > data.len() {
        return None;
    }
    Some((s, &data[padded..]))
}

fn read_i32(data: &[u8]) -> Option<(i32, &[u8])> {
    let b: [u8; 4] = data.get(..4)?.try_into().ok()?;
    Some((i32::from_be_bytes(b), &data[4..]))
}

fn read_f32(data: &[u8]) -> Option<(f32, &[u8])> {
    let b: [u8; 4] = data.get(..4)?.try_into().ok()?;
    Some((f32::from_be_bytes(b), &data[4..]))
}

fn read_i64(data: &[u8]) -> Option<(i64, &[u8])> {
    let b: [u8; 8] = data.get(..8)?.try_into().ok()?;
    Some((i64::from_be_bytes(b), &data[8..]))
}

fn read_blob(data: &[u8]) -> Option<(Vec<u8>, &[u8])> {
    let (len, rest) = read_i32(data)?;
    let len = usize::try_from(len).ok()?;
    let bytes = rest.get(..len)?.to_vec();
    let padded = (len + 3) & !3;
    if padded > rest.len() {
        return None;
    }
    Some((bytes, &rest[padded..]))
}

/// Decode a flat OSC message. Returns `None` on malformed input or an
/// unsupported type tag.
pub fn decode(data: &[u8]) -> Option<OscMessage> {
    let (addr, rest) = read_string(data)?;
    if !addr.starts_with('/') {
        return None;
    }
    let (tags, mut rest) = read_string(rest)?;
    let mut tags = tags.chars();
    if tags.next() != Some(',') {
        return None;
    }
    let mut args = Vec::new();
    for t in tags {
        let (arg, r) = match t {
            'i' => {
                let (v, r) = read_i32(rest)?;
                (OscArg::Int(v), r)
            }
            'f' => {
                let (v, r) = read_f32(rest)?;
                (OscArg::Float(v), r)
            }
            'h' => {
                let (v, r) = read_i64(rest)?;
                (OscArg::Long(v), r)
            }
            's' => {
                let (v, r) = read_string(rest)?;
                (OscArg::Str(v), r)
            }
            'b' => {
                let (v, r) = read_blob(rest)?;
                (OscArg::Blob(v), r)
            }
            _ => return None,
        };
        args.push(arg);
        rest = r;
    }
    Some(OscMessage { addr, args })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrips_mixed_args() {
        let args = vec![
            OscArg::Str("launchpad".into()),
            OscArg::Int(1),
            OscArg::Int(60),
            OscArg::Float(0.5),
            OscArg::Blob(vec![0xF0, 0x7e, 0xF7]),
        ];
        let bytes = encode("/midi/in/note_on", &args);
        assert_eq!(bytes.len() % 4, 0);
        let msg = decode(&bytes).unwrap();
        assert_eq!(msg.addr, "/midi/in/note_on");
        assert_eq!(msg.args, args);
    }

    #[test]
    fn empty_args() {
        let bytes = encode("/midi/refresh", &[]);
        let msg = decode(&bytes).unwrap();
        assert_eq!(msg.addr, "/midi/refresh");
        assert!(msg.args.is_empty());
    }

    #[test]
    fn rejects_malformed() {
        assert!(decode(&[]).is_none());
        assert!(decode(b"no-leading-slash\0\0\0\0,\0\0\0").is_none());
    }
}
