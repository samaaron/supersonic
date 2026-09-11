// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * TestPid.h — this process's id, for temp file names that must not collide
 * between test binaries running side by side. POSIX spells it getpid() in
 * <unistd.h>; Windows spells it _getpid() in <process.h>.
 */
#pragma once
#ifdef _WIN32
#include <process.h>
inline int testPid() { return _getpid(); }
#else
#include <unistd.h>
inline int testPid() { return static_cast<int>(::getpid()); }
#endif
