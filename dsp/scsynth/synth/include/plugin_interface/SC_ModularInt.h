// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron

#pragma once

#include "SC_Types.h"

// int32 arithmetic that wraps round on overflow. A phase accumulator counts
// round its range on purpose, and its increment comes from a control, so any
// sum can overflow. Signed overflow is undefined; unsigned arithmetic wraps,
// and converting the result back to int32 keeps its bits (C++20).
inline int32 sc_add_modular(int32 a, int32 b) { return (int32)((uint32)a + (uint32)b); }
inline int32 sc_sub_modular(int32 a, int32 b) { return (int32)((uint32)a - (uint32)b); }
inline int32 sc_mul_modular(int32 a, int32 b) { return (int32)((uint32)a * (uint32)b); }
