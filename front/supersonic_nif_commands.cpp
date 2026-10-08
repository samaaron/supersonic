// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
// The NIF sends through supersonic::Commands, as SuperSonic's server does: a
// BEAM client's /d_load and /b_allocRead are read here, never on the audio
// thread. Clockwork's NIF asks for it once per boot
// (clockwork/src/nif/clockwork_nif_front.h).
#include "clockwork_nif_front.h"
#include "supersonic_commands.h"

std::unique_ptr<OscFront> clockwork_nif_make_front(ClockworkEngine& engine, IOscTransport& replies) {
    return std::make_unique<supersonic::Commands>(engine, &replies);
}
