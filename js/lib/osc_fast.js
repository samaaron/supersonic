// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron
//
// Fast OSC encoder/decoder with zero-allocation design.
// Based on Sonic Pi's Ruby/Erlang OSC implementations.
// See https://opensoundcontrol.stanford.edu/ for OSC spec.

// Pre-allocated encode buffer (2MB - handles audio files and synthdefs)
const BUFFER_SIZE = 2 * 1024 * 1024;
const mainBuffer = new Uint8Array(BUFFER_SIZE);
const mainView = new DataView(mainBuffer.buffer);

// Active buffer (usually mainBuffer, temporary allocation for huge payloads)
let encodeBuffer = mainBuffer;
let encodeView = mainView;

// String cache for addresses (they repeat constantly in Sonic Pi workloads)
// e.g., "/s_new", "/n_set", "/n_free" are called thousands of times
const stringCache = new Map();
const STRING_CACHE_MAX = 1000;

// Text decoder/encoder for UTF-8 strings
const textDecoder = new TextDecoder();
const textEncoder = new TextEncoder();

// NTP epoch offset (seconds from 1900 to 1970)
export const NTP_EPOCH_OFFSET = 2208988800;
export const TWO_POW_32 = 4294967296;

// Bundle header "#bundle\0" as bytes (pre-computed)
const BUNDLE_HEADER = new Uint8Array([0x23, 0x62, 0x75, 0x6e, 0x64, 0x6c, 0x65, 0x00]);

// Type tag bytes (pre-computed to avoid charCodeAt calls)
const TAG_COMMA = 0x2C;  // ','
const TAG_INT = 0x69;    // 'i'
const TAG_FLOAT = 0x66;  // 'f'
const TAG_STRING = 0x73; // 's'
const TAG_BLOB = 0x62;   // 'b'
const TAG_TRUE = 0x54;   // 'T'
const TAG_FALSE = 0x46;  // 'F'
const TAG_INT64 = 0x68;  // 'h'
const TAG_DOUBLE = 0x64; // 'd'
const TAG_TIMETAG = 0x74; // 't'

// ============================================================================
// ENCODING
// ============================================================================

/**
 * Estimate the encoded size of a message (conservative upper bound).
 * @private
 */
function estimateMessageSize(address, args) {
  // Address: length + null + padding (up to 3)
  let size = address.length + 4;
  // Type tags: comma + tags + null + padding
  size += args.length + 4;
  // Arguments
  for (const arg of args) {
    if (arg instanceof Uint8Array) {
      size += 4 + arg.length + 3; // size + data + padding
    } else if (arg instanceof ArrayBuffer) {
      size += 4 + arg.byteLength + 3;
    } else if (typeof arg === 'string') {
      size += arg.length * 3 + 4; // UTF-8 worst case + null + padding
    } else if (arg && arg.type === 'string') {
      size += arg.value.length * 3 + 4; // tagged string: UTF-8 worst case + null + padding
    } else if (arg && arg.type === 'blob') {
      const blobVal = arg.value;
      const blobLen = blobVal instanceof Uint8Array ? blobVal.length : blobVal.byteLength;
      size += 4 + blobLen + 3; // size + data + padding
    } else {
      size += 8; // numbers, booleans, etc.
    }
  }
  return size;
}

/**
 * Estimate the encoded size of a bundle (conservative upper bound).
 * @private
 */
function estimateBundleSize(packets) {
  // Header "#bundle\0" + timetag
  let size = 16;
  for (const packet of packets) {
    size += 4; // size prefix
    if (Array.isArray(packet)) {
      size += estimateMessageSize(packet[0], packet.slice(1));
    } else if (packet.packets !== undefined) {
      size += estimateBundleSize(packet.packets);
    } else {
      size += estimateMessageSize(packet.address, packet.args || []);
    }
  }
  return size;
}

/**
 * Ensure encode buffer is large enough.
 * Uses main 2MB buffer for most cases, allocates temporary for huge payloads.
 * @private
 */
function ensureBufferSize(estimatedSize) {
  if (estimatedSize <= BUFFER_SIZE) {
    // Use main buffer (zero allocation path)
    encodeBuffer = mainBuffer;
    encodeView = mainView;
    return;
  }
  // Rare: huge payload - allocate temporary (will be GC'd)
  encodeBuffer = new Uint8Array(estimatedSize);
  encodeView = new DataView(encodeBuffer.buffer);
}

/**
 * Encode an OSC message.
 * Returns a view into the shared encode buffer - copy with copyEncoded() if
 * you need to keep it beyond the next encode call.
 *
 * @param {string} address - OSC address (e.g., "/s_new")
 * @param {Array} args - Arguments array
 * @returns {Uint8Array} - Encoded message (view into shared buffer)
 */
export function encodeMessage(address, args = []) {
  const estimated = estimateMessageSize(address, args);
  ensureBufferSize(estimated);

  let pos = 0;

  // Write address (cached)
  pos = writeStringCached(address, pos);

  // Write type tags
  pos = writeTypeTags(args, pos);

  // Write arguments
  for (let i = 0; i < args.length; i++) {
    pos = writeArg(args[i], pos);
  }

  // Return a view of the used portion
  return encodeBuffer.subarray(0, pos);
}

/**
 * Encode an OSC bundle.
 * Returns a view into the shared encode buffer.
 *
 * @param {number} timeTag - NTP timestamp (seconds since 1900) or 1 for immediate
 * @param {Array} packets - Array of {address, args} objects or nested bundles
 * @returns {Uint8Array} - Encoded bundle (view into shared buffer)
 */
export function encodeBundle(timeTag, packets) {
  const estimated = estimateBundleSize(packets);
  ensureBufferSize(estimated);

  let pos = 0;

  // Write "#bundle\0"
  encodeBuffer.set(BUNDLE_HEADER, pos);
  pos += 8;

  // Write timetag
  pos = writeTimeTag(timeTag, pos);

  // Write each packet with size prefix
  for (let i = 0; i < packets.length; i++) {
    const packet = packets[i];

    // Reserve space for size (4 bytes)
    const sizePos = pos;
    pos += 4;

    const packetStart = pos;

    if (Array.isArray(packet)) {
      // Message as [address, ...args]
      pos = encodeMessageInto(packet[0], packet.slice(1), pos);
    } else if (packet.packets !== undefined) {
      // Nested bundle
      pos = encodeBundleInto(packet.timeTag, packet.packets, pos);
    } else {
      // Legacy object format { address, args }
      pos = encodeMessageInto(packet.address, packet.args || [], pos);
    }

    // Write packet size
    const packetSize = pos - packetStart;
    encodeView.setUint32(sizePos, packetSize, false);
  }

  return encodeBuffer.subarray(0, pos);
}

/**
 * Encode a single-message bundle (common case optimization).
 *
 * @param {number} timeTag - NTP timestamp
 * @param {string} address - OSC address
 * @param {Array} args - Arguments
 * @returns {Uint8Array} - Encoded bundle
 */
export function encodeSingleBundle(timeTag, address, args = []) {
  const estimated = 16 + 4 + estimateMessageSize(address, args); // header + timetag + size + message
  ensureBufferSize(estimated);

  let pos = 0;

  // Write "#bundle\0"
  encodeBuffer.set(BUNDLE_HEADER, pos);
  pos += 8;

  // Write timetag
  pos = writeTimeTag(timeTag, pos);

  // Reserve space for message size
  const sizePos = pos;
  pos += 4;

  const messageStart = pos;

  // Write message inline
  pos = writeStringCached(address, pos);
  pos = writeTypeTags(args, pos);
  for (let i = 0; i < args.length; i++) {
    pos = writeArg(args[i], pos);
  }

  // Write message size
  encodeView.setUint32(sizePos, pos - messageStart, false);

  return encodeBuffer.subarray(0, pos);
}

/**
 * Encode message directly into buffer at offset (for bundle building).
 * @private
 */
function encodeMessageInto(address, args, pos) {
  pos = writeStringCached(address, pos);
  pos = writeTypeTags(args, pos);
  for (let i = 0; i < args.length; i++) {
    pos = writeArg(args[i], pos);
  }
  return pos;
}

/**
 * Encode bundle directly into buffer at offset (for nested bundles).
 * @private
 */
function encodeBundleInto(timeTag, packets, pos) {
  encodeBuffer.set(BUNDLE_HEADER, pos);
  pos += 8;
  pos = writeTimeTag(timeTag, pos);

  for (let i = 0; i < packets.length; i++) {
    const packet = packets[i];
    const sizePos = pos;
    pos += 4;
    const packetStart = pos;

    if (Array.isArray(packet)) {
      pos = encodeMessageInto(packet[0], packet.slice(1), pos);
    } else if (packet.packets !== undefined) {
      pos = encodeBundleInto(packet.timeTag, packet.packets, pos);
    } else {
      pos = encodeMessageInto(packet.address, packet.args || [], pos);
    }

    encodeView.setUint32(sizePos, pos - packetStart, false);
  }

  return pos;
}

/**
 * Write a string with caching (addresses repeat constantly).
 * @private
 */
function writeStringCached(str, pos) {
  const cached = stringCache.get(str);

  if (cached) {
    encodeBuffer.set(cached, pos);
    return pos + cached.length;
  }

  // Encode and cache
  const startPos = pos;
  pos = writeString(str, pos);

  if (stringCache.size < STRING_CACHE_MAX) {
    // Cache a copy of the encoded bytes
    const encoded = encodeBuffer.slice(startPos, pos);
    stringCache.set(str, encoded);
  }

  return pos;
}

/**
 * Write a string (null-terminated, padded to 4 bytes).
 * @private
 */
function writeString(str, pos) {
  // Check for non-ASCII: scan for any char >= 128
  let needsUTF8 = false;
  for (let i = 0; i < str.length; i++) {
    if (str.charCodeAt(i) >= 128) {
      needsUTF8 = true;
      break;
    }
  }

  if (needsUTF8) {
    // UTF-8 path using TextEncoder
    const result = textEncoder.encodeInto(str, encodeBuffer.subarray(pos));
    pos += result.written;
  } else {
    // ASCII fast path
    for (let i = 0; i < str.length; i++) {
      encodeBuffer[pos++] = str.charCodeAt(i);
    }
  }

  // Null terminator
  encodeBuffer[pos++] = 0;

  // Pad to 4-byte boundary
  while (pos & 3) {
    encodeBuffer[pos++] = 0;
  }

  return pos;
}

/**
 * Write type tags for arguments.
 * @private
 */
function writeTypeTags(args, pos) {
  encodeBuffer[pos++] = TAG_COMMA;

  for (let i = 0; i < args.length; i++) {
    const arg = args[i];
    const type = typeof arg;

    if (type === 'number') {
      encodeBuffer[pos++] = Number.isInteger(arg) ? TAG_INT : TAG_FLOAT;
    } else if (type === 'string') {
      encodeBuffer[pos++] = TAG_STRING;
    } else if (type === 'boolean') {
      encodeBuffer[pos++] = arg ? TAG_TRUE : TAG_FALSE;
    } else if (arg instanceof Uint8Array || arg instanceof ArrayBuffer) {
      encodeBuffer[pos++] = TAG_BLOB;
    } else if (arg && arg.type === 'int') {
      encodeBuffer[pos++] = TAG_INT;
    } else if (arg && arg.type === 'float') {
      encodeBuffer[pos++] = TAG_FLOAT;
    } else if (arg && arg.type === 'string') {
      encodeBuffer[pos++] = TAG_STRING;
    } else if (arg && arg.type === 'blob') {
      encodeBuffer[pos++] = TAG_BLOB;
    } else if (arg && arg.type === 'bool') {
      encodeBuffer[pos++] = arg.value ? TAG_TRUE : TAG_FALSE;
    } else if (arg && arg.type === 'int64') {
      encodeBuffer[pos++] = TAG_INT64;
    } else if (arg && arg.type === 'double') {
      encodeBuffer[pos++] = TAG_DOUBLE;
    } else if (arg && arg.type === 'timetag') {
      encodeBuffer[pos++] = TAG_TIMETAG;
    } else if (arg === null || arg === undefined) {
      throw new Error(`OSC argument at index ${i} is ${arg}`);
    } else {
      throw new Error(`Unknown OSC argument type at index ${i}: ${type}`);
    }
  }

  // Null terminator
  encodeBuffer[pos++] = 0;

  // Pad to 4-byte boundary
  while (pos & 3) {
    encodeBuffer[pos++] = 0;
  }

  return pos;
}

/**
 * Write a single argument.
 * @private
 */
function writeArg(arg, pos) {
  const type = typeof arg;

  if (type === 'number') {
    if (Number.isInteger(arg)) {
      encodeView.setInt32(pos, arg, false); // big-endian
      return pos + 4;
    } else {
      encodeView.setFloat32(pos, arg, false);
      return pos + 4;
    }
  }

  if (type === 'string') {
    return writeString(arg, pos);
  }

  if (type === 'boolean') {
    // T/F have no data, just type tag (already written)
    return pos;
  }

  if (arg instanceof Uint8Array) {
    // Blob: size + data + padding
    const size = arg.length;
    encodeView.setUint32(pos, size, false);
    pos += 4;
    encodeBuffer.set(arg, pos);
    pos += size;
    // Pad to 4-byte boundary
    while (pos & 3) {
      encodeBuffer[pos++] = 0;
    }
    return pos;
  }

  if (arg instanceof ArrayBuffer) {
    return writeArg(new Uint8Array(arg), pos);
  }

  if (arg && arg.type === 'int') {
    encodeView.setInt32(pos, arg.value, false);
    return pos + 4;
  }

  if (arg && arg.type === 'float') {
    encodeView.setFloat32(pos, arg.value, false);
    return pos + 4;
  }

  if (arg && arg.type === 'string') {
    return writeString(arg.value, pos);
  }

  if (arg && arg.type === 'blob') {
    const blobVal = arg.value instanceof Uint8Array ? arg.value : new Uint8Array(arg.value);
    const size = blobVal.length;
    encodeView.setUint32(pos, size, false);
    pos += 4;
    encodeBuffer.set(blobVal, pos);
    pos += size;
    while (pos & 3) {
      encodeBuffer[pos++] = 0;
    }
    return pos;
  }

  if (arg && arg.type === 'bool') {
    // T/F have no data, just type tag (already written)
    return pos;
  }

  if (arg && arg.type === 'int64') {
    // 64-bit big-endian signed integer
    encodeView.setBigInt64(pos, BigInt(arg.value), false);
    return pos + 8;
  }

  if (arg && arg.type === 'double') {
    encodeView.setFloat64(pos, arg.value, false);
    return pos + 8;
  }

  if (arg && arg.type === 'timetag') {
    return writeTimeTag(arg.value, pos);
  }

  // Unknown type - skip
  return pos;
}

/**
 * Write NTP timetag (8 bytes).
 *
 * Accepted formats:
 * - `1`, `null`, `undefined` → immediate (0x0000000000000001)
 * - `[uint32, uint32]` → write seconds/fraction pair directly
 * - Number → existing NTP float path
 *
 * @private
 */
function writeTimeTag(time, pos) {
  if (time === 1 || time === null || time === undefined) {
    // Immediate: timetag = 0x0000000000000001
    encodeView.setUint32(pos, 0, false);
    encodeView.setUint32(pos + 4, 1, false);
    return pos + 8;
  }

  if (Array.isArray(time)) {
    if (time.length !== 2) {
      throw new Error(`TimeTag array must have exactly 2 elements [seconds, fraction], got ${time.length}`);
    }
    encodeView.setUint32(pos, time[0] >>> 0, false);
    encodeView.setUint32(pos + 4, time[1] >>> 0, false);
    return pos + 8;
  }

  if (typeof time !== 'number') {
    throw new TypeError(`TimeTag must be a number, array, null, or undefined, got ${typeof time}`);
  }

  if (time > 1 && time < NTP_EPOCH_OFFSET) {
    console.warn(`TimeTag ${time} looks like a Unix timestamp (< NTP_EPOCH_OFFSET). Did you mean to add NTP_EPOCH_OFFSET (2208988800)?`);
  }

  // time is already in NTP seconds (seconds since 1900)
  const seconds = time >>> 0; // Unsigned 32-bit
  const fraction = ((time - Math.floor(time)) * TWO_POW_32) >>> 0;

  encodeView.setUint32(pos, seconds, false);
  encodeView.setUint32(pos + 4, fraction, false);

  return pos + 8;
}

// ============================================================================
// DECODING
// ============================================================================

/**
 * Decode an OSC packet (message or bundle).
 *
 * @param {Uint8Array|ArrayBuffer} data - Raw OSC data
 * @returns {Object} - Decoded packet
 */
export function decodePacket(data) {
  if (!(data instanceof Uint8Array)) {
    data = new Uint8Array(data);
  }

  // Check if bundle (starts with "#bundle")
  if (data[0] === 0x23 && data[1] === 0x62) {
    return decodeBundle(data);
  }

  return decodeMessage(data);
}

/**
 * Decode an OSC message.
 *
 * @param {Uint8Array} data - Raw OSC message data
 * @returns {Array} - [address, ...args]
 */
export function decodeMessage(data) {
  if (!(data instanceof Uint8Array)) {
    data = new Uint8Array(data);
  }

  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  let pos = 0;

  // Read address
  const [address, addrEnd] = readString(data, pos);
  pos = addrEnd;

  // Check for type tags
  if (pos >= data.length || data[pos] !== TAG_COMMA) {
    return [address];
  }

  // Read type tags
  const [tags, tagsEnd] = readString(data, pos);
  pos = tagsEnd;

  // Parse args based on tags (skip the leading ',')
  const result = [address];
  for (let i = 1; i < tags.length; i++) {
    const tag = tags.charCodeAt(i);

    switch (tag) {
      case TAG_INT: // 'i' - int32
        result.push(view.getInt32(pos, false));
        pos += 4;
        break;

      case TAG_FLOAT: // 'f' - float32
        result.push(view.getFloat32(pos, false));
        pos += 4;
        break;

      case TAG_STRING: // 's' - string
        const [str, strEnd] = readString(data, pos);
        result.push(str);
        pos = strEnd;
        break;

      case TAG_BLOB: // 'b' - blob
        const blobSize = view.getUint32(pos, false);
        pos += 4;
        result.push(data.slice(pos, pos + blobSize));
        pos += blobSize;
        pos = (pos + 3) & ~3; // Pad to 4
        break;

      case TAG_INT64: // 'h' - int64
        result.push(view.getBigInt64(pos, false));
        pos += 8;
        break;

      case TAG_DOUBLE: // 'd' - double
        result.push(view.getFloat64(pos, false));
        pos += 8;
        break;

      case TAG_TRUE: // 'T' - true
        result.push(true);
        break;

      case TAG_FALSE: // 'F' - false
        result.push(false);
        break;

      case TAG_TIMETAG: // 't' - timetag
        const seconds = view.getUint32(pos, false);
        const fraction = view.getUint32(pos + 4, false);
        result.push(seconds + fraction / TWO_POW_32);
        pos += 8;
        break;
    }
  }

  return result;
}

/**
 * Decode an OSC bundle.
 *
 * @param {Uint8Array} data - Raw OSC bundle data
 * @returns {Object} - {timeTag, packets}
 */
export function decodeBundle(data) {
  if (!(data instanceof Uint8Array)) {
    data = new Uint8Array(data);
  }

  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  let pos = 8; // Skip "#bundle\0"

  // Read timetag
  const seconds = view.getUint32(pos, false);
  const fraction = view.getUint32(pos + 4, false);
  const timeTag = seconds + fraction / TWO_POW_32;
  pos += 8;

  // Read packets
  const packets = [];
  while (pos < data.length) {
    const packetSize = view.getUint32(pos, false);
    pos += 4;

    if (packetSize > 0 && pos + packetSize <= data.length) {
      const packetData = data.subarray(pos, pos + packetSize);
      packets.push(decodePacket(packetData));
    }

    pos += packetSize;
  }

  return { timeTag, packets };
}

/**
 * Read a null-terminated string with padding.
 * @private
 */
function readString(data, pos) {
  let end = pos;
  while (end < data.length && data[end] !== 0) {
    end++;
  }

  const str = textDecoder.decode(data.subarray(pos, end));

  // Skip null and padding to 4-byte boundary
  end++;
  end = (end + 3) & ~3;

  return [str, end];
}

// ============================================================================
// UTILITIES
// ============================================================================

/**
 * Create a copy of encoded data.
 * Use this when you need to keep the encoded data beyond the next encode call.
 *
 * @param {Uint8Array} encoded - Encoded data from encodeMessage/encodeBundle
 * @returns {Uint8Array} - Independent copy
 */
export function copyEncoded(encoded) {
  return encoded.slice();
}

/**
 * Encode directly into a target buffer (for ring buffer writing).
 * Avoids the copy when you're going to copy anyway.
 *
 * @param {string} address - OSC address
 * @param {Array} args - Arguments
 * @param {Uint8Array} target - Target buffer
 * @param {number} offset - Offset in target buffer
 * @returns {number} - Bytes written
 */
export function encodeMessageIntoBuffer(address, args, target, offset) {
  // Encode to our buffer first
  const encoded = encodeMessage(address, args);

  // Copy to target
  target.set(encoded, offset);

  return encoded.length;
}

/**
 * Encode bundle directly into a target buffer.
 *
 * @param {number} timeTag - NTP timestamp
 * @param {Array} packets - Packets
 * @param {Uint8Array} target - Target buffer
 * @param {number} offset - Offset in target buffer
 * @returns {number} - Bytes written
 */
export function encodeBundleIntoBuffer(timeTag, packets, target, offset) {
  const encoded = encodeBundle(timeTag, packets);
  target.set(encoded, offset);
  return encoded.length;
}

/**
 * Clear the string cache (if memory is a concern).
 */
export function clearCache() {
  stringCache.clear();
}

/**
 * Get cache statistics.
 */
export function getCacheStats() {
  return {
    stringCacheSize: stringCache.size,
    maxSize: STRING_CACHE_MAX,
  };
}

/**
 * Check if data is an OSC bundle.
 */
export function isBundle(data) {
  if (!data || data.length < 8) return false;
  return data[0] === 0x23 && data[1] === 0x62; // "#b"
}

/**
 * Extract timetag from bundle without full decode.
 */
export function getBundleTimeTag(data) {
  if (!isBundle(data)) return null;
  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  const seconds = view.getUint32(8, false);
  const fraction = view.getUint32(12, false);
  return seconds + fraction / TWO_POW_32;
}

