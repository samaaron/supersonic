# Scsynth Command Reference

You control SuperSonic by sending **OSC** (Open Sound Control) messages which are then forwarded onto the scsynth AudioWorklet which will then act on them. This message-based API gives you full control of the synthesis engine from sending new synth designs, triggering synths, controlling running synths, loading buffers, etc.

OSC messages can also sent as a bundle along with a fine-grained timestamp for accurate scheduling.

> This reference covers all the OSC messages that SuperSonic understands. It is based on the [SuperCollider Server Command Reference](https://doc.sccode.org/Reference/Server-Command-Reference.html). However, it is not identical due to implementation differences between the original scsynth and SuperSonic's AudioWorklet. See [Unsupported Commands](#unsupported-commands) for what's not available, and [SCSYNTH_DIFFERENCES.md](SCSYNTH_DIFFERENCES.md) for a comprehensive guide to all differences including unsupported UGens.


## How to Send OSC

Send commands using `send()` which auto-detects types:

```javascript
supersonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "note", 60);
```

Or directly send OSC bytes that you have already pre-encoded via `sendOSC()`:

```javascript
supersonic.sendOSC(oscBytes);
```

## Useful Terms

If you're new to audio synthesis and SuperCollider in particular, here are some of the terms you'll come across frequently in this reference:

| Concept         | What it is                                                                                          |
| --------------- | --------------------------------------------------------------------------------------------------- |
| **OSC**         | Open Sound Control - the binary protocol used for comms. See: https://opensoundcontrol.stanford.edu |
| **Synth**       | A sound-producing unit. Created from a synthesiser design (synthdef) and assigned a node ID.        |
| **SynthDef**    | A "recipe" or template that defines how a synth generates sound. Load these before creating synths. |
| **Node**        | Anything in the audio processing tree — either a synth or a group. Each has a unique ID.            |
| **Group**       | A container that holds nodes (synths or groups). Useful for controlling multiple synths at once.    |
| **Node Tree**   | The tree of all nodes currently running. This includes all synths and groups.                       |
| **Buffer**      | A chunk of memory holding audio samples — for playback, recording, or wavetables.                   |
| **Control Bus** | A single value that synths can read/write — used for communicating between synths.                  |
| **Audio Bus**   | Similar to a control bus, but carries audio-rate signals between synths.                            |


## OSC API

| Command                                      | Description                                        |
| -------------------------------------------- | -------------------------------------------------- |
| **Top-Level**                                |                                                    |
| [`/notify`](#notify)                         | Register for node event notifications              |
| [`/status`](#status)                         | Query server status (UGens, synths, CPU)           |
| [`/sync`](#sync)                             | Wait for async commands to complete                |
| [`/version`](#version)                       | Query server version info                          |
| [`/rtMemoryStatus`](#rtmemorystatus)         | Query realtime memory usage                        |
| **Synth Definitions**                        |                                                    |
| [`/d_recv`](#d_recv)                         | Load a synthdef from binary data                   |
| [`/d_free`](#d_free)                         | Free loaded synthdefs by name                      |
| **Nodes**                                    |                                                    |
| [`/n_free`](#n_free)                         | Delete nodes                                       |
| [`/n_run`](#n_run)                           | Turn nodes on or off                               |
| [`/n_set`](#n_set)                           | Set node control values                            |
| [`/n_setn`](#n_setn)                         | Set sequential control values                      |
| [`/n_fill`](#n_fill)                         | Fill controls with a single value                  |
| [`/n_map`](#n_map)                           | Map controls to control buses                      |
| [`/n_mapn`](#n_mapn)                         | Map sequential controls to control buses           |
| [`/n_mapa`](#n_mapa)                         | Map controls to audio buses                        |
| [`/n_mapan`](#n_mapan)                       | Map sequential controls to audio buses             |
| [`/n_before`](#n_before)                     | Move node before another                           |
| [`/n_after`](#n_after)                       | Move node after another                            |
| [`/n_query`](#n_query)                       | Query node info                                    |
| [`/n_trace`](#n_trace)                       | Debug trace node execution                         |
| [`/n_order`](#n_order)                       | Reorder nodes within a group                       |
| **Synths**                                   |                                                    |
| [`/s_new`](#s_new)                           | Create a new synth                                 |
| [`/s_get`](#s_get)                           | Get synth control values                           |
| [`/s_getn`](#s_getn)                         | Get sequential synth control values                |
| [`/s_noid`](#s_noid)                         | Remove synth ID tracking                           |
| **Groups**                                   |                                                    |
| [`/g_new`](#g_new)                           | Create a new group                                 |
| [`/p_new`](#p_new)                           | Create a parallel group                            |
| [`/g_head`](#g_head)                         | Move node to head of group                         |
| [`/g_tail`](#g_tail)                         | Move node to tail of group                         |
| [`/g_freeAll`](#g_freeall)                   | Free all nodes in group                            |
| [`/g_deepFree`](#g_deepfree)                 | Recursively free all synths in group               |
| [`/g_dumpTree`](#g_dumptree)                 | Print group tree (debug)                           |
| [`/g_queryTree`](#g_querytree)               | Query group tree structure                         |
| **Buffers**                                  |                                                    |
| [`/b_alloc`](#b_alloc)                       | Allocate an empty buffer                           |
| [`/b_free`](#b_free)                         | Free a buffer                                      |
| [`/b_zero`](#b_zero)                         | Zero buffer contents                               |
| [`/b_set`](#b_set)                           | Set individual samples                             |
| [`/b_setn`](#b_setn)                         | Set sequential samples                             |
| [`/b_fill`](#b_fill)                         | Fill samples with a value                          |
| [`/b_gen`](#b_gen)                           | Generate buffer contents (sine, cheby, etc.)       |
| [`/b_query`](#b_query)                       | Query buffer info                                  |
| [`/b_get`](#b_get)                           | Get sample values                                  |
| [`/b_getn`](#b_getn)                         | Get sequential sample values                       |
| **Control Buses**                            |                                                    |
| [`/c_set`](#c_set)                           | Set control bus values                             |
| [`/c_setn`](#c_setn)                         | Set sequential bus values                          |
| [`/c_fill`](#c_fill)                         | Fill buses with a value                            |
| [`/c_get`](#c_get)                           | Get bus values                                     |
| [`/c_getn`](#c_getn)                         | Get sequential bus values                          |
| **SuperSonic Extensions**                    |                                                    |
| [`/b_allocFile`](#b_allocfile)               | Load audio from inline file data (SuperSonic only) |

---

## Conventions

### Parameter Types

| Notation | Type    | Description              |
| -------- | ------- | ------------------------ |
| `int`    | Integer | 32-bit signed integer    |
| `float`  | Float   | 32-bit floating point    |
| `double` | Double  | 64-bit floating point    |
| `string` | String  | Null-terminated string   |
| `bytes`  | Blob    | Binary data (byte array) |

### Repetition

`N ×` indicates the parameter can be repeated N times. For example:

```javascript
// /n_free takes N node IDs
supersonic.send("/n_free", 1000, 1001, 1002);
```

### Node IDs

- Use `-1` to have the server auto-generate a unique node ID
- Node ID `0` is the root group (always exists)
- Positive integers are user-assigned IDs

### Add Actions

Used when creating or moving nodes:

| Value | Action  | Description                 |
| ----- | ------- | --------------------------- |
| 0     | head    | Add to head of target group |
| 1     | tail    | Add to tail of target group |
| 2     | before  | Add before target node      |
| 3     | after   | Add after target node       |
| 4     | replace | Replace target node         |

### Control Values

Controls can be set by index (integer) or name (string). Values can be:

- `float` or `int` - Direct value
- `"cN"` - Map to control bus N (e.g., `"c0"`)
- `"aN"` - Map to audio bus N (e.g., `"a0"`)

### Asynchronous Commands

Commands marked **Async** execute on a background thread. They reply with `/done` on success or `/fail` on error. Use `/sync` to wait for all async commands to complete.

---

## Top-Level Commands

### `/notify`

Register/unregister for server notifications.

| Parameter | Type | Description                  |
| --------- | ---- | ---------------------------- |
| flag      | int  | 1 = register, 0 = unregister |
| clientID  | int  | Client ID (optional)         |

**Async:** Yes
**Reply:** `/done /notify clientID [maxLogins]`

When registered, the server sends notifications for node events (`/n_go`, `/n_end`, etc.).

```javascript
supersonic.send("/notify", 1); // Register for notifications
supersonic.send("/notify", 0); // Unregister
```

---

### `/status`

Query server status.

| Parameter | Type | Description |
| --------- | ---- | ----------- |

**Reply:** `/status.reply` with:

```javascript
supersonic.send("/status");
```

| Position | Type   | Description                        |
| -------- | ------ | ---------------------------------- |
| 0        | int    | (unused)                           |
| 1        | int    | Number of unit generators          |
| 2        | int    | Number of synths                   |
| 3        | int    | Number of groups                   |
| 4        | int    | Number of loaded synth definitions |
| 5        | float  | Average CPU usage (%)              |
| 6        | float  | Peak CPU usage (%)                 |
| 7        | double | Nominal sample rate                |
| 8        | double | Actual sample rate                 |

---

### `/dumpOSC`

Enable/disable OSC message dumping to the debug output.

| Parameter | Type | Description                                      |
| --------- | ---- | ------------------------------------------------ |
| mode      | int  | 0 = off, 1 = parsed, 2 = hex, 3 = parsed and hex |

When enabled, all incoming OSC messages are printed to the debug output (visible in the Debug Info panel).

```javascript
supersonic.send("/dumpOSC", 1); // Enable parsed output
supersonic.send("/dumpOSC", 2); // Enable hex dump
supersonic.send("/dumpOSC", 3); // Enable both
supersonic.send("/dumpOSC", 0); // Disable
```

Example output:
```
dumpOSC: [ "/s_new", "default", 1000, 0, 1, "freq", 440 ]
```

---

### `/sync`

Wait for all asynchronous commands to complete.

| Parameter | Type | Description       |
| --------- | ---- | ----------------- |
| id        | int  | Unique identifier |

**Async:** Yes
**Reply:** `/synced id`

Use this to ensure async operations (like loading synthdefs) have finished:

```javascript
supersonic.send("/d_recv", synthdefBytes);
supersonic.send("/sync", 1);
// Wait for /synced 1 before using the synthdef
```

---

### `/version`

Query server version.

| Parameter | Type | Description |
| --------- | ---- | ----------- |

```javascript
supersonic.send("/version");
```

**Reply:** `/version.reply` with:

| Position | Type   | Description   |
| -------- | ------ | ------------- |
| 0        | string | Program name  |
| 1        | int    | Major version |
| 2        | int    | Minor version |
| 3        | string | Patch version |
| 4        | string | Git branch    |
| 5        | string | Commit hash   |

---

### `/rtMemoryStatus`

Query realtime memory status.

| Parameter | Type | Description |
| --------- | ---- | ----------- |

```javascript
supersonic.send("/rtMemoryStatus");
```

**Reply:** `/rtMemoryStatus.reply` with:

| Position | Type | Description                |
| -------- | ---- | -------------------------- |
| 0        | int  | Free memory (bytes)        |
| 1        | int  | Largest free block (bytes) |

---

## Synth Definition Commands

### `/d_recv`

Receive a synth definition from bytes.

| Parameter  | Type  | Description                                     |
| ---------- | ----- | ----------------------------------------------- |
| data       | bytes | Compiled synthdef binary                        |
| completion | bytes | OSC message to execute on completion (optional) |

**Async:** Yes
**Reply:** `/done /d_recv`

```javascript
// Send raw synthdef bytes
supersonic.send("/d_recv", synthdefBytes);

// In SuperSonic, prefer loadSynthDef() which handles fetching:
await supersonic.loadSynthDef("sonic-pi-beep");
```

---

### `/d_free`

Free synth definitions.

| Parameter | Type       | Description            |
| --------- | ---------- | ---------------------- |
| names     | N × string | Synthdef names to free |

```javascript
supersonic.send("/d_free", "sonic-pi-beep", "sonic-pi-prophet");
```

---

## Node Commands

Nodes are the basic units of the server's execution tree. There are two types:

- **Synths** - Sound-producing nodes
- **Groups** - Collections of nodes

### `/n_free`

Free (delete) nodes.

| Parameter | Type    | Description      |
| --------- | ------- | ---------------- |
| nodeIDs   | N × int | Node IDs to free |

```javascript
supersonic.send("/n_free", 1000, 1001, 1002);
```

---

### `/n_run`

Turn nodes on or off.

| Parameter | Type | Description                 |
| --------- | ---- | --------------------------- |
| nodeID    | int  | Node ID                     |
| flag      | int  | 0 = off, 1 = on             |
| ...       |      | (repeat for multiple nodes) |

```javascript
supersonic.send("/n_run", 1000, 0); // Turn off node 1000
supersonic.send("/n_run", 1000, 1); // Turn on node 1000
```

---

### `/n_set`

Set node control values.

| Parameter | Type          | Description                  |
| --------- | ------------- | ---------------------------- |
| nodeID    | int           | Node ID                      |
| control   | int or string | Control index or name        |
| value     | float or int  | Control value                |
| ...       |               | (repeat control/value pairs) |

If the node is a group, sets the control on all nodes in the group.

```javascript
supersonic.send("/n_set", 1000, "freq", 440, "amp", 0.5);
supersonic.send("/n_set", 1000, 0, 440, 1, 0.5); // By index
```

---

### `/n_setn`

Set sequential control values.

| Parameter | Type          | Description                    |
| --------- | ------------- | ------------------------------ |
| nodeID    | int           | Node ID                        |
| control   | int or string | Starting control index or name |
| count     | int           | Number of sequential controls  |
| values    | N × float     | Control values                 |
| ...       |               | (repeat for more ranges)       |

```javascript
// Set controls 0, 1, 2 to values 100, 200, 300
supersonic.send("/n_setn", 1000, 0, 3, 100, 200, 300);
```

---

### `/n_fill`

Fill sequential controls with a single value.

| Parameter | Type          | Description                    |
| --------- | ------------- | ------------------------------ |
| nodeID    | int           | Node ID                        |
| control   | int or string | Starting control index or name |
| count     | int           | Number of controls to fill     |
| value     | float         | Fill value                     |
| ...       |               | (repeat for more ranges)       |

```javascript
// Fill controls 0-9 with value 0.5
supersonic.send("/n_fill", 1000, 0, 10, 0.5);
```

---

### `/n_map`

Map controls to control buses.

| Parameter | Type          | Description                     |
| --------- | ------------- | ------------------------------- |
| nodeID    | int           | Node ID                         |
| control   | int or string | Control index or name           |
| busIndex  | int           | Control bus index (-1 to unmap) |
| ...       |               | (repeat for more mappings)      |

Controls mapped to buses read their values continuously from the bus.

```javascript
supersonic.send("/n_map", 1000, "freq", 0); // Map freq to bus 0
supersonic.send("/n_map", 1000, "freq", -1); // Unmap freq
```

---

### `/n_mapn`

Map sequential controls to control buses.

| Parameter | Type          | Description                    |
| --------- | ------------- | ------------------------------ |
| nodeID    | int           | Node ID                        |
| control   | int or string | Starting control index or name |
| busIndex  | int           | Starting control bus index     |
| count     | int           | Number of controls to map      |
| ...       |               | (repeat for more ranges)       |

```javascript
// Map controls 0-3 to buses 10-13
supersonic.send("/n_mapn", 1000, 0, 10, 4);
```

---

### `/n_mapa`

Map controls to audio buses.

| Parameter | Type          | Description                   |
| --------- | ------------- | ----------------------------- |
| nodeID    | int           | Node ID                       |
| control   | int or string | Control index or name         |
| busIndex  | int           | Audio bus index (-1 to unmap) |
| ...       |               | (repeat for more mappings)    |

```javascript
supersonic.send("/n_mapa", 1000, "freq", 0); // Map freq to audio bus 0
```

---

### `/n_mapan`

Map sequential controls to audio buses.

| Parameter | Type          | Description                    |
| --------- | ------------- | ------------------------------ |
| nodeID    | int           | Node ID                        |
| control   | int or string | Starting control index or name |
| busIndex  | int           | Starting audio bus index       |
| count     | int           | Number of controls to map      |
| ...       |               | (repeat for more ranges)       |

```javascript
// Map controls 0-3 to audio buses 0-3
supersonic.send("/n_mapan", 1000, 0, 0, 4);
```

---

### `/n_before`

Move nodes to execute before other nodes.

| Parameter | Type | Description             |
| --------- | ---- | ----------------------- |
| nodeA     | int  | Node to move            |
| nodeB     | int  | Target node             |
| ...       |      | (repeat for more pairs) |

Places nodeA immediately before nodeB in the same group.

```javascript
supersonic.send("/n_before", 1001, 1000); // Move 1001 before 1000
```

---

### `/n_after`

Move nodes to execute after other nodes.

| Parameter | Type | Description             |
| --------- | ---- | ----------------------- |
| nodeA     | int  | Node to move            |
| nodeB     | int  | Target node             |
| ...       |      | (repeat for more pairs) |

Places nodeA immediately after nodeB in the same group.

```javascript
supersonic.send("/n_after", 1001, 1000); // Move 1001 after 1000
```

---

### `/n_query`

Query node information.

| Parameter | Type    | Description       |
| --------- | ------- | ----------------- |
| nodeIDs   | N × int | Node IDs to query |

**Reply:** `/n_info` for each node (see [Node Notifications](#node-notifications))

```javascript
supersonic.send("/n_query", 1000, 1001, 1002);
```

---

### `/n_trace`

Trace node execution (debug).

| Parameter | Type    | Description       |
| --------- | ------- | ----------------- |
| nodeIDs   | N × int | Node IDs to trace |

Prints control values and calculation rates for each node.

```javascript
supersonic.send("/n_trace", 1000);
```

---

### `/n_order`

Reorder nodes within a group.

| Parameter | Type    | Description                               |
| --------- | ------- | ----------------------------------------- |
| addAction | int     | 0 = head, 1 = tail, 2 = before, 3 = after |
| target    | int     | Target node ID                            |
| nodeIDs   | N × int | Node IDs to reorder                       |

```javascript
// Move nodes 1001, 1002, 1003 to head of group 0
supersonic.send("/n_order", 0, 0, 1001, 1002, 1003);
```

---

## Synth Commands

### `/s_new`

Create a new synth.

| Parameter | Type                  | Description                             |
| --------- | --------------------- | --------------------------------------- |
| name      | string                | Synthdef name                           |
| nodeID    | int                   | Synth ID (-1 for auto-assign)           |
| addAction | int                   | Where to add (see Add Actions)          |
| target    | int                   | Target node ID                          |
| control   | int or string         | Control index or name (optional)        |
| value     | float, int, or string | Control value or bus mapping (optional) |
| ...       |                       | (repeat control/value pairs)            |

```javascript
// Create synth at head of group 0
supersonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "note", 60, "amp", 0.5);

// Auto-assign node ID
supersonic.send("/s_new", "sonic-pi-beep", -1, 0, 0);

// Map control to bus
supersonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "freq", "c0"); // Control bus 0
supersonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "freq", "a0"); // Audio bus 0
```

---

### `/s_get`

Get synth control values.

| Parameter | Type                | Description              |
| --------- | ------------------- | ------------------------ |
| nodeID    | int                 | Synth ID                 |
| controls  | N × (int or string) | Control indices or names |

**Reply:** `/n_set nodeID control value ...`

```javascript
supersonic.send("/s_get", 1000, "freq", "amp");
```

---

### `/s_getn`

Get sequential synth control values.

| Parameter | Type          | Description                    |
| --------- | ------------- | ------------------------------ |
| nodeID    | int           | Synth ID                       |
| control   | int or string | Starting control index or name |
| count     | int           | Number of sequential controls  |
| ...       |               | (repeat for more ranges)       |

**Reply:** `/n_setn nodeID control count values...`

```javascript
// Get controls 0-4 from synth 1000
supersonic.send("/s_getn", 1000, 0, 5);
```

---

### `/s_noid`

Remove synth IDs (internal bookkeeping).

| Parameter | Type    | Description |
| --------- | ------- | ----------- |
| nodeIDs   | N × int | Synth IDs   |

Reassigns the synths to reserved negative IDs. Used for freeing client-side tracking while keeping synths running.

```javascript
supersonic.send("/s_noid", 1000, 1001);
```

---

## Group Commands

### `/g_new`

Create new groups.

| Parameter | Type | Description                    |
| --------- | ---- | ------------------------------ |
| nodeID    | int  | New group ID                   |
| addAction | int  | Where to add (see Add Actions) |
| target    | int  | Target node ID                 |
| ...       |      | (repeat for more groups)       |

```javascript
// Create group 100 at head of root group
supersonic.send("/g_new", 100, 0, 0);

// Create multiple groups
supersonic.send("/g_new", 100, 0, 0, 101, 1, 0, 102, 3, 100);
```

---

### `/p_new`

Create new parallel groups.

| Parameter | Type | Description                    |
| --------- | ---- | ------------------------------ |
| nodeID    | int  | New group ID                   |
| addAction | int  | Where to add (see Add Actions) |
| target    | int  | Target node ID                 |
| ...       |      | (repeat for more groups)       |

Parallel groups evaluate their children in unspecified order, allowing for parallel processing optimizations.

```javascript
supersonic.send("/p_new", 100, 0, 0); // Create parallel group 100 at head of root
```

---

### `/g_head`

Move nodes to head of groups.

| Parameter | Type | Description             |
| --------- | ---- | ----------------------- |
| groupID   | int  | Target group            |
| nodeID    | int  | Node to move            |
| ...       |      | (repeat for more pairs) |

```javascript
supersonic.send("/g_head", 0, 1000); // Move node 1000 to head of group 0
```

---

### `/g_tail`

Move nodes to tail of groups.

| Parameter | Type | Description             |
| --------- | ---- | ----------------------- |
| groupID   | int  | Target group            |
| nodeID    | int  | Node to move            |
| ...       |      | (repeat for more pairs) |

```javascript
supersonic.send("/g_tail", 0, 1000); // Move node 1000 to tail of group 0
```

---

### `/g_freeAll`

Free all nodes in groups.

| Parameter | Type    | Description |
| --------- | ------- | ----------- |
| groupIDs  | N × int | Group IDs   |

Frees all immediate children of the groups.

```javascript
supersonic.send("/g_freeAll", 0); // Free all nodes in root group
```

---

### `/g_deepFree`

Deep free all synths in groups.

| Parameter | Type    | Description |
| --------- | ------- | ----------- |
| groupIDs  | N × int | Group IDs   |

Traverses all nested groups and frees all synths found.

```javascript
supersonic.send("/g_deepFree", 0); // Free all synths recursively from root
```

---

### `/g_dumpTree`

Print group tree (debug).

| Parameter | Type | Description                                           |
| --------- | ---- | ----------------------------------------------------- |
| groupID   | int  | Group ID                                              |
| flag      | int  | 0 = structure only, non-zero = include control values |

```javascript
supersonic.send("/g_dumpTree", 0, 0); // Print structure only
supersonic.send("/g_dumpTree", 0, 1); // Print with control values
```

---

### `/g_queryTree`

Query group tree structure.

| Parameter | Type | Description                                           |
| --------- | ---- | ----------------------------------------------------- |
| groupID   | int  | Group ID                                              |
| flag      | int  | 0 = structure only, non-zero = include control values |

**Reply:** `/g_queryTree.reply` with tree structure:

| Position | Type | Description                                                                     |
| -------- | ---- | ------------------------------------------------------------------------------- |
| 0        | int  | Flag (echoed)                                                                   |
| 1        | int  | Root node ID                                                                    |
| 2        | int  | Number of children                                                              |
| ...      |      | For each child: nodeID, numChildren (-1 if synth), [synthName], [controlValues] |

```javascript
supersonic.send("/g_queryTree", 0, 0); // Query root group structure
```

---

## Buffer Commands

Buffers store audio data for playback, recording, and wavetables.

### `/b_alloc`

Allocate a buffer.

| Parameter  | Type  | Description                                  |
| ---------- | ----- | -------------------------------------------- |
| bufnum     | int   | Buffer number                                |
| frames     | int   | Number of frames                             |
| channels   | int   | Number of channels (default: 1)              |
| completion | bytes | Completion message (optional)                |
| sampleRate | float | Sample rate (optional, default: server rate) |

**Async:** Yes
**Reply:** `/done /b_alloc bufnum`

```javascript
// Allocate mono buffer with 44100 frames
supersonic.send("/b_alloc", 0, 44100, 1);

// Allocate stereo buffer
supersonic.send("/b_alloc", 1, 44100, 2);
```

---

### `/b_free`

Free a buffer.

| Parameter  | Type  | Description                   |
| ---------- | ----- | ----------------------------- |
| bufnum     | int   | Buffer number                 |
| completion | bytes | Completion message (optional) |

**Async:** Yes
**Reply:** `/done /b_free bufnum`

```javascript
supersonic.send("/b_free", 0);
```

---

### `/b_zero`

Zero a buffer's contents.

| Parameter  | Type  | Description                   |
| ---------- | ----- | ----------------------------- |
| bufnum     | int   | Buffer number                 |
| completion | bytes | Completion message (optional) |

**Async:** Yes
**Reply:** `/done /b_zero bufnum`

```javascript
supersonic.send("/b_zero", 0);
```

---

### `/b_set`

Set individual samples in a buffer.

| Parameter | Type  | Description                |
| --------- | ----- | -------------------------- |
| bufnum    | int   | Buffer number              |
| index     | int   | Sample index               |
| value     | float | Sample value               |
| ...       |       | (repeat index/value pairs) |

```javascript
supersonic.send("/b_set", 0, 0, 1.0, 100, 0.5, 200, -0.5);
```

---

### `/b_setn`

Set sequential samples in a buffer.

| Parameter  | Type      | Description              |
| ---------- | --------- | ------------------------ |
| bufnum     | int       | Buffer number            |
| startIndex | int       | Starting sample index    |
| count      | int       | Number of samples        |
| values     | N × float | Sample values            |
| ...        |           | (repeat for more ranges) |

```javascript
// Set samples 0-2 to [1.0, 0.5, 0.0]
supersonic.send("/b_setn", 0, 0, 3, 1.0, 0.5, 0.0);
```

---

### `/b_fill`

Fill buffer samples with a value.

| Parameter  | Type  | Description               |
| ---------- | ----- | ------------------------- |
| bufnum     | int   | Buffer number             |
| startIndex | int   | Starting sample index     |
| count      | int   | Number of samples to fill |
| value      | float | Fill value                |
| ...        |       | (repeat for more ranges)  |

```javascript
// Fill samples 0-99 with 0.5
supersonic.send("/b_fill", 0, 0, 100, 0.5);
```

---

### `/b_gen`

Generate buffer contents using a fill command.

| Parameter | Type   | Description                |
| --------- | ------ | -------------------------- |
| bufnum    | int    | Buffer number              |
| command   | string | Generator command name     |
| args      | ...    | Command-specific arguments |

**Async:** Yes
**Reply:** `/done /b_gen bufnum`

See [Buffer Fill Commands](#buffer-fill-commands-for-b_gen) for available generators.

```javascript
// Generate a sine wave with 3 harmonics
supersonic.send("/b_gen", 0, "sine1", 7, 1.0, 0.5, 0.25);
```

---

### `/b_query`

Query buffer information.

| Parameter | Type    | Description    |
| --------- | ------- | -------------- |
| bufnums   | N × int | Buffer numbers |

**Reply:** `/b_info` for each buffer:

| Position | Type  | Description        |
| -------- | ----- | ------------------ |
| 0        | int   | Buffer number      |
| 1        | int   | Number of frames   |
| 2        | int   | Number of channels |
| 3        | float | Sample rate        |

```javascript
supersonic.send("/b_query", 0, 1, 2); // Query buffers 0, 1, and 2
```

---

### `/b_get`

Get individual sample values from a buffer.

| Parameter | Type    | Description    |
| --------- | ------- | -------------- |
| bufnum    | int     | Buffer number  |
| indices   | N × int | Sample indices |

```javascript
supersonic.send("/b_get", 0, 0, 100, 200); // Get samples at indices 0, 100, 200
```

**Reply:** `/b_set bufnum index value ...`

---

### `/b_getn`

Get sequential sample values from a buffer.

| Parameter  | Type | Description              |
| ---------- | ---- | ------------------------ |
| bufnum     | int  | Buffer number            |
| startIndex | int  | Starting sample index    |
| count      | int  | Number of samples        |
| ...        |      | (repeat for more ranges) |

**Reply:** `/b_setn bufnum startIndex count values...`

```javascript
supersonic.send("/b_getn", 0, 0, 100); // Get samples 0-99 from buffer 0
```

---

## Control Bus Commands

Control buses are single-sample values used to pass control signals between synths.

### `/c_set`

Set control bus values.

| Parameter | Type         | Description             |
| --------- | ------------ | ----------------------- |
| index     | int          | Bus index               |
| value     | float or int | Bus value               |
| ...       |              | (repeat for more buses) |

```javascript
supersonic.send("/c_set", 0, 440, 1, 0.5);
```

---

### `/c_setn`

Set sequential control bus values.

| Parameter  | Type               | Description              |
| ---------- | ------------------ | ------------------------ |
| startIndex | int                | Starting bus index       |
| count      | int                | Number of buses          |
| values     | N × (float or int) | Bus values               |
| ...        |                    | (repeat for more ranges) |

```javascript
supersonic.send("/c_setn", 0, 3, 440, 880, 1320);
```

---

### `/c_fill`

Fill control buses with a value.

| Parameter  | Type         | Description              |
| ---------- | ------------ | ------------------------ |
| startIndex | int          | Starting bus index       |
| count      | int          | Number of buses to fill  |
| value      | float or int | Fill value               |
| ...        |              | (repeat for more ranges) |

```javascript
supersonic.send("/c_fill", 0, 10, 0.0); // Fill buses 0-9 with 0.0
```

---

### `/c_get`

Get control bus values.

| Parameter | Type    | Description |
| --------- | ------- | ----------- |
| indices   | N × int | Bus indices |

**Reply:** `/c_set index value ...`

```javascript
supersonic.send("/c_get", 0, 1, 2); // Get bus values 0, 1, 2
```

---

### `/c_getn`

Get sequential control bus values.

| Parameter  | Type | Description              |
| ---------- | ---- | ------------------------ |
| startIndex | int  | Starting bus index       |
| count      | int  | Number of buses          |
| ...        |      | (repeat for more ranges) |

**Reply:** `/c_setn startIndex count values...`

```javascript
supersonic.send("/c_getn", 0, 10); // Get buses 0-9
```

---

## Buffer Fill Commands (for /b_gen)

These commands are used with `/b_gen` to generate buffer contents.

### Flags

All waveform generators accept a flags parameter:

| Flag      | Value | Description                                 |
| --------- | ----- | ------------------------------------------- |
| normalize | 1     | Normalize peak amplitude to 1.0             |
| wavetable | 2     | Generate in wavetable format (for Osc UGen) |
| clear     | 4     | Clear buffer before generating              |

Combine flags by adding: `7 = normalize + wavetable + clear`

### `sine1`

Generate a waveform from harmonic amplitudes.

| Parameter  | Type      | Description                 |
| ---------- | --------- | --------------------------- |
| flags      | int       | See flags above             |
| amplitudes | N × float | Amplitude for each harmonic |

```javascript
// Fundamental + 2 harmonics, normalized
supersonic.send("/b_gen", 0, "sine1", 1, 1.0, 0.5, 0.25);
```

### `sine2`

Generate a waveform from frequencies and amplitudes.

| Parameter | Type  | Description                    |
| --------- | ----- | ------------------------------ |
| flags     | int   | See flags above                |
| freq      | float | Frequency in cycles per buffer |
| amp       | float | Amplitude                      |
| ...       |       | (repeat freq/amp pairs)        |

```javascript
// Two partials at different frequencies
supersonic.send("/b_gen", 0, "sine2", 1, 1.0, 1.0, 2.5, 0.5);
```

### `sine3`

Generate a waveform from frequencies, amplitudes, and phases.

| Parameter | Type  | Description                      |
| --------- | ----- | -------------------------------- |
| flags     | int   | See flags above                  |
| freq      | float | Frequency in cycles per buffer   |
| amp       | float | Amplitude                        |
| phase     | float | Phase in radians                 |
| ...       |       | (repeat freq/amp/phase triplets) |

### `cheby`

Generate a Chebyshev polynomial waveshaping transfer function.

| Parameter  | Type      | Description                         |
| ---------- | --------- | ----------------------------------- |
| flags      | int       | See flags above                     |
| amplitudes | N × float | Amplitude for each polynomial order |

Useful for creating harmonic distortion with predictable spectral content.

### `copy`

Copy samples from another buffer.

| Parameter | Type | Description                               |
| --------- | ---- | ----------------------------------------- |
| destPos   | int  | Destination sample position               |
| srcBuf    | int  | Source buffer number                      |
| srcPos    | int  | Source sample position                    |
| count     | int  | Number of samples (-1 = maximum possible) |

```javascript
// Copy 1000 samples from buffer 1 to buffer 0
supersonic.send("/b_gen", 0, "copy", 0, 1, 0, 1000);
```

---

## Reply Messages

### `/done`

Sent when an asynchronous command completes successfully.

| Position | Type   | Description           |
| -------- | ------ | --------------------- |
| 0        | string | Command name          |
| 1+       | ...    | Command-specific data |

### `/fail`

Sent when a command fails.

| Position | Type   | Description           |
| -------- | ------ | --------------------- |
| 0        | string | Command name          |
| 1        | string | Error message         |
| 2+       | ...    | Command-specific data |

---

## Node Notifications

When registered for notifications (via `/notify`), the server sends messages about node state changes. All node notifications include:

| Position | Type | Description                     |
| -------- | ---- | ------------------------------- |
| 0        | int  | Node ID                         |
| 1        | int  | Parent group ID                 |
| 2        | int  | Previous node ID (-1 if none)   |
| 3        | int  | Next node ID (-1 if none)       |
| 4        | int  | Is group (1 = group, 0 = synth) |

For groups, additionally:

| Position | Type | Description                |
| -------- | ---- | -------------------------- |
| 5        | int  | Head node ID (-1 if empty) |
| 6        | int  | Tail node ID (-1 if empty) |

### `/n_go`

Sent when a node is created.

### `/n_end`

Sent when a node is freed/deallocated.

### `/n_off`

Sent when a node is turned off (via `/n_run`).

### `/n_on`

Sent when a node is turned on (via `/n_run`).

### `/n_move`

Sent when a node is moved to a different position.

### `/n_info`

Reply to `/n_query`.

---

## Trigger Notification

### `/tr`

Sent when a `SendTrig` UGen fires in a synth.

| Position | Type  | Description   |
| -------- | ----- | ------------- |
| 0        | int   | Node ID       |
| 1        | int   | Trigger ID    |
| 2        | float | Trigger value |

This allows synths to send events back to clients.

---

## SuperSonic Extensions

These commands are specific to SuperSonic and not part of the standard scsynth protocol.

### `/b_allocFile`

Load audio from inline file data. The blob contains raw file bytes (FLAC, WAV, OGG, MP3, etc.) which are decoded using the browser's `decodeAudioData()`.

| Parameter | Type | Description          |
| --------- | ---- | -------------------- |
| bufnum    | int  | Buffer number        |
| data      | blob | Raw audio file bytes |

```javascript
// Fetch file and send as inline blob
const response = await fetch("sample.flac");
const fileBytes = new Uint8Array(await response.arrayBuffer());
supersonic.send("/b_allocFile", 0, fileBytes);
```

This is useful when you want to send sample data directly via OSC without needing a URL - for example, from an external controller or when embedding audio data.

**Reply:** `/done /b_allocFile bufnum`

---

## Unsupported Commands

These commands don't work in SuperSonic due to browser/AudioWorklet constraints.

> For a complete guide to all differences between SuperSonic and scsynth—including unsupported UGens, architectural differences, and error handling—see [SCSYNTH_DIFFERENCES.md](SCSYNTH_DIFFERENCES.md).

### Scheduling and Debug Commands

| Command       | Reason                                                                                                                |
| ------------- | --------------------------------------------------------------------------------------------------------------------- |
| `/clearSched` | Use `cancelAll()` or the fine-grained `cancelTag()`, `cancelSession()`, `cancelSessionTag()` methods instead |
| `/error`      | SuperSonic always enables error notifications so you never miss a `/fail` message                                     |
| `/quit`       | Use `destroy()` to shut down the SuperSonic instance                                                                  |

### Plugin Commands

| Command  | Status                                                               |
| -------- | -------------------------------------------------------------------- |
| `/cmd`   | No commands currently registered                                     |
| `/u_cmd` | No UGens currently define commands                                   |

These commands allow plugins to register custom functionality beyond the standard OSC API. None of the built-in UGens use them, but the mechanism exists if compelling use cases emerge. If you have a need for custom plugin commands, [open an issue](https://github.com/samaaron/supersonic/issues) describing your use case.

### Filesystem Commands

No filesystem in browser/WASM, so file-based commands aren't available:

| Command              | Alternative                                                               |
| -------------------- | ------------------------------------------------------------------------- |
| `/d_load`            | `loadSynthDef()` or `/d_recv` with bytes                                  |
| `/d_loadDir`         | `loadSynthDefs()`                                                         |
| `/b_read`            | `loadSample()`                                                            |
| `/b_readChannel`     | `loadSample()`                                                            |
| `/b_allocRead`       | `loadSample()` or `/b_allocFile` with inline bytes                        |
| `/b_allocReadChannel`| `loadSample()` (channel selection not supported)                          |
| `/b_write`           | Not available                                                             |
| `/b_close`           | Not available                                                             |

### Buffer Commands

| Command           | Reason                                                                           |
| ----------------- | -------------------------------------------------------------------------------- |
| `/b_setSampleRate`| Not implemented - WebAudio automatically resamples buffers to context sample rate |

Use the JavaScript API to load assets - it fetches via HTTP and sends the data to scsynth:

```javascript
await supersonic.loadSynthDef("sonic-pi-beep");
await supersonic.loadSample(0, "loop_amen.flac");
```
