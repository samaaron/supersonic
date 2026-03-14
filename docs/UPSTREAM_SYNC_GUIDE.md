# Supersonic ↔ SuperCollider Upstream Sync Guide

**Last Updated**: 2026-03-12
**Last Sync Commit**: 613f26f9
**Upstream Branch**: supercollider/develop (tracked to 2026-03-12)
**Verified Against**: SuperCollider 3.15.0-dev + PR #7405 (Plugin/Unit command extensions)

---

## Overview

Supersonic is a WASM port of SuperCollider's scsynth audio server. It originated from [SuperCollider PR #6569](https://github.com/supercollider/supercollider/pull/6569) but has diverged. This guide explains how to identify and backport relevant upstream changes from SuperCollider to supersonic.

---

## Quick Start Checklist

When syncing with upstream:

- [ ] Fetch latest upstream changes
- [ ] Identify scsynth-relevant commits since last sync
- [ ] Filter out non-applicable changes (sclang, supernova, threads, tests)
- [ ] Check if changes are already applied
- [ ] Cherry-pick or manually apply changes
- [ ] Adapt for WASM (worklet_debug, no threads)
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
grep -n "counter_fractional" src/scsynth/plugins/LFUGens.cpp
```

### Method 3: Check Function Signatures

```bash
# Example: checking if World_TotalFree exists (for rtMemoryStatus)
grep -r "World_TotalFree" src/scsynth/
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
   cat src/scsynth/plugins/LFUGens.cpp
   ```

3. **Apply changes manually** using the Edit tool

4. **Adapt for WASM** (see next section)

---

## Adapting Upstream Changes

SuperSonic has three build targets (WASM, native, NIF) with a shared engine. Use the ifdef conventions above to mark adaptations clearly.

### 1. Print Functions — use `#ifdef SUPERSONIC`

```cpp
#ifdef SUPERSONIC
    worklet_debug("debug message\n");
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
ls src/scsynth/plugins/ | grep "PluginName"

# If it doesn't exist, skip this part
git rm -f server/plugins/PluginName.cpp

# Or abort the cherry-pick if entire commit is not applicable
git cherry-pick --abort
```

### 4. Staging Already-Modified Files

```bash
# If you have unstaged changes that need to be included
git add src/scsynth/server/SC_GraphDef.cpp

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
Group_DumpNodeTreeAndControls functions. Adapted to use worklet_debug
instead of scprintf for WASM AudioWorklet compatibility.
```

---

## Verification Checklist

After applying changes:

- [ ] Code compiles on all targets (`scripts/build-web.sh`, `scripts/build-native.sh`)
- [ ] SuperSonic-specific changes wrapped in `#ifdef SUPERSONIC` with upstream code in `#else`
- [ ] Platform guards use `#ifndef __EMSCRIPTEN__` (not `#ifdef SUPERSONIC`)
- [ ] No bare `scprintf` in shared code — use `worklet_debug` inside `#ifdef SUPERSONIC`
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

# 9. Edit files if needed to replace scprintf with worklet_debug
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
grep -n "the_fix" src/scsynth/plugins/SomeUGen.cpp
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
ls src/scsynth/plugins/ | grep PluginName

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
- `worklet_debug` used for error messages in `PerformAsyncUnitCommand` (instead of upstream `scprintf`)
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
- Kept `worklet_debug` instead of upstream `scprintf`
- Kept `g_lastGraphDefError` static for Emscripten exception handling
- Kept `std::string* outErrorMsg` parameter on `GraphDef_Recv` for error reporting to JS

**Filesystem code guarded:**
- Wrapped filesystem-based synthdef loading in `#ifndef __EMSCRIPTEN__`: `load_file`, `GraphDef_Load`, `GraphDef_LoadDir`, `GraphDef_LoadGlob` (SC_GraphDef.cpp/.h), `LoadSynthDefCmd`, `LoadSynthDefDirCmd` (SC_SequencedCommand.h/.cpp), `meth_d_load`, `meth_d_loadDir` (SC_MiscCmds.cpp), `World_LoadGraphDefs` body (SC_World.cpp)
- Code inside guards matches upstream exactly (uses `scprintf`, not `worklet_debug`) for easy future syncing

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
- `src/scsynth/include/common/SC_Types.h`
- `src/scsynth/include/common/SC_fftlib.h`
- `src/scsynth/common/SC_fftlib.hpp` (new - private header)
- `src/scsynth/common/SC_fftlib.cpp`
- `src/scsynth/include/plugin_interface/SC_InterfaceTable.h`
- `src/scsynth/include/plugin_interface/SC_World.h`
- `src/scsynth/include/plugin_interface/SC_Unit.h`
- `src/scsynth/include/plugin_interface/SC_FifoMsg.h`
- `src/scsynth/server/SC_World.cpp`
- `src/scsynth/server/SC_Prototypes.h`
- `src/scsynth/server/SC_SequencedCommand.h`
- `src/scsynth/server/SC_SequencedCommand.cpp`
- `src/scsynth/plugins/DelayUGens.cpp`

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
  3. 66b74c05f9 - /g_dumpTree end delimiter (already applied with worklet_debug adaptation)

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
3. **Document WASM changes** - Note every `scprintf→worklet_debug` replacement
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
grep -rn "ifdef SUPERSONIC\|ifndef SUPERSONIC" src/scsynth/
```

**During upstream syncs:** Update the `#else` branch to match upstream. Then check whether the `SUPERSONIC` branch needs corresponding changes.

Current `SUPERSONIC` sites in upstream files:

- **SC_Constants.h**: `constexpr` constants (upstream uses runtime `const` with `std::acos` etc.)
- **SC_World.cpp**: `worklet_debug` declaration, `fPrint` assignment, `InitializeSynthTables`/`InitializeFFTTables` declarations and calls
- **SC_fftlib.cpp**: `worklet_debug` declaration, idempotency guard in `scfft_global_initialization`, `InitializeFFTTables` entry point
- **Samp.cpp**: Idempotency guard in `FillTables`, `InitializeSynthTables` entry point
- **SC_InterfaceTable.h**: `worklet_debug` declaration, `DefineSimpleUnit` macro (without trailing semicolon)
- **SC_SndBuf.h**: `worklet_debug` declaration
- **SC_Graph.cpp**: `worklet_debug` declaration
- **SC_Lib.cpp**: `worklet_debug` declaration, direct `SendFailure` error reporting (upstream uses staged `CallSendFailureCommand`)
- **SC_ReplyImpl.hpp**: `kWeb` protocol enum value
- **SC_OSC_Commands.h**: `cmd_b_allocPtr` command number
- **SC_World.cpp**: `supersonic_heap_alloc`/`supersonic_heap_free` for `sc_malloc`/`sc_free` (uses `SUPERSONIC` not `__EMSCRIPTEN__`)

### `#ifndef __EMSCRIPTEN__` — Platform Capability

Guards upstream code that requires APIs unavailable in WASM (filesystem, boost headers, shared memory IPC, threading primitives). These are NOT SuperSonic-specific changes — they're platform exclusions.

```bash
# Find all platform guards
grep -rn "ifdef __EMSCRIPTEN__\|ifndef __EMSCRIPTEN__" src/scsynth/
```

**During upstream syncs:** Update the guarded code to match upstream exactly (uses `scprintf`, not `worklet_debug`). Don't skip these blocks — they contain the upstream code that native builds use.

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
3. **When in doubt, apply it** - Easier to revert than to miss a critical fix
4. **Test in browser** - Some issues only manifest in WASM environment

---

**Last Updated**: 2026-03-09
**Maintainer**: See git log for recent contributors
**Upstream**: https://github.com/supercollider/supercollider
