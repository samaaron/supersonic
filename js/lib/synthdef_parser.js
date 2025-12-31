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
  // Format: "SCgf" (4) + version (4) + numDefs (2) + [nameLen (1) + name (n) + ...]
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

  // Name starts at offset 10 (after magic + version + numDefs)
  const nameLen = bytes[10];
  if (nameLen === 0 || 11 + nameLen > bytes.length) return null;

  try {
    return new TextDecoder().decode(bytes.slice(11, 11 + nameLen));
  } catch {
    return null;
  }
}
