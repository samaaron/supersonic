// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * Synth definitions as Clockwork assets.
 *
 * A definition's bytes are committed as a RAW asset (/clockwork/asset/commit, kind 0) with an id at or above
 * kDefinitionAssetFirst; below it an asset id is a buffer number. The guest reads the definition when the asset is
 * committed (dsp_asset), and Clockwork hands every asset it still holds to a new instance on a rebuild, so the
 * definitions loaded this way are still there after a sample-rate change, with nothing sent again. The guest releases
 * a definition's asset when the definition is replaced, freed (/d_free) or all are freed (/d_freeAll); the client
 * then hears /clockwork/asset/released and can reuse the bytes. The front (front/supersonic_commands.cpp) and the web
 * client (js/supersonic.js) pick the ids.
 *
 * A client gives each definition asset a higher id than the last one it committed, and never reuses an id. A new
 * instance is handed its assets in Clockwork's table order, not commit order, so the guest uses the ids to keep the
 * newest: of two definitions with one name, the one from the higher id stays.
 */
#pragma once
#include <cstdint>

namespace supersonic {
// Far above any buffer number scsynth allows (numBuffers is at most a few thousand).
inline constexpr uint32_t kDefinitionAssetFirst = 1u << 20;
}
