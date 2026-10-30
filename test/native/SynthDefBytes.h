// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
// SynthDef files written byte by byte, for the cases that send the reader a
// def it must refuse, and the checks those cases share.
#pragma once

#include "EngineFixture.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace synthdef_bytes {

// A SynthDef file, written field by field.
class Def {
public:
    Def& i8(int8_t v)   { mBytes.push_back(static_cast<uint8_t>(v)); return *this; }
    Def& i16(int16_t v) { for (int k = 1; k >= 0; --k) mBytes.push_back(uint8_t(uint16_t(v) >> (8 * k))); return *this; }
    Def& i32(int32_t v) { for (int k = 3; k >= 0; --k) mBytes.push_back(uint8_t(uint32_t(v) >> (8 * k))); return *this; }
    Def& f32(float v)   { uint32_t u; std::memcpy(&u, &v, 4); return i32(static_cast<int32_t>(u)); }
    Def& str(const char* s) {
        const auto n = std::strlen(s);
        mBytes.push_back(static_cast<uint8_t>(n));
        mBytes.insert(mBytes.end(), s, s + n);
        return *this;
    }
    const std::vector<uint8_t>& bytes() const { return mBytes; }
private:
    std::vector<uint8_t> mBytes;
};

// Sends `def` with /d_recv: true when it is answered with /done, false when
// with /fail.
inline bool loads(EngineFixture& fx, const std::vector<uint8_t>& def) {
    fx.clearReplies();
    fx.send(osc_test::messageWithBlob("/d_recv", def.data(), def.size()));
    OscReply reply;
    if (fx.waitForReply("/done", reply)) return true;
    REQUIRE(fx.waitForReply("/fail", reply));
    return false;
}

// The engine still answers after a refusal.
inline void stillRunning(EngineFixture& fx) {
    fx.send(osc_test::message("/sync", 77));
    OscReply synced;
    REQUIRE(fx.waitForReply("/synced", synced));
}

} // namespace synthdef_bytes
