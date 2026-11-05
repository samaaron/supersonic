// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * GuestWorld.h — the World of the fixture's engine, for a test that drives
 * scsynth's doors (guest_doors.h) directly.
 *
 * Every door takes the World, which names the instance it belongs to: the
 * core holds that instance (core_state.h) and the instance holds its World.
 */
#pragma once

#include "EngineFixture.h"
#include "core_state.h"    // instance_of: the core's guest instance
#include "guest_doors.h"   // supersonic_guest_world

inline World* guestWorld(EngineFixture& fx) {
    return supersonic_guest_world(clockwork::instance_of(fx.engine().core()));
}
