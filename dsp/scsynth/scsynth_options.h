// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Sam Aaron
/*
 * scsynth_options.h — THE options scsynth takes from a host, in one place.
 *
 * Every number a host may choose for the engine's own tables is listed once
 * below, with its default, its range, the scsynth command-line letter it
 * answers to, and a line of documentation. Everything else derives from this
 * list and nothing restates it:
 *
 *   - the guest parses its config block against it (scsynth_options.cpp);
 *   - the native host generates its -n/-b/-a/... flags and usage from it
 *     (host/EngineHost.cpp);
 *   - the JavaScript client's defaults, validation and encoder come from
 *     js/lib/scsynth_options_schema.js, GENERATED from this file by
 *     scripts/gen-scsynth-options.mjs, and a test fails when the two drift.
 *
 * HOW THE BLOCK IS SPELLED. DspConfig::guest_config is opaque to clockwork:
 * bytes a host writes and the guest reads. scsynth reads them as text, one
 * `name=value` per line, so no host anywhere has to know a slot number:
 *
 *     maxNodes=2048
 *     realTimeMemorySize=65536
 *
 * A name is matched case-insensitively and ignoring '_' and '-', so a host
 * whose own spelling is max_nodes (the BEAM) or MAXNODES is not made to
 * translate. Blank lines and lines starting '#' are ignored. An option not
 * mentioned takes its default. A name that is not here, a value that is not
 * a non-negative integer, or one outside the range, REFUSES THE BOOT with a
 * message naming the line: a silently ignored option is a configuration that
 * looked applied and was not.
 *
 * Every value is a uint32: these are counts and sizes.
 *
 * WHAT IS NOT HERE. Sample rate, block size and the channel counts are what
 * the host's device opened, not what the engine asked for; they arrive in
 * DspConfig and are not options of the guest's. A web client keeps a few
 * words of its own beside these (numInputBusChannels, bufLength, ...) which
 * it consumes itself and never sends.
 */
#ifndef SCSYNTH_OPTIONS_H
#define SCSYNTH_OPTIONS_H

#include <stdint.h>

/*
 * X(name, flag, default, min, max, doc)
 *
 * `flag` is the scsynth command-line letter (the native host honours it), or
 * 0 for an option with no flag. Keep the order: it is the order the native
 * host's usage prints, and the order the generated schema lists.
 */
#define SCSYNTH_OPTIONS(X) \
    X(numBuffers,            'b', 1024,  1, 65535,    "Sample buffers (SndBuf slots)") \
    X(maxNodes,              'n', 1024,  1, 4194304,  "Nodes (synths and groups) that may exist at once") \
    X(maxGraphDefs,          'd', 1024,  1, 4194304,  "Synth definitions that may be loaded at once") \
    X(maxWireBufs,           'w', 64,    1, 4194304,  "Wire buffers for a synth's internal connections") \
    X(numAudioBusChannels,   'a', 1024,  1, 4194304,  "Audio bus channels") \
    X(numControlBusChannels, 'c', 16384, 1, 16777216, "Control bus channels") \
    X(realTimeMemorySize,    'm', 8192,  1, 4194304,  "Real-time memory pool, in KB") \
    X(numRGens,              'r', 64,    1, 65536,    "Random number generators") \
    X(loadGraphDefs,         'D', 0,     0, 1,        "Load synth definitions from the synthdef directory at boot (1) or not (0)") \
    X(verbosity,             'V', 0,     0, 4,        "How much the engine prints; 0 is quiet")

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ScsynthOptions {
#define SCSYNTH_OPTION_FIELD(name, flag, def, lo, hi, doc) uint32_t name;
    SCSYNTH_OPTIONS(SCSYNTH_OPTION_FIELD)
#undef SCSYNTH_OPTION_FIELD
} ScsynthOptions;

/* Every option at its default. */
void scsynth_options_defaults(ScsynthOptions* out);

/*
 * Read a config block. `text` is at most `len` bytes and may end early at a
 * NUL. `out` starts from the defaults. Returns 0 when every line was taken;
 * otherwise a message naming the offending line is written into `err`
 * (`errcap` bytes, always terminated) and the return is non-zero. On a
 * refusal `out` is not to be trusted.
 */
int scsynth_options_parse(const char* text, uint32_t len, ScsynthOptions* out,
                          char* err, uint32_t errcap);

/* The list as data, for a host that builds its own front from it. */
typedef struct ScsynthOptionInfo {
    const char* name;
    char        flag;      /* the command-line letter, or 0 */
    uint32_t    def;
    uint32_t    min;
    uint32_t    max;
    const char* doc;
} ScsynthOptionInfo;

uint32_t                 scsynth_option_count(void);
const ScsynthOptionInfo* scsynth_option_info(uint32_t index);

#ifdef __cplusplus
}
#endif

#endif /* SCSYNTH_OPTIONS_H */
