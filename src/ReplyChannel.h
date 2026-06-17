/*
 * SuperSonic
 * Copyright (c) 2025 Sam Aaron
 *
 * Dual-licensed MIT OR GPL-3.0-or-later (see repo LICENSE).
 *
 * ReplyChannel — a backend's reply egress, bound once when the backend is
 * registered (not constructed per message). reply() emits one OSC packet to that
 * egress, keyed by the caller's origin token (0 = broadcast to the notify
 * audience). Which egress (RT OUT ring vs NRT-out ring vs a host transport) is
 * fixed by where the backend is registered, so it need not vary per call.
 *
 * RT-safe: a plain function pointer + context, callable from the audio thread
 * (no std::function, no allocation). Every scheduler backend replies through one
 * of these; the only per-message input is the token.
 */
#pragma once

#include <cstdint>

struct ReplyChannel {
    void (*fn)(void* ctx, uint32_t token, const uint8_t* osc, uint32_t len) = nullptr;
    void* ctx = nullptr;

    void reply(uint32_t token, const uint8_t* osc, uint32_t len) const {
        if (fn) fn(ctx, token, osc, len);
    }
};
