// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * TestSecret.h — one per-boot secret for every command port a test starts
 * (the same helper as clockwork/test/TestSecret.h, beside this suite's own
 * fixtures)
 * and every client a test connects with: the 32 bytes, and the secret
 * object the servers take (clockwork_comms_secret.h).
 */
#pragma once

#include "clockwork_comms_secret.h"

#include <array>
#include <cstdint>
#include <memory>
#include <random>

inline const std::array<uint8_t, 32>& testSecretBytes() {
    static const std::array<uint8_t, 32> bytes = [] {
        std::array<uint8_t, 32> b {};
        std::random_device rd;
        for (auto& x : b) x = static_cast<uint8_t>(rd());
        b[0] |= 1;   // never all zeros, which the library refuses
        return b;
    }();
    return bytes;
}

inline const ClockworkCommsSecret* testSecret() {
    static const std::unique_ptr<ClockworkCommsSecret, void (*)(ClockworkCommsSecret*)> secret{
        clockwork_comms_secret_from_bytes(testSecretBytes().data()), &clockwork_comms_secret_free};
    return secret.get();
}
