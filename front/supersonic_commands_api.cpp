// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
#include "supersonic_commands_api.h"
#include "supersonic_commands.h"

struct SupersonicCommands {
    supersonic::Commands front;
    SupersonicCommands(ClockworkClient* client, supersonic::Commands::Work work) : front(client, nullptr, work) {}
};

extern "C" {

SupersonicCommands* supersonic_commands_open(ClockworkClient* client, int own_threads) {
    if (!client) return nullptr;
    using Work = supersonic::Commands::Work;
    return new SupersonicCommands(client, own_threads ? Work::Own : Work::Host);
}

int supersonic_commands_send(SupersonicCommands* f, const uint8_t* osc, uint32_t bytes, uint32_t token) {
    if (!f || !osc || bytes == 0) return 0;
    return f->front.send(osc, bytes, token) ? 1 : 0;
}

uint32_t supersonic_commands_poll(SupersonicCommands* f, ClockworkClientMessage* out, uint32_t max) {
    return f ? f->front.poll(out, max) : 0;
}

void supersonic_commands_run_pending(SupersonicCommands* f) {
    if (f) f->front.runPending();
}

void supersonic_commands_close(SupersonicCommands* f) {
    delete f;
}

}
