// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Sam Aaron
/*
 * scsynth_options.cpp — reading the guest's config block against
 * scsynth_options.h. No allocation: this runs inside dsp_new, but nothing
 * here needs a heap anyway.
 */
#include "scsynth_options.h"

#include <cstdio>
#include <cstring>

namespace {

const ScsynthOptionInfo kOptions[] = {
#define SCSYNTH_OPTION_INFO(name, flag, def, lo, hi, doc) { #name, flag, def, lo, hi, doc },
    SCSYNTH_OPTIONS(SCSYNTH_OPTION_INFO)
#undef SCSYNTH_OPTION_INFO
};
constexpr uint32_t kOptionCount = sizeof(kOptions) / sizeof(kOptions[0]);

// A pointer to each field, in the same order as kOptions, so a parsed line
// lands in the struct without a second list to keep in step.
uint32_t* field_of(ScsynthOptions* o, uint32_t index) {
    uint32_t* fields[] = {
#define SCSYNTH_OPTION_PTR(name, flag, def, lo, hi, doc) &o->name,
        SCSYNTH_OPTIONS(SCSYNTH_OPTION_PTR)
#undef SCSYNTH_OPTION_PTR
    };
    return fields[index];
}

// Names match case-insensitively and ignoring '_' and '-': maxNodes,
// max_nodes and MAXNODES are the one option.
bool name_matches(const char* a, uint32_t alen, const char* b) {
    uint32_t i = 0;
    for (;;) {
        while (i < alen && (a[i] == '_' || a[i] == '-')) ++i;
        while (*b == '_' || *b == '-') ++b;
        const bool aEnd = i >= alen;
        const bool bEnd = *b == '\0';
        if (aEnd || bEnd) return aEnd && bEnd;
        char ca = a[i], cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
        ++i; ++b;
    }
}

void fail(char* err, uint32_t errcap, const char* fmt, const char* a, const char* b) {
    if (!err || errcap == 0) return;
    std::snprintf(err, errcap, fmt, a, b);
    err[errcap - 1] = '\0';
}

}  // namespace

extern "C" {

void scsynth_options_defaults(ScsynthOptions* out) {
    if (!out) return;
#define SCSYNTH_OPTION_DEFAULT(name, flag, def, lo, hi, doc) out->name = def;
    SCSYNTH_OPTIONS(SCSYNTH_OPTION_DEFAULT)
#undef SCSYNTH_OPTION_DEFAULT
}

uint32_t scsynth_option_count(void) { return kOptionCount; }

const ScsynthOptionInfo* scsynth_option_info(uint32_t index) {
    return index < kOptionCount ? &kOptions[index] : nullptr;
}

int scsynth_options_parse(const char* text, uint32_t len, ScsynthOptions* out,
                          char* err, uint32_t errcap) {
    if (err && errcap) err[0] = '\0';
    if (!out) return 1;
    scsynth_options_defaults(out);
    if (!text) return 0;

    // The block may be shorter than the region it sits in: a NUL ends it.
    uint32_t end = 0;
    while (end < len && text[end] != '\0') ++end;

    uint32_t pos = 0;
    while (pos < end) {
        // One line, without its terminator.
        uint32_t lineEnd = pos;
        while (lineEnd < end && text[lineEnd] != '\n') ++lineEnd;
        const char* line = text + pos;
        uint32_t n = lineEnd - pos;
        pos = lineEnd + 1;
        if (n && line[n - 1] == '\r') --n;

        // Trim, skip blanks and comments.
        uint32_t s = 0;
        while (s < n && (line[s] == ' ' || line[s] == '\t')) ++s;
        while (n > s && (line[n - 1] == ' ' || line[n - 1] == '\t')) --n;
        if (s == n || line[s] == '#') continue;

        char shown[80];
        {
            const uint32_t m = (n - s) < sizeof(shown) - 1 ? (n - s) : (uint32_t)sizeof(shown) - 1;
            std::memcpy(shown, line + s, m);
            shown[m] = '\0';
        }

        // name = value
        uint32_t eq = s;
        while (eq < n && line[eq] != '=') ++eq;
        if (eq >= n) {
            fail(err, errcap, "scsynth option \"%s\": expected name=value%s", shown, "");
            return 1;
        }
        uint32_t nameEnd = eq;
        while (nameEnd > s && (line[nameEnd - 1] == ' ' || line[nameEnd - 1] == '\t')) --nameEnd;
        uint32_t v = eq + 1;
        while (v < n && (line[v] == ' ' || line[v] == '\t')) ++v;
        if (nameEnd == s) {
            fail(err, errcap, "scsynth option \"%s\": expected name=value%s", shown, "");
            return 1;
        }

        uint32_t index = kOptionCount;
        for (uint32_t i = 0; i < kOptionCount; ++i)
            if (name_matches(line + s, nameEnd - s, kOptions[i].name)) { index = i; break; }
        if (index == kOptionCount) {
            fail(err, errcap, "scsynth option \"%s\": no such option%s", shown, "");
            return 1;
        }
        const ScsynthOptionInfo& info = kOptions[index];

        // A non-negative integer, and nothing after it.
        if (v >= n) {
            fail(err, errcap, "scsynth option \"%s\": %s takes a number", shown, info.name);
            return 1;
        }
        uint64_t value = 0;
        for (uint32_t i = v; i < n; ++i) {
            const char c = line[i];
            if (c < '0' || c > '9') {
                fail(err, errcap, "scsynth option \"%s\": %s takes a non-negative integer", shown, info.name);
                return 1;
            }
            value = value * 10 + (uint64_t)(c - '0');
            if (value > 0xFFFFFFFFull) break;
        }
        if (value < info.min || value > info.max) {
            char range[96];
            std::snprintf(range, sizeof(range), "%s must be between %u and %u", info.name, info.min, info.max);
            fail(err, errcap, "scsynth option \"%s\": %s", shown, range);
            return 1;
        }
        *field_of(out, index) = (uint32_t)value;
    }
    return 0;
}

}  // extern "C"
