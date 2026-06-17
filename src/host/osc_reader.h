/*
    SuperSonic
    Copyright (c) 2025 Sam Aaron

    Dual-licensed MIT OR GPL-3.0-or-later (see repo LICENSE).

    Minimal OSC 1.0 message reader for the standalone host's control vocabulary
    (/schedule, /osc/send, /sched/flush). Reads address, type tags, and the
    int32/int64/float/double/string/blob arguments those messages use.
    Bounds-checked: a malformed packet leaves ok() false and every read fails
    rather than reading out of range. No allocation.
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ss_host {

inline uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline uint64_t be64(const uint8_t* p) {
    return (uint64_t(be32(p)) << 32) | uint64_t(be32(p + 4));
}
inline size_t pad4(size_t n) { return (n + 3u) & ~size_t(3); }

class OscReader {
public:
    OscReader(const uint8_t* data, size_t len) : mData(data), mLen(len) { parseHeader(); }

    bool        ok() const { return mOk; }
    const char* address() const { return mAddr; }
    // Next type tag char to be read, or 0 when the arguments are exhausted.
    char        peekType() const { return mTypeIdx < mTypeLen ? mTypes[mTypeIdx] : 0; }

    bool readInt32(int32_t& out)  { return readScalar('i', 4, &out); }
    bool readInt64(int64_t& out)  { return readScalar('h', 8, &out); }
    bool readFloat(float& out)    { return readScalar('f', 4, &out); }
    bool readDouble(double& out)  { return readScalar('d', 8, &out); }

    bool readString(const char*& out) {
        if (!mOk || peekType() != 's') return false;
        size_t start = mArgPos;
        size_t end = start;
        while (end < mLen && mData[end] != 0) ++end;
        if (end >= mLen) return false;                  // unterminated
        out = reinterpret_cast<const char*>(mData + start);
        mArgPos = start + pad4((end - start) + 1);
        if (mArgPos > mLen) return false;
        ++mTypeIdx;
        return true;
    }

    bool readBlob(const uint8_t*& out, uint32_t& size) {
        if (!mOk || peekType() != 'b') return false;
        if (mArgPos + 4 > mLen) return false;
        uint32_t n = be32(mData + mArgPos);
        size_t body = mArgPos + 4;
        if (body + n > mLen) return false;
        out = mData + body;
        size = n;
        mArgPos = body + pad4(n);
        if (mArgPos > mLen) return false;
        ++mTypeIdx;
        return true;
    }

    // Skip the current argument (any supported type). False if unsupported/empty.
    bool skip() {
        switch (peekType()) {
            case 'i': { int32_t v; return readInt32(v); }
            case 'h': { int64_t v; return readInt64(v); }
            case 'f': { float v;   return readFloat(v); }
            case 'd': { double v;  return readDouble(v); }
            case 's': { const char* s; return readString(s); }
            case 'b': { const uint8_t* b; uint32_t n; return readBlob(b, n); }
            default:  return false;
        }
    }

private:
    template <typename T>
    bool readScalar(char type, size_t bytes, T* out) {
        if (!mOk || peekType() != type) return false;
        if (mArgPos + bytes > mLen) return false;
        if (bytes == 4) {
            uint32_t raw = be32(mData + mArgPos);
            std::memcpy(out, &raw, 4);
        } else {
            uint64_t raw = be64(mData + mArgPos);
            std::memcpy(out, &raw, 8);
        }
        mArgPos += bytes;
        ++mTypeIdx;
        return true;
    }

    void parseHeader() {
        // Address: NUL-terminated, 4-byte padded.
        size_t a = 0;
        while (a < mLen && mData[a] != 0) ++a;
        if (a >= mLen) return;
        mAddr = reinterpret_cast<const char*>(mData);
        size_t typesOff = pad4(a + 1);
        if (typesOff >= mLen || mData[typesOff] != ',') return;

        // Type tags: starts with ',', NUL-terminated, 4-byte padded.
        size_t t = typesOff;
        while (t < mLen && mData[t] != 0) ++t;
        if (t >= mLen) return;
        mTypes   = reinterpret_cast<const char*>(mData + typesOff + 1);  // skip ','
        mTypeLen = (t - typesOff) - 1;
        mArgPos  = typesOff + pad4((t - typesOff) + 1);
        if (mArgPos > mLen) return;
        mOk = true;
    }

    const uint8_t* mData;
    size_t         mLen;
    bool           mOk      = false;
    const char*    mAddr    = "";
    const char*    mTypes   = "";
    size_t         mTypeLen = 0;
    size_t         mTypeIdx = 0;
    size_t         mArgPos  = 0;
};

}  // namespace ss_host
