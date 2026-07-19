# Supersonic ↔ SuperCollider Upstream Sync Guide

**Last Updated**: 2026-06-08
**Last Sync Commit**: b70e7ab7e
**Upstream Branch**: supercollider/develop (tracked to 2026-06-08)
**Verified Against**: SuperCollider 3.15.0-dev (develop HEAD b70e7ab7e)

---

## Overview

Supersonic is a WASM port of SuperCollider's scsynth audio server. It originated from [SuperCollider PR #6569](https://github.com/supercollider/supercollider/pull/6569) but has diverged. This guide explains how to identify and backport relevant upstream changes from SuperCollider to supersonic.

---

## License boundary: AGPL code stays out of SuperSonic

**Read this before every sync.**

### Why it matters

- SuperSonic's own code (ingress/egress, SuperClock, scheduler, MIDI, the JS platform layer) is **`MIT OR GPL-3.0-or-later`** — deliberately permissive, so it can be embedded and redistributed without network-copyleft obligations.
- The forked scsynth *core* is **GPL-3.0-or-later** (inherited from upstream). This is the expected licence to keep syncing.
- Upstream's **WASM port** ([PR #7428](https://github.com/supercollider/supercollider/pull/7428), `wasm-audio-worklet`) is licensed **GNU Affero GPL v3 (AGPL-3.0-or-later)**, which is broader than the GPL-3.0 core.

AGPL §13 ("Remote Network Interaction") requires that anyone who lets users interact with the software **over a network** be offered its complete corresponding source. A browser-delivered audio engine meets that description. GPL-3.0 → AGPL-3.0 compatibility is **one-way**: incorporating any AGPL code places the **combined work** under AGPL. That would extend the network-source-offer requirement to everyone who deploys it and change the platform layer's effective licence from MIT/GPL to AGPL. The change applies to every copy already distributed, so it cannot be undone after a release. Keeping the two licence domains separate is therefore a release-time decision, made here, on the way in.

### The rule — it covers parts, not just whole files

> **Do not copy, cherry-pick, adapt, paraphrase, "port", or otherwise derive any
> code — a whole file OR a single fragment — from an AGPL / Affero-licensed
> upstream source. Copyright covers derivative works: manually re-typing or
> reworking an AGPL snippet into one of our GPL/MIT files still produces
> AGPL-licensed code, even without the AGPL header.**

The reliable approach is to **not use an AGPL upstream file as a backport source
at all.** The decision is made at the **source, before you read it** — not by
scanning the result afterwards. If you need equivalent behaviour, implement it
independently from the OSC spec / observable behaviour, with **no reference** to
the AGPL source.

> **[Manual Application](#option-b-manual-application) is the path where AGPL code
> could be introduced unintentionally.** "Read the upstream change, apply by hand"
> is how an AGPL fragment can end up retyped into a GPL file without carrying the
> AGPL header. No automated scan of this repo detects that — checking the source's
> licence before adapting it is what keeps it out.

### Known AGPL sources in upstream — do not use as backport sources

All from PR #7428 (upstream's *alternative* wasm port). SuperSonic already has its
own MIT-or-GPL callback-driven driver + OSC ingress, so none of these are needed:

| Upstream path | What it is | Added by |
|---|---|---|
| `server/scsynth/SC_WebAudio.cpp` | AGPL WebAudio driver backend. Note: it lives in `server/scsynth/`, an INCLUDE path, so it appears in Step 2 `--since` discovery — leave it out; do not adapt it. | `6dad9cae6` |
| `platform/wasm/SC_WebOsc.cpp` | AGPL OSC message builder | `640babcd3` |
| `platform/wasm/LICENSE` | The AGPL-3.0 licence text covering everything under `platform/wasm/` | PR #7428 |
| `platform/wasm/**` (`init.js`, `index.html`, pre-js, `README_WASM.md`, …) | Upstream's wasm demo / build glue — all AGPL-covered | PR #7428 |

**Re-verify this list every sync** — upstream may add more AGPL under these paths.

> **Not AGPL (note):** `COPYING`, `external_libraries/hidapi/LICENSE-gpl3.txt`,
> and ordinary GPL-3.0 file headers contain the word "Affero" because **GPL-3.0 §13
> *references* the AGPL**. A file is AGPL only if its own header says it is *licensed
> under* the GNU Affero General Public License.

### Checks — one primary, one backstop

1. **Provenance check, BEFORE adapting anything (the primary check).** Run this on
   the upstream SOURCE path before you open it, for cherry-pick *and* manual application:
   ```bash
   git show <upstream-commit>:<path> | grep -iqE "affero|AGPL" \
     && echo "AGPL source — leave it out; implement independently if needed" \
     || echo "ok to inspect"
   ```

2. **Whole-file backstop (limited).** Catches only a verbatim or header-preserving
   copy of an entire AGPL file. It does **not** catch fragments retyped into existing
   files, reworked code, or a copy with the header removed. Wire it into CI so a whole
   AGPL file does not land unnoticed:
   ```bash
   # Must return nothing. A hit = a whole AGPL file is in the tree.
   grep -rlI -e "Affero" -e "AGPL" src/ | grep -vE "GPL-3\.0|§13"
   ```

Check (2) only covers a whole copied file. Fragments and adaptations are covered by
check (1) and the rule above: do not use an AGPL file as a source in the first place.

---

## Quick Start Checklist

When syncing with upstream:

- [ ] **FIRST — read the [License boundary](#license-boundary-agpl-code-stays-out-of-supersonic): never derive from AGPL/Affero upstream code, whole files OR fragments**
- [ ] Fetch latest upstream changes
- [ ] Identify scsynth-relevant commits since last sync
- [ ] Filter out non-applicable changes (sclang, supernova, threads, tests)
- [ ] Check if changes are already applied
- [ ] Cherry-pick or manually apply changes
- [ ] Adapt for WASM (ss_log, no threads)
- [ ] Commit with proper attribution
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

```bash
# Replace YYYY-MM-DD with last sync date (check top of this file)
LAST_SYNC="2025-09-25"

# View all upstream commits since then
git log supercollider/develop --oneline --since="$LAST_SYNC"
```

### Step 2: Filter for scsynth-Relevant Paths

Focus on these paths ONLY:

```bash
# Server core files
git log supercollider/develop --oneline --since="$LAST_SYNC" -- \
    "server/scsynth/*.cpp" \
    "server/scsynth/*.h" \
    "include/server/*.h" \
    "include/plugin_interface/*.h"

# Plugin files
git log supercollider/develop --oneline --since="$LAST_SYNC" -- \
    "server/plugins/*.cpp"
```

### Step 3: Exclude Non-Applicable Changes

**Always SKIP these**:
- ❌ **ANY AGPL / Affero-licensed source — whole file OR fragment (see the [License boundary](#license-boundary-agpl-code-stays-out-of-supersonic)). Covers all of PR #7428's WASM port: `server/scsynth/SC_WebAudio.cpp`, `platform/wasm/**`. Check the source's license header BEFORE you open it to adapt it.**
- ❌ sclang changes (`lang/`, `SCClassLibrary/`)
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
grep -n "counter_fractional" src/synth/plugins/LFUGens.cpp
```

### Method 3: Check Function Signatures

```bash
# Example: checking if World_TotalFree exists (for rtMemoryStatus)
grep -r "World_TotalFree" src/synth/
```

**Always verify by reading code** - commit messages can be misleading!

---

## Applying Changes

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
   cat src/synth/plugins/LFUGens.cpp
   ```

3. **Apply changes manually** using the Edit tool

4. **Adapt for WASM** (see next section)

---

## Adapting Upstream Changes

SuperSonic has three build targets (WASM, native, NIF) with a shared engine. Use the ifdef conventions above to mark adaptations clearly.

### 1. Print Functions — use `#ifdef SUPERSONIC`

```cpp
#ifdef SUPERSONIC
    ss_log("debug message\n");
#else
    scprintf("debug message\n");
#endif
```

### 2. Platform-unavailable APIs — use `#ifndef __EMSCRIPTEN__`

For filesystem, boost headers, shared memory IPC — keep the upstream code in the `#ifndef` block exactly as-is.

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

Common conflicts when cherry-picking:

### 1. Help Files (.schelp)

```bash
# Supersonic doesn't include help files
git rm -f HelpSource/**/*.schelp
git rm -f SCClassLibrary/**/*.sc

# Continue with the cherry-pick
git cherry-pick --continue
```

### 2. Test Files

```bash
# Supersonic doesn't include test suite
git rm -f testsuite/**/*.sc
git rm -f testsuite/**/*.scd

# Continue
git cherry-pick --continue
```

### 3. Missing Files

If a commit modifies a file that doesn't exist in supersonic:

```bash
# Check if the plugin exists
ls src/synth/plugins/ | grep "PluginName"

# If it doesn't exist, skip this part
git rm -f server/plugins/PluginName.cpp

# Or abort the cherry-pick if entire commit is not applicable
git cherry-pick --abort
```

### 4. Staging Already-Modified Files

```bash
# If you have unstaged changes that need to be included
git add src/synth/server/SC_GraphDef.cpp

# Then continue
git cherry-pick --continue
```

---

## Commit Message Format

Use this format for all backported commits:

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

- [ ] **No AGPL code introduced — every upstream source you adapted was non-AGPL (provenance checked at the source, per the [License boundary](#license-boundary-agpl-code-stays-out-of-supersonic)); the `grep -rlI -e Affero -e AGPL src/` backstop shows nothing new (backstop only — it does not catch retyped fragments)**
- [ ] Code compiles on all targets (`scripts/build-web.sh`, `scripts/build-native.sh`)
- [ ] SuperSonic-specific changes wrapped in `#ifdef SUPERSONIC` with upstream code in `#else`
- [ ] Platform guards use `#ifndef __EMSCRIPTEN__` (not `#ifdef SUPERSONIC`)
- [ ] No bare `scprintf` in shared code — use `ss_log` inside `#ifdef SUPERSONIC`
- [ ] No malloc/free on audio thread paths
- [ ] Git commit includes upstream hash and link
- [ ] Commit message explains adaptations (if any)
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

# 2. Find commits since last sync
LAST_SYNC="2025-09-25"
git log supercollider/develop --oneline --since="$LAST_SYNC" \
    -- "server/plugins/*.cpp" "server/scsynth/*.cpp" \
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

# 8. Adapt for WASM if needed (check diff)
git diff --cached

# 9. Edit files if needed to replace scprintf with ss_log
# Use Edit tool here

# 10. Commit with proper message
git commit -m "plugins: Fix initialization

Backported from SuperCollider upstream commit $COMMIT
https://github.com/supercollider/supercollider/commit/$COMMIT

[description]"

# 11. Repeat for next commit
```

---

## Troubleshooting

### Problem: Cherry-pick creates empty commit

```bash
# This means the change is already applied
git cherry-pick --abort

# Verify by checking the code
grep -n "the_fix" src/synth/plugins/SomeUGen.cpp
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
ls src/synth/plugins/ | grep PluginName

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
  guards these with `NO_LIBSNDFILE` (src/synth/server/SC_Lib_Cintf.cpp:74,
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
- `src/synth/include/common/SC_Types.h`
- `src/synth/include/common/SC_fftlib.h`
- `src/synth/common/SC_fftlib.hpp` (new - private header)
- `src/synth/common/SC_fftlib.cpp`
- `src/synth/include/plugin_interface/SC_InterfaceTable.h`
- `src/synth/include/plugin_interface/SC_World.h`
- `src/synth/include/plugin_interface/SC_Unit.h`
- `src/synth/include/plugin_interface/SC_FifoMsg.h`
- `src/synth/server/SC_World.cpp`
- `src/synth/server/SC_Prototypes.h`
- `src/synth/server/SC_SequencedCommand.h`
- `src/synth/server/SC_SequencedCommand.cpp`
- `src/synth/plugins/DelayUGens.cpp`

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
3. **Document WASM changes** - Note every `scprintf→ss_log` replacement
4. **Test critical fixes** - EnvGen, triggers, delays are high-risk areas
5. **Keep upstream links** - Future maintainers need to trace back to original PRs
6. **Update this guide** - Add new patterns, pitfalls, or workflow improvements
7. **Use cherry-pick when possible** - Preserves upstream history and attribution

---

## Intentionally Excluded Features

Some upstream features are excluded due to AudioWorklet constraints:

- ❌ **Link UGens** (`server/plugins/LinkUGens.cpp`) - requires network sockets for tempo sync ([PR #6947](https://github.com/supercollider/supercollider/pull/6947))
- ❌ **Threading UGens** - no thread spawning in AudioWorklet
- ❌ **Disk I/O UGens** (DiskIn, DiskOut, VDiskIn) - no filesystem access
- ❌ **OSC Network UGens** (SendReply via UDP) - no network sockets

## Preprocessor Conventions for Upstream Files

SuperSonic uses two preprocessor guards in upstream scsynth files. Each serves a distinct purpose for upstream sync.

### `#ifdef SUPERSONIC` — Fork Divergence

Marks where SuperSonic intentionally diverges from upstream scsynth. The original upstream code is preserved in the `#else` branch for merge reference.

```bash
# Find all fork divergence points
grep -rn "ifdef SUPERSONIC\|ifndef SUPERSONIC" src/synth/
```

**During upstream syncs:** Update the `#else` branch to match upstream. Then check whether the `SUPERSONIC` branch needs corresponding changes.

Current `SUPERSONIC` sites in upstream files:

- **SC_Constants.h**: `constexpr` constants (upstream uses runtime `const` with `std::acos` etc.)
- **SC_World.cpp**: `ss_log` declaration, `fPrint` assignment, `InitializeSynthTables`/`InitializeFFTTables` declarations and calls
- **SC_fftlib.cpp**: `ss_log` declaration, idempotency guard in `scfft_global_initialization`, `InitializeFFTTables` entry point
- **Samp.cpp**: Idempotency guard in `FillTables`, `InitializeSynthTables` entry point
- **SC_InterfaceTable.h**: `ss_log` declaration, `DefineSimpleUnit` macro (without trailing semicolon)
- **SC_SndBuf.h**: `ss_log` declaration
- **SC_Graph.cpp**: `ss_log` declaration
- **SC_Lib.cpp**: `ss_log` declaration, direct `SendFailure` error reporting (upstream uses staged `CallSendFailureCommand`)
- **SC_ReplyImpl.hpp**: `kWeb` protocol enum value
- **SC_OSC_Commands.h**: `cmd_b_allocPtr` command number
- **SC_World.cpp**: `supersonic_heap_alloc`/`supersonic_heap_free` for `sc_malloc`/`sc_free` (uses `SUPERSONIC` not `__EMSCRIPTEN__`)
- **LFUGens.cpp**: signed squared/cubed envelope warps (sonic-pi#169) + zero-safe exponential warp endpoints (sonic-pi#881) — `sc_signed_sqrt`/`sc_signed_square`/`sc_exp_safe` helpers, EnvGen segment init/next/fill, duplicated `GET_ENV_VAL` macro
- **DemandUGens.cpp**: signed squared/cubed + zero-safe exponential envelope warps — helpers + demand-rate envelope init/next (sonic-pi#169, sonic-pi#881)

### `#ifndef __EMSCRIPTEN__` — Platform Capability

Guards upstream code that requires APIs unavailable in WASM (filesystem, boost headers, shared memory IPC, threading primitives). These are NOT SuperSonic-specific changes — they're platform exclusions.

```bash
# Find all platform guards
grep -rn "ifdef __EMSCRIPTEN__\|ifndef __EMSCRIPTEN__" src/synth/
```

**During upstream syncs:** Update the guarded code to match upstream exactly (uses `scprintf`, not `ss_log`). Don't skip these blocks — they contain the upstream code that native builds use.

Current `__EMSCRIPTEN__` sites in upstream files:

- **SC_GraphDef.cpp/.h**: `load_file()`, `GraphDef_Load()`, `GraphDef_LoadDir()`, `GraphDef_LoadGlob()`
- **SC_SequencedCommand.h/.cpp**: `LoadSynthDefCmd` class, `LoadSynthDefDirCmd` class
- **SC_MiscCmds.cpp**: `meth_d_load()`, `meth_d_loadDir()`, `meth_b_allocRead` (native sample loader hook), and their `NEW_COMMAND` registrations
- **SC_World.cpp**: `server_shm.hpp` include, `mQuitProgram` semaphore, shared memory init/cleanup, `World_LoadGraphDefs()` body
- **SC_HiddenWorld.h**: `boost/sync/semaphore.hpp` and `server_shm.hpp` includes, `mQuitProgram` and `mShmem` struct fields
- **SC_ReplyImpl.hpp**: `boost::asio::ip::address` vs `uint32_t[4]` placeholder in `ReplyAddress`
- **SC_Reply.cpp**: `operator==` and `operator<` using `memcmp` vs boost address comparison
- **audio_processor.h**: `destroy_world()`/`rebuild_world()` (native-only device hot-swap)
- **audio_processor.cpp**: `__errno_location` override, `EMSCRIPTEN_KEEPALIVE` exports, `mSharedMemoryID`, `destroy_world`/`rebuild_world` implementations

---

## Questions?

If uncertain about a commit:

1. **Check the upstream PR** - Often has discussion about scope/impact
2. **Ask on SuperCollider forums** - Community can clarify intent
3. **When in doubt about a _fix_, apply it** - Easier to revert than to miss a critical fix. **But when in doubt about a _license_, leave it out** - incorporating AGPL code (a whole file or a retyped fragment) produces a derivative work whose licence cannot be undone once distributed. See the [License boundary](#license-boundary-agpl-code-stays-out-of-supersonic).
4. **Test in browser** - Some issues only manifest in WASM environment

---

**Last Updated**: 2026-06-08
**Maintainer**: See git log for recent contributors
**Upstream**: https://github.com/supercollider/supercollider
