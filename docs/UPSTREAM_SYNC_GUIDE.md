# Supersonic ↔ SuperCollider Upstream Sync Guide

**Last Updated**: 2025-10-20
**Last Sync Commit**: 0edfbdb8f
**Upstream Branch**: supercollider/develop (tracked to 2025-09-25)

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

## WASM Adaptations

Supersonic runs in a WASM AudioWorklet - some changes are needed:

### 1. Replace Print Functions

```cpp
// ❌ Upstream uses
scprintf("debug message\n");

// ✅ Supersonic uses
worklet_debug("debug message\n");
```

### 2. Remove Thread-Related Code

```cpp
// ❌ Skip any code with these:
pthread_*
std::thread
std::mutex
std::lock_guard
std::atomic
```

### 3. Check for Platform-Specific Code

```cpp
// ❌ Skip platform-specific networking
#ifdef _WIN32
#ifdef __APPLE__

// ✅ Keep cross-platform audio/DSP code
```

### 4. Verify Memory Allocations

```cpp
// ✅ These are fine in supersonic
RTAlloc(world, size);
RTFree(world, ptr);
World_Alloc(world, size);

// ❌ Avoid direct malloc/free in RT context
```

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

- [ ] Code compiles (if you have build system set up)
- [ ] No `scprintf` - replaced with `worklet_debug`
- [ ] No thread/mutex code introduced
- [ ] No print statements for debugging
- [ ] Git commit includes upstream hash and link
- [ ] Commit message explains WASM adaptations (if any)
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
```

### Low Priority

```
server/scsynth/SC_Str4.cpp        # String utilities
server/scsynth/SC_Rate.cpp        # Rate structures
```

---

## Reference: Previous Sync Summary

Last major sync (2025-10-20) applied:
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

## Questions?

If uncertain about a commit:

1. **Check the upstream PR** - Often has discussion about scope/impact
2. **Ask on SuperCollider forums** - Community can clarify intent
3. **When in doubt, apply it** - Easier to revert than to miss a critical fix
4. **Test in browser** - Some issues only manifest in WASM environment

---

**Last Updated**: 2025-10-20
**Maintainer**: See git log for recent contributors
**Upstream**: https://github.com/supercollider/supercollider
