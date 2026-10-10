// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * scsynth_options_text.h — the guest's options as a struct, for a C++ host.
 *
 * DspConfig::guest_config is text, one `name=value` per line, read by
 * scsynth_options_parse (scsynth_options.h): the one spelling every host
 * shares, so no host knows a slot number. A C++ host that would rather fill
 * a struct than spell names fills ScsynthOptions and writes it out here; the
 * names come from the same X-macro, so a typo is a compile error rather than
 * a boot refused with a line.
 *
 *   ScsynthOptions o;
 *   scsynth_options_defaults(&o);
 *   o.numAudioBusChannels = 1024;
 *   o.realTimeMemorySize  = 131072;     // KB
 *   cfg.guestConfig = scsynth_options_text(o);
 */
#ifndef SCSYNTH_OPTIONS_TEXT_H
#define SCSYNTH_OPTIONS_TEXT_H

#include "scsynth_options.h"

#include <string>

// Every option as a line, in the header's order.
inline std::string scsynth_options_text(const ScsynthOptions& o) {
    std::string out;
#define SCSYNTH_OPTION_LINE(name, flag, def, lo, hi, doc) \
    out += #name "=" + std::to_string(o.name) + "\n";
    SCSYNTH_OPTIONS(SCSYNTH_OPTION_LINE)
#undef SCSYNTH_OPTION_LINE
    return out;
}

#endif /* SCSYNTH_OPTIONS_TEXT_H */
