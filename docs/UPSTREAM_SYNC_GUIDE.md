# Supersonic ↔ SuperCollider Upstream Sync Guide

**Last Updated**: 2026-10-08
**Last Sync Commit**: 16be628
**Upstream Branch**: supercollider/develop (tracked to 2026-10-08)
**Verified Against**: SuperCollider 3.15.0-dev (develop HEAD 16be628)

---

## Overview

Supersonic is a WASM port of SuperCollider's scsynth audio server. It originated from [SuperCollider PR #6569](https://github.com/supercollider/supercollider/pull/6569) but has diverged. This guide explains how to identify and backport relevant upstream changes from SuperCollider to supersonic.

---

## License boundary: upstream code goes into SuperSonic, never into clockwork

**Read this before every sync.**

SuperSonic as a whole is **AGPL-3.0-or-later**: clockwork, the substrate it runs
on (ingress/egress, the clock, scheduler, MIDI, the JS client and workers), is
AGPL-3.0-or-later or commercially licensed, and the combined program takes the
AGPL side. The forked scsynth core in `dsp/scsynth` is GPL-3.0-or-later,
inherited from upstream.

### SuperSonic may take GPL and AGPL upstream code

Upstream SuperCollider code — the GPL-3.0 core **and** AGPL-3.0 sources such as
upstream's own wasm port ([PR #7428](https://github.com/supercollider/supercollider/pull/7428):
`server/scsynth/SC_WebAudio.cpp`, `platform/wasm/**`) — may be backported into
SuperSonic, including the `dsp/scsynth` guest. Taking AGPL code into
`dsp/scsynth` makes those parts AGPL; that is accepted.

(Earlier versions of this guide kept AGPL out of SuperSonic entirely. That rule
predates clockwork's extraction and no longer applies.)

Most of upstream's wasm port still does not *apply*: its WebAudio driver, OSC
builder and wasm build glue do what clockwork does for SuperSonic. Treat those
files as "not applicable" on architectural grounds, but read them freely — a
fix there can point at the same bug in SuperSonic's own path.

### clockwork takes no third-party code under a non-MIT-like licence

clockwork (the `clockwork/` submodule, its own repository) is dual-licensed
AGPL/commercial, so everything in it must be code its author owns or can
relicense. **Never copy, port, paraphrase or otherwise derive code from
upstream SuperCollider (GPL or AGPL), JUCE 8+ (AGPL/commercial), or any other
copyleft source into clockwork** — a retyped fragment still carries its
licence. Permissively licensed (MIT/BSD/ISC-style) third-party code is the only
kind clockwork can carry. In practice an upstream sync never touches clockwork:
if a backport needs a new seam between the guest and clockwork, implement the
clockwork side independently, from the behaviour required, not from upstream
code.

The audio device layer is clockwork's `clockwork/smoothie`, its vendored fork
of the four **ISC-licensed** modules of JUCE 7.0.12; the never-JUCE-8 rule and
its changelog live with clockwork.

---

## Quick Start Checklist

When syncing with upstream:

- [ ] **FIRST — read the [License boundary](#license-boundary-upstream-code-goes-into-supersonic-never-into-clockwork): upstream code never goes into clockwork**
- [ ] Fetch latest upstream changes
- [ ] Identify scsynth-relevant commits since the last sync commit
- [ ] Filter out non-applicable changes (sclang, supernova, threads, tests)
- [ ] Check if changes are already applied
- [ ] Cherry-pick or manually apply changes
- [ ] Adapt for the guest (no threads, no malloc on the audio thread)
- [ ] **Leave everything uncommitted** — never `git commit` (or amend, rebase, `cherry-pick --continue`) in this repository; hand the maintainer a suggested commit split with messages in the [format below](#commit-message-format)
- [ ] Update this guide with new sync date

---

## Setup

### 1. Ensure Upstream Remote Exists

```bash
# Check if upstream remote exists
git remote -v | grep supercollider

# If not present, add it
git remote add supercollider https://github.com/supercollider/supercollider.git

# Fetch latest
git fetch supercollider
```

Or, keeping upstream's history out of this repository, a shallow clone
elsewhere reaching back past the last sync commit, with each patch's paths
remapped onto `dsp/scsynth/synth/` and applied to the working tree:

```bash
git clone --filter=blob:none --shallow-since=<a week before the last sync> \
    --branch develop https://github.com/supercollider/supercollider.git /tmp/sc-upstream
git -C /tmp/sc-upstream show --format= <hash> -- server/plugins/NoiseUGens.cpp \
    | sed -E 's#(a|b)/server/plugins/#\1/dsp/scsynth/synth/plugins/#g' > /tmp/<hash>.patch
git apply --check /tmp/<hash>.patch && git apply /tmp/<hash>.patch
```

### 2. Verify Current State

```bash
# Check your current branch
git status

# See last sync commits
git log --oneline --grep="Backported from SuperCollider" | head -10
```

---

## Finding Relevant Upstream Commits

### Step 1: Get All Commits Since Last Sync

Use the **Last Sync Commit** at the top of this file as a range start, not a
date: upstream merges branches whose commits carry older dates, and a
`--since` date filter can miss them.

```bash
LAST_SYNC_COMMIT="16be628"   # top of this file

# View all upstream commits since then
git log --oneline "$LAST_SYNC_COMMIT"..supercollider/develop
```

### Step 2: Filter for scsynth-Relevant Paths

Focus on these paths ONLY:

```bash
git log --reverse --format='%h %ad %s' --date=short \
    "$LAST_SYNC_COMMIT"..supercollider/develop -- \
    server/scsynth server/plugins \
    include/server include/plugin_interface include/common common/
```

Upstream paths map onto `dsp/scsynth/synth/`: `server/plugins/` →
`plugins/`, `server/scsynth/` → `server/`, `include/` → `include/`,
`common/` → `common/`. An upstream patch can usually be applied directly
once its paths are rewritten (`git show <hash> -- <paths>`, rewrite the
`a/` and `b/` prefixes, then `git apply --check`).

### Step 3: Exclude Non-Applicable Changes

**Always SKIP these**:
- ❌ Upstream's own wasm port (`server/scsynth/SC_WebAudio.cpp`, `platform/wasm/**`) — not applicable, clockwork does that job; read it for bugs that may also be ours
- ❌ Anything that would land in `clockwork/` (see the [License boundary](#license-boundary-upstream-code-goes-into-supersonic-never-into-clockwork))
- ❌ sclang changes (`lang/`, `SCClassLibrary/`, `common/SC_Filesystem*` class-library folder selection)
- ❌ supernova changes (`server/supernova/`)
- ❌ Help files (`HelpSource/`, `*.schelp`)
- ❌ Test files (`testsuite/`)
- ❌ CMake/build system (`CMakeLists.txt`)
- ❌ Documentation (`README`, `*.md` in upstream)
- ❌ Thread/mutex/lock code
- ❌ Print statements (`printf`, `scprintf` for debugging)
- ❌ Link UGens (`server/plugins/LinkUGens.cpp`) - see [Intentionally Excluded Features](#intentionally-excluded-features)

**Only INCLUDE**:
- ✅ scsynth server core (`server/scsynth/`)
- ✅ Plugin UGens (`server/plugins/`)
- ✅ Plugin interface headers (`include/plugin_interface/`)
- ✅ Server headers (`include/server/`)

---

## Categorizing Commits

### Priority Levels

Organize commits by priority before applying:

#### 🔴 **CRITICAL** - Apply immediately
- Crashes, memory corruption, undefined behavior
- Audio glitches (pops, clicks, distortion)
- Timing bugs in envelopes or triggers
- UGen initialization issues (first sample incorrect)

#### 🟡 **HIGH** - Apply soon
- Algorithm improvements affecting output
- Performance optimizations
- Memory leaks
- Edge case fixes

#### 🟢 **MEDIUM** - Apply when convenient
- Code quality improvements
- Refactoring (if it doesn't change behavior)
- Minor optimizations
- Documentation in code

#### ⚪ **LOW** - Optional
- Style changes
- Comment improvements
- Variable renames (unless fixing ambiguity)

---

## Checking If Already Applied

Before applying a commit, check if it's already in supersonic:

### Method 1: Search by Commit Message

```bash
# Search for upstream commit hash in supersonic history
UPSTREAM_HASH="a4c18eef9"
git log --all --grep="$UPSTREAM_HASH"

# If found, it's already applied!
```

### Method 2: Check the Actual Code

```bash
# Read the relevant file to see if the fix is present
# Example: checking if EnvGen has counter_fractional
grep -n "counter_fractional" dsp/scsynth/synth/plugins/LFUGens.cpp
```

### Method 3: Check Function Signatures

```bash
# Example: checking if World_TotalFree exists (for rtMemoryStatus)
grep -r "World_TotalFree" dsp/scsynth/synth/
```

**Always verify by reading code** - commit messages can be misleading!

---

## Applying Changes

Whichever option, the result stays **uncommitted** in the working tree for the
maintainer to review and commit. Always pass `--no-commit` to `cherry-pick`.

### Option A: Cherry-Pick (Preferred When Possible)

```bash
# Fetch the specific commit
UPSTREAM_HASH="a4c18eef9"
git fetch supercollider $UPSTREAM_HASH

# Try to cherry-pick
git cherry-pick $UPSTREAM_HASH --no-commit

# Check what was picked
git status
git diff --cached

# If conflicts occur, see "Handling Conflicts" section below
```

### Option B: Manual Application

When cherry-pick isn't feasible:

1. **View the upstream change**:
   ```bash
   git show supercollider/develop:$UPSTREAM_HASH
   ```

2. **Read the relevant supersonic file**:
   ```bash
   # Use absolute path
   cat dsp/scsynth/synth/plugins/LFUGens.cpp
   ```

3. **Apply changes manually** using the Edit tool

4. **Adapt for WASM** (see next section)

---

## Adapting Upstream Changes

SuperSonic has three build targets (WASM, native, NIF) with a shared engine. Use the [preprocessor conventions](#preprocessor-conventions-for-upstream-files) to mark adaptations clearly.

### 1. Print Functions — keep upstream's `scprintf` / `Print`

No change needed. `scprintf` is SuperSonic's own (`dsp/scsynth/synth/server/SC_Stubs.cpp`):
it formats into a stack buffer and hands the line to the host's log through
`supersonic_guest_log` (`dsp/scsynth/scsynth_dsp.cpp`), on every target. A
plugin's `Print` reaches the same place. (The old `ss_log` substitution is
gone.)

### 2. Platform-unavailable APIs — use `#ifndef __EMSCRIPTEN__`

For filesystem, shared memory IPC and other hosted-OS APIs — keep the upstream code in the `#ifndef` block exactly as-is (converting any Boost usage per the Boost-free table below).

### 3. Verify Memory Allocations

```cpp
// ✅ These are fine in supersonic (pre-allocated pool)
RTAlloc(world, size);
RTFree(world, ptr);
World_Alloc(world, size);

// ❌ Avoid direct malloc/free in audio thread
```

### 4. Thread-Related Code

Guard with `#ifndef __EMSCRIPTEN__` if the code requires threading APIs. WASM AudioWorklet is single-threaded. Native SuperSonic also runs NRT (single-threaded) but can link threading libraries.

---

## Handling Conflicts

Common conflicts when cherry-picking. Once they are resolved, end the
cherry-pick with `git cherry-pick --quit`, which leaves the changes in place
uncommitted — **never** `git cherry-pick --continue`, which commits.

### 1. Help Files (.schelp)

```bash
# Supersonic doesn't include help files
git rm -f HelpSource/**/*.schelp
git rm -f SCClassLibrary/**/*.sc

# End the cherry-pick, keeping the changes uncommitted
git cherry-pick --quit
```

### 2. Test Files

```bash
# Supersonic doesn't include test suite
git rm -f testsuite/**/*.sc
git rm -f testsuite/**/*.scd

# End the cherry-pick, keeping the changes uncommitted
git cherry-pick --quit
```

### 3. Missing Files

If a commit modifies a file that doesn't exist in supersonic:

```bash
# Check if the plugin exists
ls dsp/scsynth/synth/plugins/ | grep "PluginName"

# If it doesn't exist, skip this part
git rm -f server/plugins/PluginName.cpp

# Or abort the cherry-pick if entire commit is not applicable
git cherry-pick --abort
```

### 4. Conflicts in Files SuperSonic Has Changed

```bash
# Resolve the conflict markers by hand, then end the cherry-pick,
# keeping the changes uncommitted
git cherry-pick --quit
```

---

## Commit Message Format

The maintainer commits; a sync never does. For each backport, give them a
suggested commit — the files it covers and a message in this format:

```
<component>: <brief description>

Backported from SuperCollider upstream commit <hash>
https://github.com/supercollider/supercollider/commit/<full-hash>

[Optional: Original PR link]
Original PR: https://github.com/supercollider/supercollider/pull/<number>

[Detailed description of what changed]
[Any WASM-specific adaptations made]
```

### Examples

```
plugins: EnvGen: fix endtime bug and add tests (#6664)

Backported from SuperCollider upstream commit a4c18eef9
https://github.com/supercollider/supercollider/commit/a4c18eef9

Uses counter_fractional for accurate envelope calculations and
tracks residual error to prevent timing drift.
```

```
server: Add /g_dumpTree end delimiter

Backported from SuperCollider upstream commit 66b74c05f
https://github.com/supercollider/supercollider/commit/66b74c05f9056acf9fc94f81bc9be834ca18a8a3

Original PR: https://github.com/supercollider/supercollider/pull/7039

Adds END NODE TREE delimiter to both Group_DumpNodeTree and
Group_DumpNodeTreeAndControls functions. Adapted to use ss_log
instead of scprintf for WASM AudioWorklet compatibility.
```

---

## Verification Checklist

After applying changes:

- [ ] **Nothing from upstream landed in `clockwork/`** (see the [License boundary](#license-boundary-upstream-code-goes-into-supersonic-never-into-clockwork))
- [ ] Code compiles on all targets (`scripts/build-all.sh` — native, NIF, web)
- [ ] `scripts/check-guest-boundary.sh` passes (the guest reaches clockwork only through `clockwork/src/dsp_api.h`)
- [ ] SuperSonic-specific changes wrapped in `#ifdef CLOCKWORK_GUEST` with upstream code in `#else`
- [ ] Platform guards use `#ifndef __EMSCRIPTEN__` (not `#ifdef CLOCKWORK_GUEST`)
- [ ] No malloc/free on audio thread paths
- [ ] Nothing committed — every change is left in the working tree
- [ ] Each suggested commit message includes the upstream hash and link
- [ ] Each suggested commit message explains adaptations (if any)
- [ ] Tests pass: `scripts/test-native.sh`, `npx playwright test`, `scripts/test-nif.sh`
- [ ] Updated LAST_SYNC date at top of this file

---

## Common Patterns

### Pattern 1: UGen Initialization Fixes

**What to look for**: Changes to how first sample is computed

```cpp
// Before
ZOUT0(0) = 0.f;

// After
ZOUT0(0) = compute_actual_value(input);
```

**Why it matters**: Prevents pops, clicks, incorrect initial values

### Pattern 2: Algorithm Corrections

**What to look for**: Changes to DSP calculations, loop logic, boundary conditions

```cpp
// Example: Phase wrapping
phase = sc_wrap(phase, -1.0, 1.0);  // Added wrapping
```

**Why it matters**: Affects audio output quality and correctness

### Pattern 3: Memory Safety

**What to look for**: RTAlloc failure handling, buffer bounds checking

```cpp
// Before
float* buf = (float*)RTAlloc(world, size);
// use buf without checking

// After
float* buf = (float*)RTAlloc(world, size);
if (!buf) {
    ClearUnitOutputs(unit, 1);
    return;
}
```

**Why it matters**: Prevents crashes in WASM environment

### Pattern 4: Code Quality (Low Priority)

**What to look for**: Replace `Fill` with `Clear`, use macros instead of literals

```cpp
// Before
Fill(count, out, 0.f);

// After
Clear(count, out);
```

**Why it matters**: Better semantic clarity, easier maintenance

---

## Batch Processing

When you have multiple commits to apply:

### 1. Create a TODO List

```bash
# Save commit list to file
git log supercollider/develop --oneline --since="2025-09-25" \
    -- "server/scsynth/*.cpp" "server/plugins/*.cpp" \
    > /tmp/upstream_commits.txt

# Review and categorize by priority
```

### 2. Apply in Priority Order

1. Apply all CRITICAL fixes first
2. Then HIGH priority
3. Then MEDIUM/LOW together

### 3. Track Progress

Keep notes as you go:
- ✅ Applied successfully
- ⏭️ Already present
- ❌ Not applicable
- ⚠️ Needs manual review

---

## Example Workflow

Here's a complete example sync session:

```bash
# 1. Setup
cd /path/to/supersonic
git fetch supercollider
git checkout main
git status  # ensure clean working directory

# 2. Find commits since last sync (Last Sync Commit, top of this file)
LAST_SYNC_COMMIT="16be628"
git log --reverse --oneline "$LAST_SYNC_COMMIT"..supercollider/develop -- \
    server/scsynth server/plugins \
    include/server include/plugin_interface include/common common/ \
    > /tmp/new_commits.txt

# 3. Review the list
cat /tmp/new_commits.txt

# 4. Check first commit
COMMIT="abc123def"
git show supercollider/develop:$COMMIT

# 5. Check if already applied
git log --all --grep="$COMMIT"

# 6. Not found, so apply it
git cherry-pick $COMMIT --no-commit

# 7. Handle conflicts (help files)
git rm -f HelpSource/*.schelp testsuite/*.sc
git status

# 8. Adapt for the guest if needed (check diff)
git diff --cached

# 9. Do NOT commit. Note the files and a suggested message for the
#    maintainer (see Commit Message Format):
#      plugins: Fix initialization
#
#      Backported from SuperCollider upstream commit $COMMIT
#      https://github.com/supercollider/supercollider/commit/$COMMIT
#
#      [description]

# 10. Repeat for next commit
```

---

## Troubleshooting

### Problem: Cherry-pick produces no changes

```bash
# This means the change is already applied
git cherry-pick --abort

# Verify by checking the code
grep -n "the_fix" dsp/scsynth/synth/plugins/SomeUGen.cpp
```

### Problem: Too many conflicts

```bash
# Abort and apply manually
git cherry-pick --abort

# View the change
git show $COMMIT

# Apply by hand using Edit tool
```

### Problem: File doesn't exist in supersonic

```bash
# Check if plugin exists
ls dsp/scsynth/synth/plugins/ | grep PluginName

# If not, this commit is not applicable
git cherry-pick --abort

# Mark as "not applicable" in your notes
```

### Problem: Commit includes thread code

```bash
# View the commit
git show $COMMIT | grep -i "thread\|mutex\|lock"

# If thread code is core to the fix, skip it
# If it's incidental, cherry-pick and remove the thread parts manually
```

---

## Files to Monitor

### High Priority - Monitor Closely

```
server/scsynth/SC_Group.cpp       # Node tree management
server/scsynth/SC_Graph.cpp       # Synth graph execution
server/scsynth/SC_GraphDef.cpp    # SynthDef handling
server/scsynth/SC_World.cpp       # Server core
server/plugins/LFUGens.cpp        # Envelopes (EnvGen, IEnvGen, etc.)
server/plugins/TriggerUGens.cpp   # Timing-critical triggers
server/plugins/DelayUGens.cpp     # Delay lines
server/plugins/FilterUGens.cpp    # Filters
```

### Medium Priority

```
server/scsynth/SC_BufGen.cpp      # Buffer generation
server/scsynth/SC_MiscCmds.cpp    # OSC commands
server/scsynth/SC_Unit.cpp        # UGen base
include/plugin_interface/*.h      # Plugin API
include/common/*.h                # clz, SC_Types, fftlib headers
```

### Low Priority

```
server/scsynth/SC_Str4.cpp        # String utilities
server/scsynth/SC_Rate.cpp        # Rate structures
```

---

## Reference: Previous Sync Summary

### Full sync — develop 16be628 (2026-10-08)

Reviewed `7e6f20928..16be628` (12 upstream commits, 3 touching scsynth paths).
Two backported, both applied as upstream patches with paths remapped onto
`dsp/scsynth/synth/`, taken from a shallow clone rather than a remote (see
[Setup](#setup)).

**Applied:**
- `91cc120` RandID on every control block ([PR #6159](https://github.com/supercollider/supercollider/pull/6159)) — 🟡 RandID switched the synth's random generator only when its input changed, so with several RandIDs in one SynthDef the synth stayed on the last one's generator after the first block. `NoiseUGens.cpp`. The class library half of the commit is whitespace only.
- `16be628` `/g_new` always fails on a node ID in use ([PR #7764](https://github.com/supercollider/supercollider/pull/7764)) — 🟢 it used to succeed silently when the group already had the parent asked for, so a client never learned its ID was taken; `addReplace` also drops a redundant `Node_RemoveID`. `SC_MiscCmds.cpp`. Sonic Pi is unaffected: native allocates a fresh ID for every group, and the web runtime creates its studio groups once.

No adaptation was needed.

**Not applicable:**
- `4c9c4ef` terminal client rework ([PR #7747](https://github.com/supercollider/supercollider/pull/7747)) — its one file on the scsynth paths, `common/sc_defer.hpp`, is not carried here.
- `68ba070`, `d81d67f`, `e9c45f1`, `c7bbf31`, `7db41f5`, `73e2c0f`, `28e006b` — sclang and class library.
- `a716d89` — IDE. `0b0dcfc` — issue and PR templates.
- The class library, HelpSource and testsuite parts of `91cc120`.

**Testing:**
- New fixture `randid_two_ids_probe` (`test/synthdefs/compile_randid_synthdefs.scd`), compiled with sclang 3.14.0-rc1: the commit's class library change is whitespace only, so any sclang compiles it the same, and the script reproduces it byte-identical.
- `test/native/test_randid.cpp` (two RandIDs, seeded the same, draw identically on every block) and a duplicate-group case in `test/native/test_error_handling.cpp`. Against the pre-sync engine both fail — the draws differ by up to 1.94, and the second `/g_new` gets no `/fail` — and with the backports both pass.
- `scripts/build-all.sh` (native, NIF, web): built. Native (`scripts/test-native.sh`): 560/560, the 3 device cases not run. NIF: 18 passed. Guest boundary: clean, 174 files.

### Full sync — first since clockwork's extraction (2026-10-01)

Reviewed `b70e7ab7e..7e6f20928` (141 upstream commits, 15 touching scsynth
paths). Four backported, all applied as upstream patches with paths remapped
onto `dsp/scsynth/synth/` (PanUGens by hand, for the Boost-free form).

**Applied:**
- `8a483574d` Median: clip length to 1 ([PR #7672](https://github.com/supercollider/supercollider/pull/7672)) — 🔴 a length below 1 started the insertion at index -1 and wrote outside the median window. `FilterUGens.cpp`.
- `79884629f` PanUGens: `else` before the alignment check — 🟡 `LinPan2`/`XFade2`/`Pan2` chose their 64-sample SIMD path and then overwrote it with the generic aligned one. `PanUGens.cpp`, applied to the Boost-free `((BUFLENGTH & 15) == 0)` form.
- `11193fb00` improve reblocking ([PR #7707](https://github.com/supercollider/supercollider/pull/7707)) — 🟡 a Synth's block size may now exceed the Server's as long as blockSize / resample factor does not (too large is reduced to Server block size × factor), and may be smaller than the factor; wire-buffer offsets are scaled by the Graph's block size in `Graph_Ctor` instead of by the Server's in `DoBufferColoring`; `Graph::mNumTicks`/`mTickCounter` become `uint32`; plugin API version 6 → 7. `SC_Graph.h`, `SC_InterfaceTable.h`, `IOUGens.cpp`, `SC_Graph.cpp`, `SC_GraphDef.cpp`. Nothing else in SuperSonic reads `OutputSpec::mBufferIndex`, and the wire-buffer space is `mMaxWireBufs × mBufLength` as upstream assumes.
- `19954900c` `SC_PlugIn.hpp`: `in`, `zin`, … take `uint32` ([PR #7742](https://github.com/supercollider/supercollider/pull/7742)) — ⚪ type tidy, no behaviour change.

No adaptation was needed: the incoming `scprintf`/`Print` calls go to the host log as they are.

**Not applicable:**
- `25e1ffc99` TCP disconnect deregisters `/notify` clients ([PR #7671](https://github.com/supercollider/supercollider/pull/7671)) — the trigger lives in upstream's `SC_ComPort.cpp`; clockwork owns SuperSonic's sockets and already prunes closed connections from its own notify lists. Clients are identified to the guest by clockwork's `origin`, so a closed TCP origin stays in scsynth's `mUsers` until it sends `/notify 0` — a separate follow-up that would need a clockwork seam, implemented independently.
- `9a070cfe8`, `403a0cb62` — upstream's `SC_WebAudio.cpp` driver (`realTimeMemorySize` passthrough, `try`/`finally` around the reply callback). SuperSonic doesn't build it and already passes `realTimeMemorySize` through (`scsynth_dsp.cpp`).
- `6081aaa63` — sclang class-library platform folders (`SC_Filesystem_*.cpp`).
- `963a8d2d3` — reviewed 2026-06-10.
- `6314d5840`, `0d11c50a3`, `a9c02c442`, `a86e3c411`, `d7801926f`, `558eb939d` — CMake only.
- Supernova, sclang, HelpSource and testsuite parts of the above.

**Testing:**
- New fixtures, compiled with sclang 3.15.0-dev built from `7e6f20928`: `rr_probe_reblock_resample_ctrl` and `rr_audio_reblock_resample_ctrl` (both block size and factor as Synth controls). The nine existing reblock fixtures recompiled byte-identical. `median_length_probe` compiled with sclang 3.14.
- `test/reblock_resample.spec.mjs`: 4 new rate probes (256/2, 512/4, 512/2 → 256, 1/2) and 2 new audio cases (256/2, 512/4). `test/median_length.spec.mjs`: lengths 3, 1, 0, −3. Against the pre-sync engine the Median 0/−3 and the three larger-than-Server block cases fail; 1/2 and the audio cases already passed and guard the new paths.
- Web (Playwright): 1493 passed, 76 skipped, 1 failed — the postMessage "prophet stress: find the breaking point" benchmark, under a load average of ~13; `wasm_benchmark.spec.mjs` passed 6/6 on rerun. Native: 1007/1007 in the per-test run; the shuffled one-process run's only failure was `LinkTempo.kr` reading 60 BPM from a running Sonic Pi on the Link session. NIF: 18 passed. Guest boundary: clean.

---

### Single-commit check — static-plugins build option (2026-06-10)

Reviewed `963a8d2d3` ("build: expose static plugins as a build option",
[PR #7548](https://github.com/supercollider/supercollider/pull/7548)) on
request. **Nothing backported.** Not a full-range sweep — only this commit.

- 5 of 6 files are upstream build system / CI / docs (`CMakeLists.txt`,
  `server/{plugins,scsynth}/CMakeLists.txt`, `.github/workflows/build_wasm.yml`,
  `README_WASM.md`) — all excluded categories.
- `server/scsynth/SC_Lib_Cintf.cpp` (provenance-checked: plain GPL core, ok to
  inspect): upstream switches the STATIC_PLUGINS DiskIO/UIUGens load/unload
  guards from `#ifndef __EMSCRIPTEN__` to capability macros (`NO_LIBSNDFILE`,
  `NO_X11`) driven by new CMake options. **Already present** — SuperSonic
  guards these with `NO_LIBSNDFILE` (dsp/scsynth/synth/server/SC_Lib_Cintf.cpp:74,
  100, 129; `-DNO_LIBSNDFILE` in build-web.sh) and additionally provides no-op
  stubs for `DiskIO_Load`/`DiskIO_Unload`/`UIUGens_Unload` (SC_Stubs.cpp:380),
  implemented independently before this upstream change. The remaining delta
  (a separate `NO_X11` guard for `UIUGens_Unload`) is moot here: the stubs
  make both unloads no-ops on every SuperSonic target.
- PR #7548 is part of upstream's wasm-port work (PR #7428 lineage) — its
  non-core files are not backport sources regardless.

**Testing:** No code changed, so no build/test run required.

---

### No-op sync — upstream's own scsynth.wasm port (2026-06-08)

Reviewed `a27ec6c01..b70e7ab7e` (45 upstream commits). **Nothing backported** — every
scsynth-relevant commit is either already present in SuperSonic (implemented
independently) or not applicable.

The notable finding: the entire batch of "relevant" commits is an **alternative
scsynth.wasm port** ([PR #7428](https://github.com/supercollider/supercollider/pull/7428),
`wasm-audio-worklet`). It targets a WebAudio driver / Emscripten build and takes a
different approach from SuperSonic's AudioWorklet architecture, so its changes don't
apply here.

**Already present (SuperSonic did it independently, its own way):**
- `7a2d326a1` "add endian for wasm" — `SC_Endian.h` already has the `#elif defined(__EMSCRIPTEN__)` block.
- `7e681d089` "remove ReplyAddress.mAddress for wasm" — SuperSonic uses `kWeb` protocol + `uint32_t mAddressPlaceholder[4]` under `__EMSCRIPTEN__` (upstream removes the member entirely; SuperSonic keeps a trivially-copyable placeholder).
- `c68cdbbd3` "fix excessive parameters on init-rate UGen constructors" — the param-drop (`void Foo_Ctor(Unit*)` instead of `(Unit*, int)`, which crashes on wasm via fn-pointer signature mismatch) is **already present** in `DelayUGens.cpp`. ⚠️ **Do NOT cherry-pick this commit**: it also reverts `RadiansPerSample_Ctor` back to `unit->mWorld->mFullRate.mRadiansPerSample`, which would regress the per-Graph reblock/resample lookup (`unit->mParent->mFullRate->mRadiansPerSample`) adopted in the 2026-04-25 sync. This is an ordering artifact — c68cdbbd3 was authored before the reblock PR but merged after it.

**Not applicable (upstream's wasm port / build / dynamic loading):**
- `6dad9cae6` WebAudio driver backend (`SC_WebAudio.cpp`) — SuperSonic excludes this in `build-web.sh`.
- `200abb9da` remove NOVA_TT_PRIORITY_RT / `06223d7f1` add webaudio api — `SC_CoreAudio.cpp/.h` driver path.
- `d331037f6` dummy `main()` — SuperSonic has no main; `scsynth_main.cpp` excluded.
- `4ae4513ea` "add plugins to scsynth wasm build" — wraps DiskIO in `#ifndef __EMSCRIPTEN__`; SuperSonic already excludes DiskIO via no-op stubs in `SC_Stubs.cpp` (`DiskIO_Load`/`DiskIO_Unload`/`UIUGens_Unload`) plus source removal.
- `dc04b92a0` "do not skip .so files" (#7484) — Linux dynamic plugin loading; SuperSonic stripped all dynamic-loading infra (2026-02-25) and builds `-DSTATIC_PLUGINS`.
- `640babcd3` add OscMessageBuilder (`platform/wasm/SC_WebOsc.cpp`) — their port's OSC builder; SuperSonic excludes `SC_WasmOscBuilder.cpp`.
- Plus sclang (new lexer #7394), supernova, CMake, and HelpSource commits in the wider 45.

**Testing:** No code changed, so no build/test run required.

---

### Synth reblocking and upsampling (2026-04-25)

Applied SuperCollider PR #7402 (commit a27ec6c01).

**Feature summary:**
- Per-Synth `Reblock(N)` and `Resample(factor)` — a SynthDef can now run at a different block size and/or sample rate than the server. The factor can be a constant or a Synth Control.
- SynthDef v3 format extended with 4 new fields: `mBlockSize`, `mBlockSizeIndex`, `mResampleFactor`, `mResampleIndex`. Old v3 fixtures (compiled before this PR) are not compatible — the new reader expects these 16 bytes.
- Plugin API version bumped 5 → 6 (upstream notes the bump may be temporary; will likely revert to 4 once 3.15 stabilises).

**Applied:**
- **SC_Graph.h** (header): added `mFlags`, `mNumTicks`, `mTickCounter`, `mFullRate*`, `mBufRate*` fields. Moved `mSubsampleOffset` next to `mSampleOffset`. Preserved SuperSonic's `int` typedef preference (instead of upstream's `int32`).
- **SC_InterfaceTable.h**: bumped `sc_api_version` 5 → 6 with upstream's transitional comment. SuperSonic's `ss_log` decl and `DefineSimpleUnit` macro variant preserved (different region).
- **SC_Unit.h**: `FULLRATE`/`FULLBUFLENGTH`/`FULLSAMPLEDUR` macros now read from `mParent->mFullRate->...` (per-Graph rate) instead of `mWorld->mFullRate.X`. Added `REBLOCK_OR_RESAMPLE` macro. Required for reblock/resample correctness — UGens must use these macros to pick up the local rate.
- **ErrorMessage.hpp**: added API version 4 → "3.15" entry.
- **SC_GraphDef.cpp/.h**: read 4 new fields when `inVersion > 2`, default to `(0, 0, 1.0, 0)` for older versions.
- **SC_Prototypes.h** + **SC_Unit.cpp**: `Unit_New` now takes `Graph* graph`. The unit's `mRate` is now `graph->mFullRate` or `graph->mBufRate` (per-Graph) instead of `inUnitSpec->mRateInfo` (global). Note `mRateInfo` is still set up in `SC_GraphDef.cpp` but is now effectively dead code.
- **SC_Graph.cpp**: new `Graph_Ctor` block (~100 lines) reads `mBlockSize`/`mResampleFactor` from the GraphDef (or from a Synth Control if they're negative), allocates per-Graph `Rate*` via `World_Alloc` only when reblock/resample is requested, otherwise points to `inWorld->mFullRate`/`mBufRate`. `Graph_Dtor` frees the allocated rates if non-shared. `Graph_Calc` and `Graph_CalcTrace` wrap their existing loops in an outer `for k < numTicks` to drive sub-block ticks. `Graph_New` reformatted (signature on one line, comment moved out). SuperSonic's `Graph_InitUnits` split, `ss_log` substitutions, and `[Graph_New] ERROR` log all preserved.
- **SC_MiscCmds.cpp**: factored `meth_s_new` and `meth_s_newargs` into a shared `meth_s_do_new(...)` per upstream. SuperSonic's eager `Graph_InitUnits(graph)` call (commit `7438a8fe2`) moved into the unified function — both `s_new` and `s_newargs` still get eager init, now from a single call site. Note: upstream removed the `Node_RemoveID(replaceThisNode)` call from case 4 (replace) — `Node_Replace → Node_Delete → Graph_Dtor → Node_Dtor` still emits `kNode_End` with the original node ID, which is what the SAB mirror expects.
- **DelayUGens.cpp**: `RadiansPerSample_Ctor` now reads `unit->mParent->mFullRate->mRadiansPerSample` instead of `mWorld->mFullRate.mRadiansPerSample`. SuperSonic's ctor signature (no `inNumSamples`) preserved.
- **IOUGens.cpp**: 1091-line rewrite — every IO UGen (`In`, `Out`, `XOut`, `OffsetOut`, `ReplaceOut`, `LocalIn`, `LocalOut`, `AudioControl`, `LagControl`, `LagIn`, `InFeedback`, `InTrig`, `SharedIn`, `SharedOut`) gains a `_reblock` variant and per-channel `m_busTouchedCache` for first-tick buffer state. AudioBusGuard improved (privatised + `isValid()`). SuperSonic's `extern "C"` before `PluginLoad(IO)` preserved.

**v3 SynthDef fixtures recompiled:**
- `test/synthdefs/versions/test_simple_v3.scsyndef` (214 → 230 bytes)
- `test/synthdefs/versions/test_multi_v3.scsyndef` (514 → 530 bytes)
- `packages/supersonic-scsynth-synthdefs/synthdefs/u_cmd_test.scsyndef` (130 → 146 bytes)
- `packages/supersonic-scsynth-synthdefs/synthdefs/number.scsyndef` (106 → 122 bytes)

Each gained 16 bytes per def: `00 00 00 00` (mBlockSize=0) + `00 00 00 00` (mBlockSizeIndex=0) + `3F 80 00 00` (mResampleFactor=1.0 BE) + `00 00 00 00` (mResampleIndex=0). The v3 per-def size header was bumped accordingly. Compiled with sclang built from supercollider commit `a27ec6c01`.

**WASM Adaptations:**
- New error/warning prints in `Graph_Ctor` (block size out of range, resample factor not power-of-two, etc.) use `ss_log` instead of upstream's `scprintf`.
- `Graph_CalcTrace` debug output uses `ss_log`.

**Skipped (not applicable):**
- `SCClassLibrary/Common/Audio/SynthDef.sc`, `Reblock.sc`, `Resample.sc` — sclang only.
- `HelpSource/` schelp files — not in SuperSonic.
- `testsuite/classlibrary/TestReblock.sc`, `TestResample.sc` — sclang test suite.
- `server/supernova/sc/*` — supernova not in SuperSonic.

**Testing:**
- Native: 6480 assertions, 579 test cases passed.
- Web (Playwright): 1358 passed, 60 skipped, 0 failed across SAB and postMessage modes (pre-feature-tests).
- Existing v3 synthdef tests (`test/synthdef_versions.spec.mjs`) verified that the new format reads correctly.
- New `test/reblock_resample.spec.mjs` (22 tests across SAB+PM, 1 skipped in PM where audio capture is unavailable). Probe-based: each fixture writes `BlockSize.ir` and `SampleRate.ir` to two control buses, the test reads them via `/c_get` and asserts the *observed* per-Graph rate matches the requested Reblock/Resample. This is what proves the feature is actually wired up — a no-op Reblock would still produce audio, but only the rate observation distinguishes "feature is active" from "feature was silently ignored". Coverage:
  - 6 constant configurations (baseline, Reblock(32), Reblock(64), Resample(2), Resample(4), Reblock(32)+Resample(2))
  - 2 control-driven Reblock cases (default + override via `/s_new` args)
  - 2 control-driven Resample cases (default + override)
  - 1 audio-output smoke test for combined Reblock+Resample (SAB only)
- Fixtures generated by `test/synthdefs/compile_reblock_resample_synthdefs.scd` and committed to `test/synthdefs/reblock_resample/`.

**Upstream:**
- https://github.com/supercollider/supercollider/commit/a27ec6c01ac78fba2967c136ba8cfc94414d1c61
- https://github.com/supercollider/supercollider/pull/7402

---

### Convolution UGen fixes (2026-03-18)

Applied SuperCollider PR #7418 (commit b365366bc).

**Applied:**
- **Convolution.cpp**: `ConvGetBuffer()` changed `uint32 bufnum` parameter to `int32` — negative buffer numbers (invalid) previously wrapped to huge positive values and indexed out of bounds. Added explicit negative check. Also changed `uint32` to `int32` for buffer number variables throughout Convolution2, Convolution2L, StereoConvolution2L, and Convolution3 constructors and next functions. Added missing null check after `ConvGetBuffer()` in `Convolution3_next_a`.

**Skipped:**
- Test file changes (`testsuite/classlibrary/TestUGen_RTAlloc.sc`) — not applicable

**Upstream:**
- https://github.com/supercollider/supercollider/commit/b365366bc
- https://github.com/supercollider/supercollider/pull/7418

---

### ISPOWEROFTWO fix for zero (2026-03-14)

Applied SuperCollider PR #7409 (commit d991af0b8).

**Applied:**
- **clz.h**: `ISPOWEROFTWO(0)` now correctly returns false. The previous implementation `(x & (x-1)) == 0` returned true for 0, which is not a power of two.

**Upstream:**
- https://github.com/supercollider/supercollider/commit/d991af0b8642eab4ca3e114b5e2d2c7ccfbe86d3
- https://github.com/supercollider/supercollider/pull/7409

---

### Plugin command and unit command extensions (2026-03-12)

Applied SuperCollider PR #7405 (commit 613f26f9) which adds extended plugin commands, async unit commands, and Graph refcounting.

**New Plugin API (v4 → v5):**
- `DoAsynchronousCommandEx` — async plugin commands with reply address passed to stage functions
- `DefineUnitCmdEx` — unit commands with reply address (for async unit commands)
- `DoAsyncUnitCommand` — async unit commands with Graph refcounting to keep the graph alive

**Applied:**
- **SC_Command.h** (NEW): Central typedef declarations for command function types, extracted from multiple headers
- **SC_InterfaceTable.h**: API version bumped to 5, added function pointers and macros for new functions
- **SC_Graph.h**: Added `int32 mRefCount` to Graph struct
- **SC_Graph.cpp**: Added `Graph_AddRef`, `Graph_Release`, `Graph_Delete`, `Graph_HasParent`. Made `Graph_Dtor` static. Added `mRefCount = 1` in Graph_Ctor. Updated `Graph_QueueUnitCmd` to pass ReplyAddress
- **SC_Node.cpp**: Added null-parent guard in `Node_Remove`. Changed `Node_Delete` to call `Graph_Delete` instead of `Graph_Dtor`
- **SC_UnitDef.h**: `UnitCmd` struct now uses union for `mFunc`/`mFuncEx` with `mHasFuncEx` bool
- **SC_UnitDef.cpp**: Templated `UnitDef_DoAddCmd`. Added `UnitDef_AddCmdEx`, `Unit_RunCommand`. Updated `Unit_DoCmd` to pass ReplyAddress
- **SC_Prototypes.h**: Updated declarations for new graph functions and async command functions
- **SC_SequencedCommand.h**: Templated `AsyncPlugInCmd_<StageFn>` with `if constexpr` stage dispatch. Added `AsyncUnitCmd` class with Graph refcounting
- **SC_SequencedCommand.cpp**: Added `PerformAsynchronousCommandEx` and `PerformAsyncUnitCommand`. Full `AsyncUnitCmd` implementation
- **SC_MiscCmds.cpp**: `meth_u_cmd` now passes reply address to `Unit_DoCmd`
- **SC_World.cpp**: Registered new function pointers in interface table
- **SC_GraphDef.h**: Removed redundant typedef
- **SC_Lib_Cintf.cpp**: Added DemoUGens plugin loading
- **DemoUGens.cpp**: Updated to match upstream — uses `DoAsynchronousCommandEx`, adds `UnitCmdDemo` with async `testCommand`, `DefineDtorUnit`, destructor with `SendMsgFromRT`

**WASM Adaptations:**
- `ss_log` used for error messages in `PerformAsyncUnitCommand` (instead of upstream `scprintf`)
- `DemoUGens_Load` declared outside `extern "C"` block in `SC_Lib_Cintf.cpp` to match C++ linkage of `PluginLoad()` macro
- In WASM NRT mode (`mRealTime=false`), `CallEveryStage()` processes all async command stages synchronously — no RT/NRT thread interleaving. This means concurrent synth free during async commands cannot happen

**Testing:**
- Compiled `u_cmd_test` and `number` test synthdefs using sclang 3.15.0-dev (commit 613f26f9)
- 24 new Playwright tests (12 per mode) across SAB and postMessage covering:
  - Async plugin commands: success with /done, failure with no /done, minimal args
  - Synchronous unit commands: setValue queued and non-queued
  - Async unit commands: success /done, failure no /done, concurrent free safety, multiple commands
  - Graph refcounting: null parent guard, ID reuse after async free, rapid create-command-free cycles

**Upstream:**
- https://github.com/supercollider/supercollider/commit/613f26f9549693f1d3032145048f1fc14a863981
- https://github.com/supercollider/supercollider/pull/7405

---

### SynthDef v3 format support (2026-03-09)

Applied SuperCollider PR #7395 (commit 99be55460) which introduces SynthDef version 3 and substantially improves the GraphDef parser.

**Format Change:**
- v3 adds a 4-byte size field before each synthdef definition, enabling forward-compatible parsing. Parsers can skip unknown fields in future versions without breaking.

**Applied:**
- **ReadWriteMacros.h**: All buffer-based read functions now take `const char*& buf, const char* end` and perform bounds checking via `checkBufferSpace()`. Fixes UB on truncated data. Return types corrected (e.g. `readInt8` returns `int8` not `int32`).
- **SC_GraphDef.cpp**: Major refactor:
  - Unified `GraphDef_Read`/`GraphDef_ReadVer1` into single version-aware `GraphDef_Read` with `readCount()` helper
  - Unified `calcParamSpecs`/`calcParamSpecs1`, `ParamSpec_Read`/`ParamSpec_ReadVer1`, `InputSpec_Read`/`InputSpec_ReadVer1`, `UnitSpec_Read`/`UnitSpec_ReadVer1`
  - v3 support in `GraphDefLib_Read` with per-definition size field parsing
  - `GraphDefLib_Read` and `GraphDef_Recv` signatures changed to take `const char* buffer, size_t size`
  - `malloc`/`calloc`/`free` replaced with `new`/`delete` throughout
  - `std::unique_ptr` with custom deleter for exception-safe GraphDef cleanup
  - `BufColorAllocator` modernized from raw arrays to `std::vector`
  - Throws on invalid magic/version instead of silently returning
- **SC_GraphDef.h**: `GraphDef_Recv` signature updated (adds `size_t size` parameter)
- **SC_SequencedCommand.h**: Added `mSize` field to `RecvSynthDefCmd`
- **SC_SequencedCommand.cpp**: `RecvSynthDefCmd` stores and passes buffer size through to `GraphDef_Recv`

**JS Changes:**
- **js/lib/synthdef_parser.js**: `extractSynthDefName` updated to handle v3 format (name offset is 14 instead of 10 due to size field prefix)

**WASM Adaptations:**
- Kept `ss_log` instead of upstream `scprintf`
- Kept `g_lastGraphDefError` static for Emscripten exception handling
- Kept `std::string* outErrorMsg` parameter on `GraphDef_Recv` for error reporting to JS

**Filesystem code guarded:**
- Wrapped filesystem-based synthdef loading in `#ifndef __EMSCRIPTEN__`: `load_file`, `GraphDef_Load`, `GraphDef_LoadDir`, `GraphDef_LoadGlob` (SC_GraphDef.cpp/.h), `LoadSynthDefCmd`, `LoadSynthDefDirCmd` (SC_SequencedCommand.h/.cpp), `meth_d_load`, `meth_d_loadDir` (SC_MiscCmds.cpp), `World_LoadGraphDefs` body (SC_World.cpp)
- Code inside guards matches upstream exactly (uses `scprintf`, not `ss_log`) for easy future syncing

**Bug Fix:**
- Bounds checking on all buffer reads now prevents server crashes from truncated/corrupted synthdef data. Previously, truncated v2 synthdefs could crash the WASM server by reading past buffer boundaries.

**Testing:**
- Compiled v1, v2, and v3 test fixtures using sclang 3.15.0-dev (commit 99be55460)
- 36 new Playwright tests across SAB and postMessage modes covering:
  - Loading and name extraction for all versions
  - Synth creation and control for all versions
  - Synthdef count verification for all versions
  - Corruption resilience (truncated data) for all versions

**Upstream:**
- https://github.com/supercollider/supercollider/commit/99be55460
- https://github.com/supercollider/supercollider/pull/7395

---

### World_Cleanup ordering fix + dead code removal (2026-02-25)

Reviewed upstream commits 5076833 and 71813a9.

**Applied:**
- **SC_World.cpp**: Moved `deinitialize_library()` call from before `Group_DeleteAll()` to after it, matching upstream commit 5076833. This fixes a bug where plugins could be unloaded while nodes still reference them during cleanup.
- **SC_Lib_Cintf.cpp**: Removed ~150 lines of dead dynamic-loading code (`PlugIn_Load`, `PlugIn_LoadDir`, `checkAPIVersion`, `checkServerVersion`, `open_handles`, directory scanning, Apple mach-o preloading). SuperSonic always compiles with `-DSTATIC_PLUGINS` so none of this code could execute. Also removed unused includes (`dirent.h`, `dlfcn.h`, `libgen.h`, `filesystem`, `iostream`, etc.).

**Skipped (not applicable to WASM):**
- Plugin loading rework (71813a9) — dynamic loading infrastructure changes
- CoreAudio boost→std::optional migration — platform-specific audio driver code

**Upstream commits:**
- https://github.com/supercollider/supercollider/commit/5076833
- https://github.com/supercollider/supercollider/commit/71813a9

---

### Plugin API v4 Update (2026-01-25)

Applied SuperCollider PR #7329 which converts the Plugin API from C++ to C:

**Key Changes:**
- API version bumped from 3 to 4
- Replaced C++ `SCFFT_Allocator` virtual class with C struct using function pointers
- Changed `bool` to `SCBool` typedef (uint8_t in C, bool in C++)
- Changed `int` to `int32` for explicit sizing
- Changed C++ references to C pointers (`FifoMsg&` → `FifoMsg*`, `ScopeBufferHnd&` → `ScopeBufferHnd*`, `SCFFT_Allocator&` → `SCFFT_Allocator*`)
- Removed `bool mUnused0` field from InterfaceTable
- Added `SC_INLINE` macro for C/C++ compatible inline functions
- Added `kSCTrue`/`kSCFalse` enum constants

**Files Modified:**
- `dsp/scsynth/synth/include/common/SC_Types.h`
- `dsp/scsynth/synth/include/common/SC_fftlib.h`
- `dsp/scsynth/synth/common/SC_fftlib.hpp` (new - private header)
- `dsp/scsynth/synth/common/SC_fftlib.cpp`
- `dsp/scsynth/synth/include/plugin_interface/SC_InterfaceTable.h`
- `dsp/scsynth/synth/include/plugin_interface/SC_World.h`
- `dsp/scsynth/synth/include/plugin_interface/SC_Unit.h`
- `dsp/scsynth/synth/include/plugin_interface/SC_FifoMsg.h`
- `dsp/scsynth/synth/server/SC_World.cpp`
- `dsp/scsynth/synth/server/SC_Prototypes.h`
- `dsp/scsynth/synth/server/SC_SequencedCommand.h`
- `dsp/scsynth/synth/server/SC_SequencedCommand.cpp`
- `dsp/scsynth/synth/plugins/DelayUGens.cpp`

**Benefits for WASM:**
- Eliminates C++ vtable overhead for FFT allocator
- Simpler function pointer dispatch (better for WASM indirect calls)
- C-compatible plugin interface enables future C plugin support

**Upstream PR:** https://github.com/supercollider/supercollider/pull/7329
**Commit:** 765ac9982ebca76079f75db9555a940fa2e8f35c

---

### Verification: SuperCollider 3.14.1 (2025-11-24)

Verified SuperSonic is aligned with SuperCollider 3.14.1 (released 2025-11-23):

**Analysis:**
- Examined all commits between last sync (2025-09-25) and Version-3.14.1 tag
- Found 3 server directory commits:
  1. fdd4db02f1 - portaudio CMake cleanup (build system only - skipped)
  2. 04a9350261 - Boost CMake integration (build system only - skipped)
  3. 66b74c05f9 - /g_dumpTree end delimiter (already applied with ss_log adaptation)

**3.14.1 Release Content:**
- Single sclang fix: keyword arguments crash (PR #7206)
- No scsynth or plugin changes since 3.14.1-rc2
- Not applicable to SuperSonic (sclang-only)

**Conclusion:** No changes needed. SuperSonic is current with all relevant upstream changes.

### Last major sync (2025-10-20)

Applied:
- 20 new commits
- 9 commits found already applied
- 13 commits marked not applicable

Key fixes included:
- EnvGen timing bug (counter_fractional)
- 16 UGen initialization fixes
- Server infrastructure (/g_dumpTree delimiter)
- Code quality improvements (Fill→Clear, macros)

See `BACKPORT_COMPLETION_SUMMARY.md` for full details.

---

## Tips for Future Maintainers

1. **Batch related fixes together** - Apply all initialization fixes in one session
2. **Trust but verify** - Even if commit message matches, check the actual code
3. **Document adaptations** - Note every change made to fit the guest (Boost-free conversions, `#ifdef CLOCKWORK_GUEST` blocks)
4. **Test critical fixes** - EnvGen, triggers, delays are high-risk areas
5. **Keep upstream links** - Future maintainers need to trace back to original PRs
6. **Update this guide** - Add new patterns, pitfalls, or workflow improvements
7. **Use `cherry-pick --no-commit` (or a path-remapped `git apply`) when possible** - Applies the upstream change exactly; the maintainer's commit carries the attribution
8. **Never commit** - Leave the sync uncommitted for the maintainer to review and commit

---

## Intentionally Excluded Features

Some upstream features are excluded due to AudioWorklet constraints:

- ❌ **Link UGens** (`server/plugins/LinkUGens.cpp`) - requires network sockets for tempo sync ([PR #6947](https://github.com/supercollider/supercollider/pull/6947))
- ❌ **Threading UGens** - no thread spawning in AudioWorklet
- ❌ **Disk I/O UGens** (DiskIn, DiskOut, VDiskIn) - no filesystem access
- ❌ **OSC Network UGens** (SendReply via UDP) - no network sockets

## Preprocessor Conventions for Upstream Files

SuperSonic uses two preprocessor guards in upstream scsynth files. Each serves a distinct purpose for upstream sync.

### `#ifdef CLOCKWORK_GUEST` — Fork Divergence

Marks where SuperSonic intentionally diverges from upstream scsynth. The original upstream code is preserved in the `#else` branch for merge reference. `CLOCKWORK_GUEST` is defined for every target by `dsp/scsynth/CMakeLists.txt` (it replaced the old `SUPERSONIC` guard when clockwork was extracted).

```bash
# Find all fork divergence points
grep -rn "CLOCKWORK_GUEST" dsp/scsynth/synth/
```

**During upstream syncs:** Update the `#else` branch to match upstream. Then check whether the `CLOCKWORK_GUEST` branch needs corresponding changes.

Current `CLOCKWORK_GUEST` sites in upstream files:

- **SC_Constants.h**: `constexpr` constants (upstream uses runtime `const` with `std::acos` etc.)
- **SC_World.cpp**: `InitializeSynthTables`/`InitializeFFTTables` declarations and calls in `World_New`; the guest's doors to the host (`supersonic_guest_*` in `scsynth_dsp.cpp`) and `sc_malloc`/`sc_free` on the host's heap
- **SC_fftlib.cpp**: idempotency guard in `scfft_global_initialization`, `InitializeFFTTables` entry point
- **Samp.cpp**: idempotency guard in `FillTables`, `InitializeSynthTables` entry point
- **SC_InterfaceTable.h**: `DefineSimpleUnit` macro (without trailing semicolon)
- **SC_Lib.cpp**: direct `SendFailure` error reporting (upstream uses staged `CallSendFailureCommand`)
- **SC_ReplyImpl.hpp**: `kWeb` protocol enum value
- **SC_OSC_Commands.h**: `cmd_b_allocPtr` and `cmd_supersonic_piano_wavetable` command numbers
- **LFUGens.cpp**: signed squared/cubed envelope warps (sonic-pi#169) + zero-safe exponential warp endpoints (sonic-pi#881) — `sc_signed_sqrt`/`sc_signed_square`/`sc_exp_safe` helpers, EnvGen segment init/next/fill, duplicated `GET_ENV_VAL` macro
- **DemandUGens.cpp**: signed squared/cubed + zero-safe exponential envelope warps — helpers + demand-rate envelope init/next (sonic-pi#169, sonic-pi#881)
- Empty `#ifdef CLOCKWORK_GUEST` blocks in SC_Graph.cpp, SC_Lib.cpp, SC_MiscCmds.cpp and SC_fftlib.cpp are where the old `ss_log` declarations were.

### Boost-free conversions — global, unconditional divergence

SuperSonic carries **no Boost** (the vendored bcp subset was removed 2026-08-04;
there is nothing for a `boost/...` include to resolve against). Unlike the
`#ifdef CLOCKWORK_GUEST` sites above, these divergences are deliberately
**unconditional** — an `#else` branch preserving the upstream Boost code could
never compile here, so this table is the merge reference instead.

**During upstream syncs:** apply these mechanical conversions to any incoming
upstream code that uses them:

| Upstream (Boost) | SuperSonic replacement |
|---|---|
| `#include <boost/align/is_aligned.hpp>` + `boost::alignment::is_aligned(BUFLENGTH, 16)` | `((BUFLENGTH & 15) == 0)` (no include) |
| `boost::optional<T>` / `<boost/optional.hpp>` | `std::optional<T>` / `<optional>` |
| `boost::enable_if_c<C, T>::type` / `boost::disable_if_c<C, T>::type` | `std::enable_if<C, T>::type` / `std::enable_if<!C, T>::type` (`<type_traits>`) |
| `<boost/predef/hardware.h>` + `BOOST_HW_SIMD_X86 >= BOOST_HW_SIMD_X86_SSE_VERSION` | `defined(__SSE__) \|\| defined(_M_X64) \|\| (defined(_M_IX86_FP) && _M_IX86_FP >= 1)` |
| `boost::sync::semaphore` (via the old NativeShim stub) | `sc::sync::semaphore` — no-op stub in `dsp/scsynth/synth/include/common/SC_QuitSemaphore.hpp` |
| `boost::alignment::aligned_alloc/aligned_free` (in `malloc_aligned.hpp`) | `_aligned_malloc/_aligned_free` (Windows) / `posix_memalign` + `free` (elsewhere) |
| `boost::iequals` (in `SC_SndFileHelpers.hpp`) | local ASCII `iequals` helper defined in the same file |

Files converted (2026-08-04): `plugins/{Pan,Trigger,BinaryOp,Delay,LF,IO,UnaryOp}UGens.cpp`,
`server/SC_CoreAudio.h` + `include/server/SC_CoreAudio.h`, `server/SC_World.cpp`,
`server/SC_HiddenWorld.h`, `common/malloc_aligned.hpp`, `common/SC_SndFileHelpers.hpp`.

Never reintroduce a `boost/` include into `dsp/scsynth/synth` — the build has no
resolver for it, and keeping the engine Boost-free is part of keeping the
dependency surface auditable (see the licence boundary above).

### `#ifndef __EMSCRIPTEN__` — Platform Capability

Guards upstream code that requires APIs unavailable in WASM (filesystem, shared memory IPC, threading primitives). These are NOT SuperSonic-specific changes — they're platform exclusions.

```bash
# Find all platform guards
grep -rn "__EMSCRIPTEN__" dsp/scsynth/
```

**During upstream syncs:** Update the guarded code to match upstream exactly. Don't skip these blocks — they contain the upstream code that native builds use.

Current `__EMSCRIPTEN__` sites in upstream files:

- **SC_GraphDef.cpp/.h**: `load_file()`, `GraphDef_Load()`, `GraphDef_LoadDir()`, `GraphDef_LoadGlob()`
- **SC_CoreAudio.h** (both copies): `SC_AUDIO_API_WEBAUDIO` selection
- **SC_Endian.h**, **SC_Platform.h**: wasm endianness and `SCP_TARGET_WASM`
- **SC_Filesystem_unix.cpp**: compiled for wasm as for Linux
- **SC_OscUtils.hpp**: `kWeb` in the reply-address dump
- **MdaUGens.cpp**: `EMSCRIPTEN_KEEPALIVE` export

SuperSonic's own files use it too: **SC_Stubs.cpp** (native-only stubs) and **scsynth_dsp.cpp**.

---

## Questions?

If uncertain about a commit:

1. **Check the upstream PR** - Often has discussion about scope/impact
2. **Ask on SuperCollider forums** - Community can clarify intent
3. **When in doubt about a _fix_, apply it** - Easier to revert than to miss a critical fix. **But never put upstream code in clockwork** - see the [License boundary](#license-boundary-upstream-code-goes-into-supersonic-never-into-clockwork).
4. **Test in browser** - Some issues only manifest in WASM environment

---

**Last Updated**: 2026-10-08
**Maintainer**: See git log for recent contributors
**Upstream**: https://github.com/supercollider/supercollider
