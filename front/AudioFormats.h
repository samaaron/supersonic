// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
// The header and sample format names scsynth has always accepted, mapped to
// what the codecs write. Anything else is refused, as libsndfile refused it.
#pragma once

#include "clockwork_audio_file.h"

#include <string>
#if defined(_WIN32)
#  define supersonic_strcasecmp _stricmp
#else
#  include <strings.h>
#  define supersonic_strcasecmp strcasecmp
#endif

namespace supersonic_formats {

inline uint32_t headerFormat(const std::string& name) {
    const char* n = name.c_str();
    if (supersonic_strcasecmp(n, "wav") == 0 || supersonic_strcasecmp(n, "wave") == 0) return CLOCKWORK_AUDIO_FORMAT_WAV;
    if (supersonic_strcasecmp(n, "flac") == 0) return CLOCKWORK_AUDIO_FORMAT_FLAC;
    if (supersonic_strcasecmp(n, "aiff") == 0 || supersonic_strcasecmp(n, "aif") == 0) return CLOCKWORK_AUDIO_FORMAT_AIFF;
    if (supersonic_strcasecmp(n, "w64") == 0) return CLOCKWORK_AUDIO_FORMAT_W64;
    if (supersonic_strcasecmp(n, "rf64") == 0) return CLOCKWORK_AUDIO_FORMAT_RF64;
    return CLOCKWORK_AUDIO_FORMAT_UNKNOWN;
}

inline uint32_t sampleEncoding(const std::string& name) {
    const char* n = name.c_str();
    if (supersonic_strcasecmp(n, "int16") == 0) return CLOCKWORK_AUDIO_ENCODING_PCM_S16;
    if (supersonic_strcasecmp(n, "int24") == 0) return CLOCKWORK_AUDIO_ENCODING_PCM_S24;
    if (supersonic_strcasecmp(n, "int32") == 0) return CLOCKWORK_AUDIO_ENCODING_PCM_S32;
    if (supersonic_strcasecmp(n, "float") == 0) return CLOCKWORK_AUDIO_ENCODING_FLOAT32;
    return CLOCKWORK_AUDIO_ENCODING_UNKNOWN;
}

// /clockwork/record/start's bit depth: 16 and 24 are PCM, 32 is float, as
// the engine's recorder mapped them.
inline uint32_t bitDepthEncoding(int bits) {
    switch (bits) {
        case 16: return CLOCKWORK_AUDIO_ENCODING_PCM_S16;
        case 24: return CLOCKWORK_AUDIO_ENCODING_PCM_S24;
        case 32: return CLOCKWORK_AUDIO_ENCODING_FLOAT32;
        default: return CLOCKWORK_AUDIO_ENCODING_UNKNOWN;
    }
}

} // namespace supersonic_formats
