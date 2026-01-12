// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * AIFF to WAV Converter
 *
 * Converts AIFF/AIFC audio files to WAV format in-memory.
 * This is needed because Web Audio API's decodeAudioData() doesn't support AIFF.
 *
 * AIFF and WAV both contain uncompressed PCM data - only the header format
 * and byte order differ (AIFF is big-endian, WAV is little-endian).
 */

/**
 * Check if an ArrayBuffer contains AIFF data
 * @param {ArrayBuffer} buffer - The audio file data
 * @returns {boolean} True if the buffer contains AIFF/AIFC data
 */
export function isAiff(buffer) {
    if (buffer.byteLength < 12) {
        return false;
    }

    const view = new Uint8Array(buffer, 0, 12);
    const magic = String.fromCharCode(view[0], view[1], view[2], view[3]);
    const formType = String.fromCharCode(view[8], view[9], view[10], view[11]);

    return magic === 'FORM' && (formType === 'AIFF' || formType === 'AIFC');
}

/**
 * Parse 80-bit IEEE 754 extended precision float (used for AIFF sample rate)
 * @param {Uint8Array} bytes - 10 bytes representing the 80-bit float
 * @returns {number} The parsed floating point value
 */
function parseFloat80(bytes) {
    const sign = (bytes[0] >> 7) & 1;
    const exponent = ((bytes[0] & 0x7F) << 8) | bytes[1];

    // Build mantissa from bytes 2-9 (64 bits)
    let mantissa = 0;
    for (let i = 2; i < 10; i++) {
        mantissa = mantissa * 256 + bytes[i];
    }

    if (exponent === 0) {
        return 0;
    }

    // 80-bit extended has explicit integer bit, bias is 16383
    const value = mantissa * Math.pow(2, exponent - 16383 - 63);
    return sign ? -value : value;
}

/**
 * Find a chunk in AIFF data by its 4-character ID
 * @param {DataView} view - DataView of the AIFF file
 * @param {string} chunkId - 4-character chunk ID to find
 * @returns {{offset: number, size: number} | null} Chunk location or null if not found
 */
function findChunk(view, chunkId) {
    let offset = 12; // Skip FORM header (4 + 4 + 4 bytes)

    while (offset < view.byteLength - 8) {
        const id = String.fromCharCode(
            view.getUint8(offset),
            view.getUint8(offset + 1),
            view.getUint8(offset + 2),
            view.getUint8(offset + 3)
        );
        const size = view.getUint32(offset + 4, false); // big-endian

        if (id === chunkId) {
            return { offset: offset + 8, size };
        }

        // Move to next chunk (chunks are word-aligned, so pad odd sizes)
        offset += 8 + size + (size % 2);
    }

    return null;
}

/**
 * Convert AIFF/AIFC audio data to WAV format
 * @param {ArrayBuffer} aiffBuffer - The AIFF file data
 * @returns {ArrayBuffer} WAV file data
 * @throws {Error} If the AIFF file is malformed or uses unsupported compression
 */
export function aiffToWav(aiffBuffer) {
    const view = new DataView(aiffBuffer);

    // Check for AIFC (compressed) vs AIFF (uncompressed)
    const formType = String.fromCharCode(
        view.getUint8(8), view.getUint8(9), view.getUint8(10), view.getUint8(11)
    );

    // Find COMM chunk (required)
    const commChunk = findChunk(view, 'COMM');
    if (!commChunk) {
        throw new Error('AIFF file missing COMM chunk');
    }

    // Parse COMM chunk
    const numChannels = view.getUint16(commChunk.offset, false); // big-endian
    const numSampleFrames = view.getUint32(commChunk.offset + 2, false);
    const bitsPerSample = view.getUint16(commChunk.offset + 6, false);

    // Parse 80-bit sample rate
    const sampleRateBytes = new Uint8Array(aiffBuffer, commChunk.offset + 8, 10);
    const sampleRate = parseFloat80(sampleRateBytes);

    // For AIFC, check compression type
    if (formType === 'AIFC') {
        // Compression type is at offset 18 in COMM chunk (4 bytes)
        if (commChunk.size >= 22) {
            const compressionType = String.fromCharCode(
                view.getUint8(commChunk.offset + 18),
                view.getUint8(commChunk.offset + 19),
                view.getUint8(commChunk.offset + 20),
                view.getUint8(commChunk.offset + 21)
            );
            // NONE and sowt (little-endian) are uncompressed
            if (compressionType !== 'NONE' && compressionType !== 'sowt') {
                throw new Error(`AIFC compression type '${compressionType}' is not supported. Only uncompressed AIFF/AIFC files are supported.`);
            }
            // sowt is already little-endian, no byte swap needed
            if (compressionType === 'sowt') {
                return aiffSowtToWav(aiffBuffer, numChannels, numSampleFrames, bitsPerSample, sampleRate);
            }
        }
    }

    // Find SSND chunk (required)
    const ssndChunk = findChunk(view, 'SSND');
    if (!ssndChunk) {
        throw new Error('AIFF file missing SSND chunk');
    }

    // SSND has 8 bytes of offset/blockSize before audio data
    const ssndOffset = view.getUint32(ssndChunk.offset, false);
    const audioDataStart = ssndChunk.offset + 8 + ssndOffset;
    const bytesPerSample = bitsPerSample / 8;
    const audioDataSize = numSampleFrames * numChannels * bytesPerSample;

    // Validate we have enough data
    if (audioDataStart + audioDataSize > aiffBuffer.byteLength) {
        throw new Error('AIFF file truncated: not enough audio data');
    }

    // Create WAV file
    const wavHeaderSize = 44;
    const wavBuffer = new ArrayBuffer(wavHeaderSize + audioDataSize);
    const wavView = new DataView(wavBuffer);
    const wavBytes = new Uint8Array(wavBuffer);

    // Write WAV header
    writeWavHeader(wavView, {
        numChannels,
        sampleRate: Math.round(sampleRate),
        bitsPerSample,
        dataSize: audioDataSize
    });

    // Copy and byte-swap PCM data
    const srcBytes = new Uint8Array(aiffBuffer, audioDataStart, audioDataSize);

    if (bytesPerSample === 1) {
        // 8-bit: no swap needed, but AIFF uses signed, WAV uses unsigned
        for (let i = 0; i < audioDataSize; i++) {
            wavBytes[wavHeaderSize + i] = srcBytes[i] + 128;
        }
    } else if (bytesPerSample === 2) {
        // 16-bit: swap byte pairs
        for (let i = 0; i < audioDataSize; i += 2) {
            wavBytes[wavHeaderSize + i] = srcBytes[i + 1];
            wavBytes[wavHeaderSize + i + 1] = srcBytes[i];
        }
    } else if (bytesPerSample === 3) {
        // 24-bit: swap byte triplets
        for (let i = 0; i < audioDataSize; i += 3) {
            wavBytes[wavHeaderSize + i] = srcBytes[i + 2];
            wavBytes[wavHeaderSize + i + 1] = srcBytes[i + 1];
            wavBytes[wavHeaderSize + i + 2] = srcBytes[i];
        }
    } else if (bytesPerSample === 4) {
        // 32-bit: swap 4 bytes
        for (let i = 0; i < audioDataSize; i += 4) {
            wavBytes[wavHeaderSize + i] = srcBytes[i + 3];
            wavBytes[wavHeaderSize + i + 1] = srcBytes[i + 2];
            wavBytes[wavHeaderSize + i + 2] = srcBytes[i + 1];
            wavBytes[wavHeaderSize + i + 3] = srcBytes[i];
        }
    } else {
        throw new Error(`Unsupported bit depth: ${bitsPerSample}`);
    }

    return wavBuffer;
}

/**
 * Convert AIFC 'sowt' (little-endian) audio data to WAV format
 * No byte swapping needed since sowt is already little-endian
 */
function aiffSowtToWav(aiffBuffer, numChannels, numSampleFrames, bitsPerSample, sampleRate) {
    const view = new DataView(aiffBuffer);

    const ssndChunk = findChunk(view, 'SSND');
    if (!ssndChunk) {
        throw new Error('AIFF file missing SSND chunk');
    }

    const ssndOffset = view.getUint32(ssndChunk.offset, false);
    const audioDataStart = ssndChunk.offset + 8 + ssndOffset;
    const bytesPerSample = bitsPerSample / 8;
    const audioDataSize = numSampleFrames * numChannels * bytesPerSample;

    if (audioDataStart + audioDataSize > aiffBuffer.byteLength) {
        throw new Error('AIFF file truncated: not enough audio data');
    }

    const wavHeaderSize = 44;
    const wavBuffer = new ArrayBuffer(wavHeaderSize + audioDataSize);
    const wavView = new DataView(wavBuffer);
    const wavBytes = new Uint8Array(wavBuffer);

    writeWavHeader(wavView, {
        numChannels,
        sampleRate: Math.round(sampleRate),
        bitsPerSample,
        dataSize: audioDataSize
    });

    // sowt is little-endian, just copy directly
    const srcBytes = new Uint8Array(aiffBuffer, audioDataStart, audioDataSize);

    if (bytesPerSample === 1) {
        // 8-bit: AIFF uses signed, WAV uses unsigned
        for (let i = 0; i < audioDataSize; i++) {
            wavBytes[wavHeaderSize + i] = srcBytes[i] + 128;
        }
    } else {
        // 16/24/32-bit: direct copy (already little-endian)
        wavBytes.set(srcBytes, wavHeaderSize);
    }

    return wavBuffer;
}

/**
 * Write a WAV file header
 * @param {DataView} view - DataView to write to
 * @param {Object} params - Audio parameters
 */
function writeWavHeader(view, { numChannels, sampleRate, bitsPerSample, dataSize }) {
    const byteRate = sampleRate * numChannels * (bitsPerSample / 8);
    const blockAlign = numChannels * (bitsPerSample / 8);

    // RIFF header
    view.setUint8(0, 0x52);  // 'R'
    view.setUint8(1, 0x49);  // 'I'
    view.setUint8(2, 0x46);  // 'F'
    view.setUint8(3, 0x46);  // 'F'
    view.setUint32(4, 36 + dataSize, true);  // file size - 8
    view.setUint8(8, 0x57);  // 'W'
    view.setUint8(9, 0x41);  // 'A'
    view.setUint8(10, 0x56); // 'V'
    view.setUint8(11, 0x45); // 'E'

    // fmt chunk
    view.setUint8(12, 0x66); // 'f'
    view.setUint8(13, 0x6D); // 'm'
    view.setUint8(14, 0x74); // 't'
    view.setUint8(15, 0x20); // ' '
    view.setUint32(16, 16, true);           // fmt chunk size
    view.setUint16(20, 1, true);            // audio format (1 = PCM)
    view.setUint16(22, numChannels, true);
    view.setUint32(24, sampleRate, true);
    view.setUint32(28, byteRate, true);
    view.setUint16(32, blockAlign, true);
    view.setUint16(34, bitsPerSample, true);

    // data chunk
    view.setUint8(36, 0x64); // 'd'
    view.setUint8(37, 0x61); // 'a'
    view.setUint8(38, 0x74); // 't'
    view.setUint8(39, 0x61); // 'a'
    view.setUint32(40, dataSize, true);
}
