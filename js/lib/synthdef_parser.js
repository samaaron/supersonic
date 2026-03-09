// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * Extract synthdef name from path or binary data
 *
 * Handles two input types:
 * - String path: extracts filename without .scsyndef extension
 * - Binary data: parses SCgf format header to extract name
 *
 * @param {string|Uint8Array|ArrayBuffer} input - File path or scsyndef binary
 * @returns {string|null} Synthdef name or null if not found
 *
 * @example
 * // From path
 * extractSynthDefName('/path/to/sonic-pi-beep.scsyndef') // => 'sonic-pi-beep'
 *
 * // From binary (SCgf format)
 * extractSynthDefName(synthdefBytes) // => 'sonic-pi-beep'
 */
export function extractSynthDefName(input) {
  if (!input) return null;

  // Handle string path - extract from filename
  if (typeof input === "string") {
    const lastSegment = input.split("/").filter(Boolean).pop() || input;
    return lastSegment.replace(/\.scsyndef$/i, "");
  }

  // Handle binary data - parse scsyndef format
  // v0/v1/v2: "SCgf" (4) + version (4) + numDefs (2) + nameLen (1) + name (n) + ...
  // v3:       "SCgf" (4) + version (4) + numDefs (2) + defSize (4) + nameLen (1) + name (n) + ...
  const bytes = input instanceof ArrayBuffer ? new Uint8Array(input) : input;
  if (!(bytes instanceof Uint8Array) || bytes.length < 11) return null;

  // Check magic "SCgf"
  if (
    bytes[0] !== 0x53 ||
    bytes[1] !== 0x43 ||
    bytes[2] !== 0x67 ||
    bytes[3] !== 0x66
  ) {
    return null;
  }

  // Read version (big-endian int32 at offset 4)
  const version = (bytes[4] << 24) | (bytes[5] << 16) | (bytes[6] << 8) | bytes[7];

  // v3 has a 4-byte size field before each definition
  const nameOffset = version >= 3 ? 14 : 10;
  if (nameOffset >= bytes.length) return null;

  const nameLen = bytes[nameOffset];
  if (nameLen === 0 || nameOffset + 1 + nameLen > bytes.length) return null;

  try {
    return new TextDecoder().decode(bytes.slice(nameOffset + 1, nameOffset + 1 + nameLen));
  } catch {
    return null;
  }
}
