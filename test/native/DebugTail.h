// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
// The last engine debug lines and a backtrace, printed on a fatal signal.
//
// A SIGABRT under Catch2 reports the test's name and nothing else: the
// engine's own account of what went wrong sits in the fixture's captured
// debug stream, which no INFO() gets to dump once the process is dying, and
// the frame that aborted is lost. Every fixture pushes each debug line here;
// the test main installs the handler. Catch2 installs its own per test case,
// reports, restores this one and re-raises, so this runs last — and prints
// what it can with async-signal-safe calls only.
#pragma once

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#if defined(__APPLE__) || defined(__linux__)
#include <execinfo.h>
#include <unistd.h>
#endif

namespace debug_tail {

constexpr int kLines = 64;
constexpr int kWidth = 240;
inline char g_lines[kLines][kWidth] = {};
inline std::atomic<unsigned> g_next{0};

inline void push(const std::string& line) {
    const unsigned i = g_next.fetch_add(1, std::memory_order_relaxed) % kLines;
    std::strncpy(g_lines[i], line.c_str(), kWidth - 1);
    g_lines[i][kWidth - 1] = '\0';
}

inline void emit(const char* s) {
#if defined(__APPLE__) || defined(__linux__)
    (void)!::write(2, s, std::strlen(s));
#else
    std::fputs(s, stderr);
#endif
}

inline void onFatal(int sig) {
    emit("\n[debug-tail] fatal signal; the last engine debug lines were:\n");
    const unsigned end = g_next.load(std::memory_order_relaxed);
    const unsigned begin = end > (unsigned)kLines ? end - kLines : 0;
    for (unsigned i = begin; i < end; ++i) {
        emit("  | "); emit(g_lines[i % kLines]); emit("\n");
    }
#if defined(__APPLE__) || defined(__linux__)
    emit("[debug-tail] backtrace:\n");
    void* frames[64];
    const int n = ::backtrace(frames, 64);
    ::backtrace_symbols_fd(frames, n, 2);
#endif
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

inline void installFatalHandlers() {
    std::signal(SIGABRT, onFatal);
    std::signal(SIGSEGV, onFatal);
    std::signal(SIGILL,  onFatal);
    std::signal(SIGFPE,  onFatal);
#ifdef SIGBUS
    std::signal(SIGBUS,  onFatal);
#endif
}

} // namespace debug_tail
