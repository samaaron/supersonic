// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * scsynth_synthdef_dirs.h — where scsynth's -D looks.
 *
 * The directories scsynth loads synth definitions from at boot (-D 1), as it
 * has always read them: each in SC_SYNTHDEF_PATH, when that is set, else
 * `synthdefs` in the user's application-support directory. The engine reads no
 * files: a host reads these and hands the definitions over as bytes, in
 * /d_recv (supersonic::Commands::loadDefinitions). Native builds only.
 */
#pragma once

#include <string>
#include <vector>

std::vector<std::string> scsynth_synthdef_directories();
