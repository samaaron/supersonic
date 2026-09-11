// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! The flat node-tree mirror JavaScript polls.
//!
//! Asking the engine for the node tree over OSC costs a round trip per
//! answer. That is fine for an occasional query and hopeless for a 60fps
//! visualisation, so clockwork keeps a copy of the tree's *shape* in
//! shared memory: a flat array of entries with sibling links, updated whenever
//! a node is created, freed or moved, and stamped with a version so a reader
//! can tell in one comparison whether anything changed.
//!
//! # What this module does not know
//!
//! Nothing here refers to the engine's `Node` or `Group`. The callers pass the
//! facts — id, parent, siblings, name — and this decides where they go. That
//! is the whole reason the module exists apart from either engine: the mirror
//! is clockwork's idea, and both engines report into it.
//!
//! # Indices
//!
//! Two of them, because a linear scan of a thousand entries on every node
//! event is not free on the audio thread:
//!
//! * a free list, so claiming a slot is O(1);
//! * an open-addressed hash from node id to slot, so finding one is too.
//!
//! The hash deletes by backward shift (Knuth's Algorithm R) rather than
//! tombstones, so a long session of creating and freeing nodes does not
//! degrade into a linear probe over a table full of gravestones.
//!
//! # Overflow is not failure
//!
//! The mirror is bounded. A tree that outgrows it keeps playing — the engine's
//! own tree has no such limit — and the nodes that did not fit are counted, so
//! a reader can tell it is seeing a partial picture rather than a wrong one.

use std::os::raw::{c_char, c_int};
use std::sync::atomic::{AtomicU32, Ordering};

/// Bytes reserved for a name, terminator included.
pub const DEF_NAME_SIZE: usize = 32;

/// The most entries any build mirrors. The real bound comes from the host and
/// is never larger; it sizes the indices, which cannot be allocated at run
/// time on a path that must not allocate.
const MAX_NODES: usize = 4096;

/// Buckets in the id → slot hash. A power of two, and comfortably more than
/// twice `MAX_NODES` so probes stay short at full occupancy.
const HASH_CAPACITY: usize = 16384;
const HASH_MASK: usize = HASH_CAPACITY - 1;
/// Marks a bucket as never used. Not a valid node id.
const HASH_EMPTY: i32 = i32::MIN;

/// The header a reader checks before walking the entries.
///
/// VERSION IS FIRST, and that position is a contract rather than a taste:
/// clockwork reads the first word of `DspConfig::shm_window` to decide when
/// the window is worth copying out, and reads nothing else in it. Everything
/// after this word is ours.
#[repr(C, align(4))]
pub struct NodeTreeHeader {
    /// Bumped on every change, so a reader can skip an unchanged tree — and
    /// so the host knows to snapshot it.
    pub version: AtomicU32,
    /// Entries currently in use.
    pub node_count: AtomicU32,
    /// Nodes the engine has that did not fit here.
    pub dropped_count: AtomicU32,
    pub _padding: u32,
}

/// One node's place in the tree. `id == -1` marks an unused slot.
///
/// Slots are never compacted: a removed entry is marked empty and returned to
/// the free list, which keeps every other slot index stable for a reader that
/// is mid-walk.
#[repr(C, align(8))]
pub struct NodeEntry {
    pub id: i32,
    pub parent_id: i32,
    pub is_group: i32,
    pub prev_id: i32,
    pub next_id: i32,
    pub head_id: i32,
    pub def_name: [c_char; DEF_NAME_SIZE],
    pub uuid_hi: u64,
    pub uuid_lo: u64,
    // Entry v2 — the process-tree semantics, appended so every v1 offset
    // still holds. The session-driven rebuild fills these; the legacy
    // header-walk path zeroes them.
    pub parent_uuid_hi: u64,
    pub parent_uuid_lo: u64,
    /// Peak of the process's output block, refreshed every block.
    pub out_peak: f32,
    /// Synths OWNED by the process — its contents, not its children.
    pub synth_count: u16,
    /// Whether any owned synthdef reads the process input.
    pub listens: u8,
    pub pad: u8,
}

const _: () = assert!(std::mem::size_of::<NodeEntry>() == 96);
const _: () = assert!(std::mem::size_of::<NodeTreeHeader>() == 16);

/// The free list and the hash, which together are the whole reason this is
/// O(1) rather than O(n) per node event.
struct Indices {
    /// `free_next[i]` is the next free slot after `i`; -1 ends the chain.
    /// Slots in use have meaningless entries — they are not on the list.
    free_next: [i16; MAX_NODES],
    free_head: i16,
    /// The next slot never yet handed out.
    ///
    /// With this, a reset does not have to thread a free list through every
    /// slot: it sets this to zero and empties the list. Slots are handed out
    /// from here until they run out, and returned ones go on the list.
    next_fresh: i16,
    /// Open addressing with linear probing.
    hash_key: [i32; HASH_CAPACITY],
    hash_slot: [i16; HASH_CAPACITY],
    /// Which reset each bucket belongs to.
    ///
    /// A bucket is empty unless its stamp matches [`Indices::generation`], so
    /// emptying the table is one increment rather than sixteen thousand
    /// writes. The whole mirror is rebuilt on every change to the tree, and
    /// the old reset walked the entire table however few nodes were in it —
    /// so its cost was set by the mirror's capacity rather than by the size of
    /// the tree being mirrored, which is the wrong thing for it to depend on.
    hash_gen: [u32; HASH_CAPACITY],
    generation: u32,
    /// How many slots this build actually has, from the host.
    capacity: usize,
}

/// The indices, which live for the process.
///
/// A `static mut` in all but name, and deliberately: every caller is the audio
/// thread, running inside a node lifecycle callback, and there is exactly one
/// mirror. A lock here would be a lock on the audio path guarding against a
/// contender that does not exist.
static mut INDICES: Indices = Indices {
    free_next: [-1; MAX_NODES],
    free_head: -1,
    next_fresh: 0,
    hash_key: [HASH_EMPTY; HASH_CAPACITY],
    hash_slot: [-1; HASH_CAPACITY],
    hash_gen: [0; HASH_CAPACITY],
    generation: 1,
    capacity: 0,
};

#[allow(static_mut_refs)]
unsafe fn indices() -> &'static mut Indices {
    &mut INDICES
}

/// A hash good enough to scatter small consecutive ids, which is what node ids
/// are.
fn hash_of(key: i32) -> usize {
    let mut h = key as u32;
    h ^= h >> 16;
    h = h.wrapping_mul(0x45d9_f3b);
    h ^= h >> 16;
    (h as usize) & HASH_MASK
}

impl Indices {
    /// Whether a bucket holds an entry from the current generation.
    fn occupied(&self, i: usize) -> bool {
        self.hash_gen[i] == self.generation && self.hash_key[i] != HASH_EMPTY
    }

    fn insert(&mut self, key: i32, slot: i16) {
        let mut i = hash_of(key);
        while self.occupied(i) {
            i = (i + 1) & HASH_MASK;
        }
        self.hash_key[i] = key;
        self.hash_slot[i] = slot;
        self.hash_gen[i] = self.generation;
    }

    fn find(&self, key: i32) -> i32 {
        let mut i = hash_of(key);
        while self.occupied(i) {
            if self.hash_key[i] == key {
                return self.hash_slot[i] as i32;
            }
            i = (i + 1) & HASH_MASK;
        }
        -1
    }

    /// Backward-shift deletion: no tombstones, so probe lengths do not grow
    /// over a long session of adds and removes.
    fn remove(&mut self, key: i32) {
        let mut i = hash_of(key);
        while self.occupied(i) {
            if self.hash_key[i] == key {
                loop {
                    self.hash_key[i] = HASH_EMPTY;
                    let mut j = i;
                    loop {
                        j = (j + 1) & HASH_MASK;
                        if !self.occupied(j) {
                            return;
                        }
                        // The natural bucket of whatever sits at j. If it lies
                        // cyclically within (i, j] it is already reachable and
                        // can stay; otherwise moving it to i keeps its probe
                        // chain intact.
                        let r = hash_of(self.hash_key[j]);
                        let stays =
                            if i <= j { i < r && r <= j } else { i < r || r <= j };
                        if !stays {
                            break;
                        }
                    }
                    self.hash_key[i] = self.hash_key[j];
                    self.hash_slot[i] = self.hash_slot[j];
                    self.hash_gen[i] = self.generation;
                    i = j;
                }
            }
            i = (i + 1) & HASH_MASK;
        }
    }
}

/// Everything the mirror needs to know about a node, without knowing what a
/// node is.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct NodeFacts {
    pub id: i32,
    /// -1 when the node is the root group.
    pub parent_id: i32,
    /// Non-zero for a group.
    pub is_group: c_int,
    /// -1 when the node is its parent's first child.
    pub prev_id: i32,
    /// -1 when the node is its parent's last child.
    pub next_id: i32,
    /// A group's first child, -1 otherwise.
    pub head_id: i32,
    /// The synthdef's name, or null. Groups are named "group" whatever this
    /// says.
    pub def_name: *const c_char,
}

unsafe fn write_name(entry: *mut NodeEntry, name: *const c_char, is_group: bool) {
    let dst = (*entry).def_name.as_mut_ptr();
    let src: &[u8] = if is_group {
        b"group"
    } else if name.is_null() {
        b"unknown"
    } else {
        let mut n = 0usize;
        while n < DEF_NAME_SIZE - 1 && *name.add(n) != 0 {
            n += 1;
        }
        std::slice::from_raw_parts(name as *const u8, n)
    };
    let n = src.len().min(DEF_NAME_SIZE - 1);
    std::ptr::copy_nonoverlapping(src.as_ptr() as *const c_char, dst, n);
    for k in n..DEF_NAME_SIZE {
        *dst.add(k) = 0;
    }
}

unsafe fn entry_at(entries: *mut NodeEntry, slot: i32) -> *mut NodeEntry {
    entries.add(slot as usize)
}

// ── C ABI ───────────────────────────────────────────────────────────────────

/// Build the free list and clear the hash.
///
/// Called once, after the mirror's memory is zeroed and every entry marked
/// empty. `capacity` is how many entries this build has.
#[no_mangle]
pub unsafe extern "C" fn supersonic_node_mirror_init(capacity: usize) {
    let ix = indices();
    ix.capacity = capacity.min(MAX_NODES);
    // Both in constant time. This runs on every change to the node tree —
    // every synth created, every one freed — and the obvious version threaded
    // a free list through four thousand slots and cleared sixteen thousand
    // hash buckets each time, whether the tree held one node or a thousand.
    ix.next_fresh = 0;
    ix.free_head = -1;
    ix.generation = ix.generation.wrapping_add(1);
    if ix.generation == 0 {
        // Wrapped, which takes four billion rebuilds: clear for real, so a
        // stale bucket from generation zero cannot be mistaken for a live one.
        ix.generation = 1;
        ix.hash_gen = [0; HASH_CAPACITY];
    }
}

/// Claim a slot: from the never-used run first, then from returned ones.
///
/// -1 when there are none, which is not an error — the mirror is bounded and
/// the tree is not.
fn take_slot(ix: &mut Indices) -> i32 {
    if (ix.next_fresh as usize) < ix.capacity {
        let slot = ix.next_fresh;
        ix.next_fresh += 1;
        return slot as i32;
    }
    let slot = ix.free_head;
    if slot >= 0 {
        ix.free_head = ix.free_next[slot as usize];
    }
    slot as i32
}

/// The slot holding `node_id`, or -1.
#[no_mangle]
pub unsafe extern "C" fn supersonic_node_mirror_find(node_id: i32) -> i32 {
    indices().find(node_id)
}

/// The next slot a node would be given, or -1 when the mirror is full.
#[no_mangle]
pub unsafe extern "C" fn supersonic_node_mirror_next_slot() -> i32 {
    let ix = indices();
    if (ix.next_fresh as usize) < ix.capacity {
        ix.next_fresh as i32
    } else {
        ix.free_head as i32
    }
}

/// How many nodes did not fit, so far.
#[no_mangle]
pub unsafe extern "C" fn supersonic_node_mirror_dropped(header: *const NodeTreeHeader) -> u32 {
    if header.is_null() {
        return 0;
    }
    (*header).dropped_count.load(Ordering::Relaxed)
}

/// Record a node that has just been created.
///
/// # Safety
/// `header` and `entries` must point at the mirror, and `entries` must have at
/// least the capacity passed to [`supersonic_node_mirror_init`].
#[no_mangle]
pub unsafe extern "C" fn supersonic_node_mirror_add(
    facts: *const NodeFacts,
    header: *mut NodeTreeHeader,
    entries: *mut NodeEntry,
) {
    if facts.is_null() || header.is_null() || entries.is_null() {
        return;
    }
    let f = *facts;
    let ix = indices();

    let slot = take_slot(ix);
    if slot < 0 {
        // The engine's tree has no such limit, so the audio is unaffected;
        // count it so a reader knows its picture is partial.
        (*header).dropped_count.fetch_add(1, Ordering::Relaxed);
        return;
    }


    let is_group = f.is_group != 0;
    let entry = entry_at(entries, slot);
    (*entry).id = f.id;
    (*entry).parent_id = f.parent_id;
    (*entry).is_group = if is_group { 1 } else { 0 };
    (*entry).prev_id = f.prev_id;
    (*entry).next_id = f.next_id;
    (*entry).head_id = if is_group { f.head_id } else { -1 };
    write_name(entry, f.def_name, is_group);
    // The caller populates these from its own reverse lookup if it has one.
    (*entry).parent_uuid_hi = 0;
    (*entry).parent_uuid_lo = 0;
    (*entry).out_peak = 0.0;
    (*entry).synth_count = 0;
    (*entry).listens = 0;
    (*entry).pad = 0;
    (*entry).uuid_hi = 0;
    (*entry).uuid_lo = 0;

    // Indexed before the sibling patching below, which looks nodes up.
    ix.insert(f.id, slot as i16);

    relink_neighbours(ix, entries, f.prev_id, f.next_id, f.parent_id, f.id);

    let count = (*header).node_count.load(Ordering::Relaxed);
    (*header).node_count.store(count + 1, Ordering::Relaxed);
    (*header).version.fetch_add(1, Ordering::Release);
}

/// Point the neighbours at `id`: its previous sibling forward, its next
/// sibling back, and its parent's head at it when it is first.
unsafe fn relink_neighbours(
    ix: &Indices,
    entries: *mut NodeEntry,
    prev_id: i32,
    next_id: i32,
    parent_id: i32,
    id: i32,
) {
    if prev_id != -1 {
        let s = ix.find(prev_id);
        if s >= 0 {
            (*entry_at(entries, s)).next_id = id;
        }
    }
    if next_id != -1 {
        let s = ix.find(next_id);
        if s >= 0 {
            (*entry_at(entries, s)).prev_id = id;
        }
    }
    if parent_id != -1 && prev_id == -1 {
        let s = ix.find(parent_id);
        if s >= 0 {
            (*entry_at(entries, s)).head_id = id;
        }
    }
}

/// Forget a node that has just been freed.
///
/// # Safety
/// As [`supersonic_node_mirror_add`].
#[no_mangle]
pub unsafe extern "C" fn supersonic_node_mirror_remove(
    node_id: i32,
    header: *mut NodeTreeHeader,
    entries: *mut NodeEntry,
) {
    if header.is_null() || entries.is_null() {
        return;
    }
    let ix = indices();
    let slot = ix.find(node_id);
    if slot < 0 {
        // The engine had this node but the mirror never did: it was dropped on
        // overflow. This is only reached for nodes that really existed, so the
        // drop count can safely come back down.
        if (*header).dropped_count.load(Ordering::Relaxed) > 0 {
            (*header).dropped_count.fetch_sub(1, Ordering::Relaxed);
        }
        return;
    }

    let entry = entry_at(entries, slot);
    let (prev_id, next_id, parent_id) = ((*entry).prev_id, (*entry).next_id, (*entry).parent_id);

    // Close the gap: the siblings on either side become each other's.
    if prev_id != -1 {
        let s = ix.find(prev_id);
        if s >= 0 {
            (*entry_at(entries, s)).next_id = next_id;
        }
    }
    if next_id != -1 {
        let s = ix.find(next_id);
        if s >= 0 {
            (*entry_at(entries, s)).prev_id = prev_id;
        }
    }
    if parent_id != -1 && prev_id == -1 {
        let s = ix.find(parent_id);
        if s >= 0 {
            // The next sibling becomes the head, or the group empties.
            (*entry_at(entries, s)).head_id = next_id;
        }
    }

    ix.remove(node_id);
    (*entry).id = -1;
    (*entry).uuid_hi = 0;
    (*entry).uuid_lo = 0;

    ix.free_next[slot as usize] = ix.free_head;
    ix.free_head = slot as i16;

    let count = (*header).node_count.load(Ordering::Relaxed);
    if count > 0 {
        (*header).node_count.store(count - 1, Ordering::Relaxed);
    }
    (*header).version.fetch_add(1, Ordering::Release);
}

/// Record a node that has just moved.
///
/// # Safety
/// As [`supersonic_node_mirror_add`].
#[no_mangle]
pub unsafe extern "C" fn supersonic_node_mirror_update(
    facts: *const NodeFacts,
    header: *mut NodeTreeHeader,
    entries: *mut NodeEntry,
) {
    if facts.is_null() || header.is_null() || entries.is_null() {
        return;
    }
    let f = *facts;
    let ix = indices();

    let slot = ix.find(f.id);
    if slot < 0 {
        // Not mirrored yet, which should not happen — but adding it is a
        // better answer than dropping the update.
        supersonic_node_mirror_add(facts, header, entries);
        return;
    }

    let entry = entry_at(entries, slot);
    let (old_prev, old_next, old_parent) =
        ((*entry).prev_id, (*entry).next_id, (*entry).parent_id);

    (*entry).parent_id = f.parent_id;
    (*entry).prev_id = f.prev_id;
    (*entry).next_id = f.next_id;
    if f.is_group != 0 {
        (*entry).head_id = f.head_id;
    }

    // Close the hole the node left behind.
    if old_prev != -1 {
        let s = ix.find(old_prev);
        if s >= 0 {
            (*entry_at(entries, s)).next_id = old_next;
        }
    }
    if old_next != -1 {
        let s = ix.find(old_next);
        if s >= 0 {
            (*entry_at(entries, s)).prev_id = old_prev;
        }
    }
    // Only if it was still the old parent's head: a move within one group can
    // leave the head where it was.
    if old_parent != -1 && old_prev == -1 {
        let s = ix.find(old_parent);
        if s >= 0 && (*entry_at(entries, s)).head_id == f.id {
            (*entry_at(entries, s)).head_id = old_next;
        }
    }

    relink_neighbours(ix, entries, f.prev_id, f.next_id, f.parent_id, f.id);

    // The tree's shape changed but its population did not.
    (*header).version.fetch_add(1, Ordering::Release);
}

#[cfg(test)]
mod tests {
    use super::*;

    const CAP: usize = 64;

    struct Mirror {
        header: Box<NodeTreeHeader>,
        entries: Vec<NodeEntry>,
    }

    impl Mirror {
        fn new() -> Mirror {
            let mut entries = Vec::with_capacity(CAP);
            for _ in 0..CAP {
                let mut e: NodeEntry = unsafe { std::mem::zeroed() };
                e.id = -1;
                entries.push(e);
            }
            unsafe { supersonic_node_mirror_init(CAP) };
            Mirror {
                header: Box::new(NodeTreeHeader {
                    node_count: AtomicU32::new(0),
                    version: AtomicU32::new(0),
                    dropped_count: AtomicU32::new(0),
                    _padding: 0,
                }),
                entries,
            }
        }

        fn add(&mut self, id: i32, parent: i32, prev: i32, next: i32, group: bool) {
            let f = NodeFacts {
                id,
                parent_id: parent,
                is_group: group as c_int,
                prev_id: prev,
                next_id: next,
                head_id: -1,
                def_name: std::ptr::null(),
            };
            unsafe {
                supersonic_node_mirror_add(&f, &mut *self.header, self.entries.as_mut_ptr());
            }
        }

        fn remove(&mut self, id: i32) {
            unsafe {
                supersonic_node_mirror_remove(id, &mut *self.header, self.entries.as_mut_ptr());
            }
        }

        fn get(&self, id: i32) -> &NodeEntry {
            let slot = unsafe { supersonic_node_mirror_find(id) };
            assert!(slot >= 0, "node {id} is not in the mirror");
            &self.entries[slot as usize]
        }

        fn count(&self) -> u32 {
            self.header.node_count.load(Ordering::Relaxed)
        }
        fn version(&self) -> u32 {
            self.header.version.load(Ordering::Relaxed)
        }
        fn dropped(&self) -> u32 {
            self.header.dropped_count.load(Ordering::Relaxed)
        }
    }

    // The indices are process-wide, so the tests share them; one lock keeps
    // them from interleaving. Not a property of the module — a property of
    // there being one mirror, which is also true in the engine.
    static LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

    #[test]
    fn an_added_node_is_findable_and_counted() {
        let _g = LOCK.lock().unwrap();
        let mut m = Mirror::new();
        m.add(1000, -1, -1, -1, false);
        assert_eq!(m.get(1000).id, 1000);
        assert_eq!(m.count(), 1);
        assert!(m.version() > 0, "a change should bump the version");
    }

    #[test]
    fn a_group_is_named_group_whatever_it_was_given() {
        let _g = LOCK.lock().unwrap();
        let mut m = Mirror::new();
        m.add(1, -1, -1, -1, true);
        let name = &m.get(1).def_name;
        let s: Vec<u8> = name.iter().take_while(|&&c| c != 0).map(|&c| c as u8).collect();
        assert_eq!(String::from_utf8(s).unwrap(), "group");
    }

    #[test]
    fn removing_the_middle_links_its_neighbours() {
        // A -> B -> C, remove B, and A and C should become each other's.
        let _g = LOCK.lock().unwrap();
        let mut m = Mirror::new();
        m.add(1, -1, -1, -1, true);
        m.add(10, 1, -1, 11, false);
        m.add(11, 1, 10, 12, false);
        m.add(12, 1, 11, -1, false);
        m.remove(11);
        assert_eq!(m.get(10).next_id, 12);
        assert_eq!(m.get(12).prev_id, 10);
        assert_eq!(m.count(), 3);
    }

    #[test]
    fn removing_the_head_promotes_the_next_child() {
        let _g = LOCK.lock().unwrap();
        let mut m = Mirror::new();
        m.add(1, -1, -1, -1, true);
        m.add(10, 1, -1, 11, false);
        m.add(11, 1, 10, -1, false);
        assert_eq!(m.get(1).head_id, 10);
        m.remove(10);
        assert_eq!(m.get(1).head_id, 11);
    }

    #[test]
    fn removing_the_only_child_empties_the_group() {
        let _g = LOCK.lock().unwrap();
        let mut m = Mirror::new();
        m.add(1, -1, -1, -1, true);
        m.add(10, 1, -1, -1, false);
        m.remove(10);
        assert_eq!(m.get(1).head_id, -1);
    }

    #[test]
    fn a_move_patches_both_ends() {
        let _g = LOCK.lock().unwrap();
        let mut m = Mirror::new();
        m.add(1, -1, -1, -1, true);
        m.add(2, -1, 1, -1, true);
        m.add(10, 1, -1, 11, false);
        m.add(11, 1, 10, -1, false);

        // Move 10 out of group 1 and into group 2.
        let f = NodeFacts {
            id: 10,
            parent_id: 2,
            is_group: 0,
            prev_id: -1,
            next_id: -1,
            head_id: -1,
            def_name: std::ptr::null(),
        };
        unsafe { supersonic_node_mirror_update(&f, &mut *m.header, m.entries.as_mut_ptr()) };

        assert_eq!(m.get(1).head_id, 11, "the old group's head should move on");
        assert_eq!(m.get(11).prev_id, -1, "the sibling left behind is now first");
        assert_eq!(m.get(2).head_id, 10, "the new group should point at it");
        assert_eq!(m.get(10).parent_id, 2);
    }

    #[test]
    fn slots_are_reused_but_never_shuffled() {
        let _g = LOCK.lock().unwrap();
        let mut m = Mirror::new();
        m.add(1, -1, -1, -1, false);
        m.add(2, -1, -1, -1, false);
        let slot_of_2 = unsafe { supersonic_node_mirror_find(2) };
        m.remove(1);
        // Removing 1 must not move 2: a reader mid-walk holds slot indices.
        assert_eq!(unsafe { supersonic_node_mirror_find(2) }, slot_of_2);
        // And the freed slot comes back for the next node.
        m.add(3, -1, -1, -1, false);
        assert!(unsafe { supersonic_node_mirror_find(3) } >= 0);
    }

    #[test]
    fn overflow_is_counted_rather_than_fatal() {
        let _g = LOCK.lock().unwrap();
        let mut m = Mirror::new();
        for id in 0..(CAP as i32) {
            m.add(id + 1, -1, -1, -1, false);
        }
        assert_eq!(m.count(), CAP as u32);
        m.add(9999, -1, -1, -1, false);
        assert_eq!(m.dropped(), 1, "the node that did not fit should be counted");
        assert_eq!(m.count(), CAP as u32, "and should not be counted twice");

        // Freeing a node the mirror never held brings the drop count back down
        // rather than the population.
        m.remove(9999);
        assert_eq!(m.dropped(), 0);
        assert_eq!(m.count(), CAP as u32);
    }

    #[test]
    fn a_rebuild_leaves_nothing_of_the_last_one() {
        // The reset does not clear the table — it stamps a new generation and
        // lets the old entries be ignored, so the property is not visible in
        // the code and needs a test: after a rebuild, nothing from before it
        // can be found, and the slots start again.
        let _g = LOCK.lock().unwrap();
        let mut m = Mirror::new();
        for id in 1..=50 {
            m.add(id, -1, -1, -1, false);
        }
        let slot_of_1 = unsafe { supersonic_node_mirror_find(1) };
        assert!(slot_of_1 >= 0);

        m = Mirror::new(); // rebuilds, as the engine does on any tree change

        for id in 1..=50 {
            assert_eq!(unsafe { supersonic_node_mirror_find(id) }, -1, "id {id} outlived the rebuild");
        }
        // And a node added after it gets the slot the old one had, rather than
        // the table having quietly filled up.
        m.add(1, -1, -1, -1, false);
        assert_eq!(unsafe { supersonic_node_mirror_find(1) }, slot_of_1);
    }

    #[test]
    fn rebuilds_do_not_accumulate() {
        // A generation stamp only works if it advances. If a rebuild ever
        // reused the previous generation, entries from the previous tree would
        // still be visible — so churn the rebuild itself, not just the table.
        let _g = LOCK.lock().unwrap();
        for round in 1..=200i32 {
            let mut m = Mirror::new();
            m.add(round, -1, -1, -1, false);
            assert!(unsafe { supersonic_node_mirror_find(round) } >= 0);
            assert_eq!(
                unsafe { supersonic_node_mirror_find(round - 1) },
                -1,
                "the previous round's node survived into round {round}"
            );
            assert_eq!(m.count(), 1, "round {round} should hold exactly its own node");
        }
    }

    #[test]
    fn the_hash_survives_a_long_churn() {
        // Backward-shift deletion is the reason this stays fast; a tombstoned
        // table would still be *correct* here, so what this really guards is
        // that deletion does not lose entries as chains are rebuilt.
        let _g = LOCK.lock().unwrap();
        let mut m = Mirror::new();
        let mut live: Vec<i32> = Vec::new();
        let mut next_id = 1;
        for round in 0..2_000 {
            if live.len() >= CAP || (round % 3 == 0 && !live.is_empty()) {
                let id = live.remove(round % live.len());
                m.remove(id);
                assert_eq!(unsafe { supersonic_node_mirror_find(id) }, -1, "removed id still found");
            } else {
                m.add(next_id, -1, -1, -1, false);
                live.push(next_id);
                next_id += 1;
            }
            for &id in &live {
                assert!(unsafe { supersonic_node_mirror_find(id) } >= 0, "live id {id} lost");
            }
        }
    }
}
