//! Dependency-free, complete OSC 1.0 codec (messages + bundles).
//!
//! Supports every standard OSC argument type — int32/float32/int64/double,
//! timetag, string/symbol, blob, char, rgba, midi, true/false, nil, impulse,
//! and nested arrays — plus `#bundle` packets (with nested bundles). `std`-only
//! so it builds identically for the native staticlibs and the wasm-bindgen
//! modules without pulling in a third-party OSC crate.
//!
//! `encode`/`decode` operate on a flat message (what the `/midi/*`, `/gamepad/*`
//! and `/osc/*` schemas use day to day). `encode_packet`/`decode_packet` add the
//! bundle layer for forwarding arbitrary external OSC.

/// A single decoded OSC argument. Covers the full OSC 1.0 type-tag set.
#[derive(Clone, Debug, PartialEq)]
pub enum OscArg {
    Int(i32),         // 'i'
    Float(f32),       // 'f'
    Long(i64),        // 'h'
    Double(f64),      // 'd'
    TimeTag(u64),     // 't'
    Char(u32),        // 'c'
    Color(u32),       // 'r' — RGBA packed big-endian
    Midi(u32),        // 'm' — port id + 3 MIDI bytes, packed
    Bool(bool),       // 'T' / 'F' (no payload)
    Nil,              // 'N' (no payload)
    Inf,              // 'I' — impulse / infinitum (no payload)
    Str(String),      // 's'
    Symbol(String),   // 'S'
    Blob(Vec<u8>),    // 'b'
    Array(Vec<OscArg>), // '[' .. ']'
}

impl OscArg {
    pub fn as_i32(&self) -> Option<i32> {
        match self {
            OscArg::Int(v) => Some(*v),
            OscArg::Long(v) => Some(*v as i32),
            OscArg::Float(v) => Some(*v as i32),
            OscArg::Double(v) => Some(*v as i32),
            _ => None,
        }
    }
    pub fn as_f64(&self) -> Option<f64> {
        match self {
            OscArg::Float(v) => Some(*v as f64),
            OscArg::Double(v) => Some(*v),
            OscArg::Int(v) => Some(*v as f64),
            OscArg::Long(v) => Some(*v as f64),
            _ => None,
        }
    }
    pub fn as_str(&self) -> Option<&str> {
        match self {
            OscArg::Str(s) | OscArg::Symbol(s) => Some(s),
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

/// A decoded OSC bundle: a timetag + ordered elements (each a packet).
#[derive(Clone, Debug, PartialEq)]
pub struct OscBundle {
    pub timetag: u64,
    pub elements: Vec<OscPacket>,
}

/// A top-level OSC packet — either a single message or a bundle.
#[derive(Clone, Debug, PartialEq)]
pub enum OscPacket {
    Message(OscMessage),
    Bundle(OscBundle),
}

// ── encoding ─────────────────────────────────────────────────────────────────

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

// Append an argument's type tag(s). Arrays recurse, bracketed by '[' ']'.
fn push_tag(tags: &mut String, a: &OscArg) {
    match a {
        OscArg::Int(_) => tags.push('i'),
        OscArg::Float(_) => tags.push('f'),
        OscArg::Long(_) => tags.push('h'),
        OscArg::Double(_) => tags.push('d'),
        OscArg::TimeTag(_) => tags.push('t'),
        OscArg::Char(_) => tags.push('c'),
        OscArg::Color(_) => tags.push('r'),
        OscArg::Midi(_) => tags.push('m'),
        OscArg::Bool(true) => tags.push('T'),
        OscArg::Bool(false) => tags.push('F'),
        OscArg::Nil => tags.push('N'),
        OscArg::Inf => tags.push('I'),
        OscArg::Str(_) => tags.push('s'),
        OscArg::Symbol(_) => tags.push('S'),
        OscArg::Blob(_) => tags.push('b'),
        OscArg::Array(items) => {
            tags.push('[');
            for it in items {
                push_tag(tags, it);
            }
            tags.push(']');
        }
    }
}

// Append an argument's payload bytes. Type-tag-only args (bool/nil/inf) and
// arrays (payload is just their elements') write nothing structural here.
fn push_payload(out: &mut Vec<u8>, a: &OscArg) {
    match a {
        OscArg::Int(v) => out.extend_from_slice(&v.to_be_bytes()),
        OscArg::Float(v) => out.extend_from_slice(&v.to_be_bytes()),
        OscArg::Long(v) => out.extend_from_slice(&v.to_be_bytes()),
        OscArg::Double(v) => out.extend_from_slice(&v.to_be_bytes()),
        OscArg::TimeTag(v) => out.extend_from_slice(&v.to_be_bytes()),
        OscArg::Char(v) | OscArg::Color(v) | OscArg::Midi(v) => {
            out.extend_from_slice(&v.to_be_bytes())
        }
        OscArg::Bool(_) | OscArg::Nil | OscArg::Inf => {}
        OscArg::Str(s) | OscArg::Symbol(s) => push_string(out, s),
        OscArg::Blob(b) => {
            out.extend_from_slice(&(b.len() as i32).to_be_bytes());
            out.extend_from_slice(b);
            pad4(out);
        }
        OscArg::Array(items) => {
            for it in items {
                push_payload(out, it);
            }
        }
    }
}

/// Encode a flat OSC message.
pub fn encode(addr: &str, args: &[OscArg]) -> Vec<u8> {
    let mut out = Vec::with_capacity(addr.len() + 16 + args.len() * 4);
    push_string(&mut out, addr);

    let mut tags = String::with_capacity(args.len() + 1);
    tags.push(',');
    for a in args {
        push_tag(&mut tags, a);
    }
    push_string(&mut out, &tags);

    for a in args {
        push_payload(&mut out, a);
    }
    out
}

/// Encode an OSC packet (message or bundle, nested).
pub fn encode_packet(packet: &OscPacket) -> Vec<u8> {
    match packet {
        OscPacket::Message(m) => encode(&m.addr, &m.args),
        OscPacket::Bundle(b) => {
            let mut out = Vec::new();
            push_string(&mut out, "#bundle");
            out.extend_from_slice(&b.timetag.to_be_bytes());
            for el in &b.elements {
                let bytes = encode_packet(el);
                out.extend_from_slice(&(bytes.len() as i32).to_be_bytes());
                out.extend_from_slice(&bytes);
            }
            out
        }
    }
}

// ── decoding ─────────────────────────────────────────────────────────────────

fn read_string(data: &[u8]) -> Option<(String, &[u8])> {
    let end = data.iter().position(|&b| b == 0)?;
    let s = core::str::from_utf8(&data[..end]).ok()?.to_string();
    let padded = (end + 4) & !3; // include NUL, round up to 4
    if padded > data.len() {
        return None;
    }
    Some((s, &data[padded..]))
}

fn read_u32(data: &[u8]) -> Option<(u32, &[u8])> {
    let b: [u8; 4] = data.get(..4)?.try_into().ok()?;
    Some((u32::from_be_bytes(b), &data[4..]))
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

fn read_u64(data: &[u8]) -> Option<(u64, &[u8])> {
    let b: [u8; 8] = data.get(..8)?.try_into().ok()?;
    Some((u64::from_be_bytes(b), &data[8..]))
}

fn read_f64(data: &[u8]) -> Option<(f64, &[u8])> {
    let b: [u8; 8] = data.get(..8)?.try_into().ok()?;
    Some((f64::from_be_bytes(b), &data[8..]))
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

/// Max nesting depth for arrays and bundles. Untrusted packets can craft
/// arbitrarily deep `[`/`#bundle` nesting; without a cap the recursive decoder
/// would stack-overflow (an uncatchable abort). Real OSC never nests this deep.
const MAX_DEPTH: u32 = 64;

// Decode one argument given its type-tag char, consuming from `rest`. Arrays
// recurse over `tags` until the matching ']'. Returns the arg and the remaining
// payload bytes.
fn read_arg<'a>(
    t: char,
    tags: &mut core::str::Chars,
    rest: &'a [u8],
    depth: u32,
) -> Option<(OscArg, &'a [u8])> {
    Some(match t {
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
        'd' => {
            let (v, r) = read_f64(rest)?;
            (OscArg::Double(v), r)
        }
        't' => {
            let (v, r) = read_u64(rest)?;
            (OscArg::TimeTag(v), r)
        }
        'c' => {
            let (v, r) = read_u32(rest)?;
            (OscArg::Char(v), r)
        }
        'r' => {
            let (v, r) = read_u32(rest)?;
            (OscArg::Color(v), r)
        }
        'm' => {
            let (v, r) = read_u32(rest)?;
            (OscArg::Midi(v), r)
        }
        'T' => (OscArg::Bool(true), rest),
        'F' => (OscArg::Bool(false), rest),
        'N' => (OscArg::Nil, rest),
        'I' => (OscArg::Inf, rest),
        's' => {
            let (v, r) = read_string(rest)?;
            (OscArg::Str(v), r)
        }
        'S' => {
            let (v, r) = read_string(rest)?;
            (OscArg::Symbol(v), r)
        }
        'b' => {
            let (v, r) = read_blob(rest)?;
            (OscArg::Blob(v), r)
        }
        '[' => {
            if depth >= MAX_DEPTH {
                return None;
            }
            let mut items = Vec::new();
            let mut r = rest;
            loop {
                let nt = tags.next()?;
                if nt == ']' {
                    break;
                }
                let (item, nr) = read_arg(nt, tags, r, depth + 1)?;
                items.push(item);
                r = nr;
            }
            (OscArg::Array(items), r)
        }
        _ => return None,
    })
}

/// Decode a flat OSC message. Returns `None` on malformed input.
pub fn decode(data: &[u8]) -> Option<OscMessage> {
    let (addr, rest) = read_string(data)?;
    if !addr.starts_with('/') {
        return None;
    }
    let (tagstr, mut rest) = read_string(rest)?;
    let mut tags = tagstr.chars();
    if tags.next() != Some(',') {
        return None;
    }
    let mut args = Vec::new();
    while let Some(t) = tags.next() {
        let (arg, r) = read_arg(t, &mut tags, rest, 0)?;
        args.push(arg);
        rest = r;
    }
    Some(OscMessage { addr, args })
}

/// Decode an OSC packet — a `#bundle` (recursively) or a single message.
pub fn decode_packet(data: &[u8]) -> Option<OscPacket> {
    decode_packet_depth(data, 0)
}

fn decode_packet_depth(data: &[u8], depth: u32) -> Option<OscPacket> {
    if data.starts_with(b"#bundle\0") {
        if depth >= MAX_DEPTH {
            return None;
        }
        let (timetag, mut rest) = read_u64(data.get(8..)?)?;
        let mut elements = Vec::new();
        while !rest.is_empty() {
            let (len, r) = read_i32(rest)?;
            let len = usize::try_from(len).ok()?;
            let elem = r.get(..len)?;
            elements.push(decode_packet_depth(elem, depth + 1)?);
            rest = &r[len..];
        }
        Some(OscPacket::Bundle(OscBundle { timetag, elements }))
    } else {
        Some(OscPacket::Message(decode(data)?))
    }
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
    fn roundtrips_all_scalar_types() {
        let args = vec![
            OscArg::Int(-7),
            OscArg::Float(1.5),
            OscArg::Long(1 << 40),
            OscArg::Double(-2.5),
            OscArg::TimeTag(0xDEAD_BEEF_0000_0001),
            OscArg::Char(0x41),
            OscArg::Color(0x11223344),
            OscArg::Midi(0x90400000),
            OscArg::Bool(true),
            OscArg::Bool(false),
            OscArg::Nil,
            OscArg::Inf,
            OscArg::Str("hi".into()),
            OscArg::Symbol("sym".into()),
            OscArg::Blob(vec![1, 2, 3, 4, 5]),
        ];
        let bytes = encode("/all", &args);
        assert_eq!(bytes.len() % 4, 0);
        assert_eq!(decode(&bytes).unwrap().args, args);
    }

    #[test]
    fn roundtrips_nested_arrays() {
        let args = vec![
            OscArg::Int(1),
            OscArg::Array(vec![
                OscArg::Float(2.0),
                OscArg::Array(vec![OscArg::Str("x".into()), OscArg::Bool(true)]),
            ]),
            OscArg::Int(9),
        ];
        let bytes = encode("/nested", &args);
        assert_eq!(decode(&bytes).unwrap().args, args);
    }

    #[test]
    fn roundtrips_bundle_with_nested_bundle() {
        let inner = OscPacket::Bundle(OscBundle {
            timetag: 42,
            elements: vec![OscPacket::Message(OscMessage {
                addr: "/b".into(),
                args: vec![OscArg::Int(2)],
            })],
        });
        let pkt = OscPacket::Bundle(OscBundle {
            timetag: 1,
            elements: vec![
                OscPacket::Message(OscMessage {
                    addr: "/a".into(),
                    args: vec![OscArg::Float(1.0), OscArg::Str("hi".into())],
                }),
                inner,
            ],
        });
        let bytes = encode_packet(&pkt);
        assert_eq!(decode_packet(&bytes).unwrap(), pkt);
    }

    #[test]
    fn decode_packet_reads_plain_message() {
        let bytes = encode("/foo", &[OscArg::Int(1)]);
        match decode_packet(&bytes).unwrap() {
            OscPacket::Message(m) => assert_eq!(m.addr, "/foo"),
            _ => panic!("expected message"),
        }
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

    // ── byte-exact wire vectors (differential against the OSC 1.0 spec itself) ──
    // Hand-computed so a wire-format regression is caught precisely, not just
    // self-consistently.

    #[test]
    fn wire_exact_int_message() {
        // "/a" + ",i" + int32(1)
        let bytes = encode("/a", &[OscArg::Int(1)]);
        assert_eq!(
            bytes,
            vec![
                b'/', b'a', 0, 0, // address, NUL, pad to 4
                b',', b'i', 0, 0, // type tags, pad to 4
                0, 0, 0, 1, // int32 big-endian
            ]
        );
    }

    #[test]
    fn wire_exact_bool_and_nil_have_no_payload() {
        // ",TFNI" carry no argument bytes — the tag IS the value.
        let bytes = encode(
            "/x",
            &[OscArg::Bool(true), OscArg::Bool(false), OscArg::Nil, OscArg::Inf],
        );
        assert_eq!(
            bytes,
            vec![
                b'/', b'x', 0, 0, //
                b',', b'T', b'F', b'N', b'I', 0, 0, 0, // ",TFNI" + NUL, pad to 8
            ]
        );
    }

    #[test]
    fn wire_exact_string_and_blob() {
        // "/s" + ",sb" + "hi"\0\0 + blob(len=3,[1,2,3],pad)
        let bytes = encode("/s", &[OscArg::Str("hi".into()), OscArg::Blob(vec![1, 2, 3])]);
        assert_eq!(
            bytes,
            vec![
                b'/', b's', 0, 0, //
                b',', b's', b'b', 0, //
                b'h', b'i', 0, 0, // "hi" + NUL + pad
                0, 0, 0, 3, // blob length
                1, 2, 3, 0, // blob bytes + pad to 4
            ]
        );
    }

    #[test]
    fn wire_exact_bundle() {
        // "#bundle"\0 + timetag(1) + elem-size(12) + ("/a" ",i" int32(1))
        let pkt = OscPacket::Bundle(OscBundle {
            timetag: 1,
            elements: vec![OscPacket::Message(OscMessage {
                addr: "/a".into(),
                args: vec![OscArg::Int(1)],
            })],
        });
        let bytes = encode_packet(&pkt);
        let mut expect = b"#bundle\0".to_vec();
        expect.extend_from_slice(&1u64.to_be_bytes()); // timetag
        expect.extend_from_slice(&12i32.to_be_bytes()); // element size
        expect.extend_from_slice(&[b'/', b'a', 0, 0, b',', b'i', 0, 0, 0, 0, 0, 1]);
        assert_eq!(bytes, expect);
    }

    // ── seeded generator (deterministic; no dev-deps) ──────────────────────────

    struct Lcg(u64);
    impl Lcg {
        fn next(&mut self) -> u64 {
            self.0 = self
                .0
                .wrapping_mul(6364136223846793005)
                .wrapping_add(1442695040888963407);
            self.0
        }
        fn below(&mut self, n: u32) -> u32 {
            ((self.next() >> 33) as u32) % n
        }
    }

    // A non-NaN, non-interior-NUL, valid-UTF-8 value for each type so encode→
    // decode is exact under PartialEq (NaN floats would break equality).
    fn gen_arg(rng: &mut Lcg, depth: u32) -> OscArg {
        let pick = rng.below(if depth < 3 { 15 } else { 14 });
        match pick {
            0 => OscArg::Int(rng.next() as i32),
            1 => OscArg::Float((rng.below(20000) as f32) / 100.0 - 100.0),
            2 => OscArg::Long(rng.next() as i64),
            3 => OscArg::Double((rng.below(20000) as f64) / 100.0 - 100.0),
            4 => OscArg::TimeTag(rng.next()),
            5 => OscArg::Char(rng.below(0x110000)),
            6 => OscArg::Color(rng.next() as u32),
            7 => OscArg::Midi(rng.next() as u32),
            8 => OscArg::Bool(rng.below(2) == 1),
            9 => OscArg::Nil,
            10 => OscArg::Inf,
            11 => OscArg::Str(gen_ascii(rng)),
            12 => OscArg::Symbol(gen_ascii(rng)),
            13 => OscArg::Blob((0..rng.below(10)).map(|_| rng.next() as u8).collect()),
            _ => {
                let n = rng.below(4);
                OscArg::Array((0..n).map(|_| gen_arg(rng, depth + 1)).collect())
            }
        }
    }

    fn gen_ascii(rng: &mut Lcg) -> String {
        let n = rng.below(8);
        (0..n)
            .map(|_| (b'a' + (rng.below(26) as u8)) as char)
            .collect()
    }

    fn gen_message(rng: &mut Lcg) -> OscMessage {
        let args = (0..rng.below(6)).map(|_| gen_arg(rng, 0)).collect();
        OscMessage { addr: format!("/{}", gen_ascii(rng)), args }
    }

    fn gen_packet(rng: &mut Lcg, depth: u32) -> OscPacket {
        if depth < 3 && rng.below(3) == 0 {
            let n = rng.below(4);
            OscPacket::Bundle(OscBundle {
                timetag: rng.next(),
                elements: (0..n).map(|_| gen_packet(rng, depth + 1)).collect(),
            })
        } else {
            OscPacket::Message(gen_message(rng))
        }
    }

    // ── property round-trips (full type space, nested arrays + bundles) ────────

    #[test]
    fn property_message_roundtrip() {
        for seed in 0..5000u64 {
            let mut rng = Lcg(seed.wrapping_mul(0x9E3779B97F4A7C15) ^ 0xABCD);
            let m = gen_message(&mut rng);
            let decoded = decode(&encode(&m.addr, &m.args));
            assert_eq!(decoded.as_ref(), Some(&m), "message round-trip failed (seed {seed})");
        }
    }

    #[test]
    fn property_packet_roundtrip() {
        for seed in 0..5000u64 {
            let mut rng = Lcg(seed.wrapping_mul(0x100000001B3) ^ 0x1234);
            let p = gen_packet(&mut rng, 0);
            let decoded = decode_packet(&encode_packet(&p));
            assert_eq!(decoded.as_ref(), Some(&p), "packet round-trip failed (seed {seed})");
        }
    }

    // ── robustness: never panic on arbitrary or truncated input ────────────────

    #[test]
    fn fuzz_decode_never_panics() {
        let mut rng = Lcg(0xF022);
        for _ in 0..200_000 {
            let len = rng.below(80) as usize;
            let mut bytes: Vec<u8> = (0..len).map(|_| rng.next() as u8).collect();
            // Bias a fraction toward the bundle path to exercise that branch.
            if rng.below(4) == 0 && bytes.len() >= 8 {
                bytes[..8].copy_from_slice(b"#bundle\0");
            }
            let _ = decode(&bytes);
            let _ = decode_packet(&bytes);
        }
    }

    #[test]
    fn fuzz_truncations_never_panic() {
        for seed in 0..2000u64 {
            let mut rng = Lcg(seed ^ 0x5151);
            let p = gen_packet(&mut rng, 0);
            let bytes = encode_packet(&p);
            for cut in 0..=bytes.len() {
                let _ = decode(&bytes[..cut]);
                let _ = decode_packet(&bytes[..cut]);
            }
        }
    }

    // ── depth caps: pathological nesting returns None, never overflows ─────────

    #[test]
    fn rejects_overdeep_bundle() {
        // Nest bundles past MAX_DEPTH; decode must reject, not stack-overflow.
        let mut p = OscPacket::Message(OscMessage { addr: "/x".into(), args: vec![] });
        for _ in 0..(MAX_DEPTH + 5) {
            p = OscPacket::Bundle(OscBundle { timetag: 0, elements: vec![p] });
        }
        assert!(decode_packet(&encode_packet(&p)).is_none());
    }

    #[test]
    fn rejects_overdeep_array() {
        let mut a = OscArg::Int(1);
        for _ in 0..(MAX_DEPTH + 5) {
            a = OscArg::Array(vec![a]);
        }
        assert!(decode(&encode("/x", &[a])).is_none());
    }

    #[test]
    fn accepts_nesting_within_cap() {
        let mut p = OscPacket::Message(OscMessage {
            addr: "/x".into(),
            args: vec![OscArg::Int(7)],
        });
        for _ in 0..10 {
            p = OscPacket::Bundle(OscBundle { timetag: 3, elements: vec![p] });
        }
        assert_eq!(decode_packet(&encode_packet(&p)).as_ref(), Some(&p));
    }
}
