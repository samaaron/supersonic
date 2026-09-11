//! Node containment: which synths and groups exist, and in what order they run.
//!
//! This is an independent reimplementation of scsynth's node tree, written
//! against observed behaviour and checked against the engine by the
//! differential tests in `supersonic-seam`.
//!
//! Deliberately not called clean-room. Clean-room means the implementer never
//! saw the original, with a separate team writing a specification from it;
//! that is not what happened here, and the weaker claim is the accurate one.
//! What can be said is narrower and still useful: the structure is not the
//! engine's — its list is intrusive and doubly linked, this is a map of
//! children — and every behaviour is pinned by a test that compares against
//! the engine rather than by transcription.
//!
//! Order is not bookkeeping. The tree is walked depth-first every block, and
//! that walk decides the order in which synths accumulate onto buses. Float
//! addition is not associative, so rearranging the tree changes the samples —
//! which is why this has an oracle rather than just unit tests.
//!
//! Unlike the structs the ugens see, nothing here needs scsynth's memory
//! layout. `mPrev`, `mNext` and `mParent` exist so that C server code can walk
//! a list; a ugen never sees a `Node`. With the server side in Rust, only the
//! semantics have to match, and this is free to be an ordinary Rust structure.

use std::collections::BTreeMap;

/// The root group, id 0 in a running server. Always present, never freed.
pub const ROOT: i32 = 0;

/// Where a new node goes, relative to a target — the `/s_new` and `/g_new`
/// add action.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AddAction {
    /// First child of the target group.
    Head,
    /// Last child of the target group.
    Tail,
    /// Immediately before the target node, under the target's parent.
    Before,
    /// Immediately after the target node, under the target's parent.
    After,
}

impl AddAction {
    /// The wire encoding, as it arrives in an OSC message.
    pub fn from_i32(v: i32) -> Option<AddAction> {
        match v {
            0 => Some(AddAction::Head),
            1 => Some(AddAction::Tail),
            2 => Some(AddAction::Before),
            3 => Some(AddAction::After),
            _ => None,
        }
    }
}

/// Why a placement was refused.
///
/// The engine refuses rather than panics, and so does this: a session
/// replaying a capture should report what it could not do and carry on.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TreeError {
    /// No node with this id.
    UnknownNode(i32),
    /// Head or tail placement named a node that is not a group.
    NotAGroup(i32),
    /// Before or after placement named a node with no parent — in practice the
    /// root, which cannot have siblings.
    NoParent(i32),
    /// Id 0 is the root and cannot be added; the engine refuses it outright.
    ReservedId,
    /// A node with this id already exists.
    Duplicate(i32),
}

#[derive(Debug, Clone)]
struct Entry {
    parent: Option<i32>,
    /// Empty and unused for synths.
    children: Vec<i32>,
    is_group: bool,
}

/// The node tree: every live node, and the order they run in.
#[derive(Debug, Clone)]
pub struct Tree {
    nodes: BTreeMap<i32, Entry>,
}

impl Default for Tree {
    fn default() -> Self {
        Self::new()
    }
}

impl Tree {
    /// A tree containing only the root group.
    pub fn new() -> Tree {
        let mut nodes = BTreeMap::new();
        nodes.insert(ROOT, Entry { parent: None, children: Vec::new(), is_group: true });
        Tree { nodes }
    }

    /// Add a node, placed relative to `target`.
    ///
    /// `is_group` decides whether the new node can itself hold children; the
    /// engine distinguishes these at creation (`/s_new` versus `/g_new`) and
    /// never converts one into the other.
    pub fn add(
        &mut self,
        id: i32,
        is_group: bool,
        action: AddAction,
        target: i32,
    ) -> Result<(), TreeError> {
        if id == ROOT {
            return Err(TreeError::ReservedId);
        }
        if self.nodes.contains_key(&id) {
            return Err(TreeError::Duplicate(id));
        }

        let (parent, index) = self.slot_for(action, target)?;
        self.nodes.insert(id, Entry { parent: Some(parent), children: Vec::new(), is_group });
        self.nodes.get_mut(&parent).expect("parent checked above").children.insert(index, id);
        Ok(())
    }

    /// Which parent a placement lands in, and at which index among its
    /// children. Resolved before mutating anything so that a refused placement
    /// leaves the tree untouched.
    fn slot_for(&self, action: AddAction, target: i32) -> Result<(i32, usize), TreeError> {
        match action {
            AddAction::Head | AddAction::Tail => {
                let entry = self.nodes.get(&target).ok_or(TreeError::UnknownNode(target))?;
                if !entry.is_group {
                    return Err(TreeError::NotAGroup(target));
                }
                let index = match action {
                    AddAction::Head => 0,
                    _ => entry.children.len(),
                };
                Ok((target, index))
            }
            AddAction::Before | AddAction::After => {
                let entry = self.nodes.get(&target).ok_or(TreeError::UnknownNode(target))?;
                let parent = entry.parent.ok_or(TreeError::NoParent(target))?;
                let siblings = &self.nodes[&parent].children;
                let pos = siblings
                    .iter()
                    .position(|c| *c == target)
                    .expect("a node's parent lists it as a child");
                Ok((parent, if action == AddAction::Before { pos } else { pos + 1 }))
            }
        }
    }

    /// Remove a node and everything beneath it. Removing the root is ignored,
    /// as `/n_free 0` is in the engine.
    ///
    /// Returns the ids removed, deepest last, so a caller can drop whatever
    /// state it holds for them.
    pub fn remove(&mut self, id: i32) -> Vec<i32> {
        if id == ROOT || !self.nodes.contains_key(&id) {
            return Vec::new();
        }
        if let Some(parent) = self.nodes[&id].parent {
            if let Some(p) = self.nodes.get_mut(&parent) {
                p.children.retain(|c| *c != id);
            }
        }
        let mut removed = Vec::new();
        self.take(id, &mut removed);
        removed
    }

    fn take(&mut self, id: i32, out: &mut Vec<i32>) {
        let Some(entry) = self.nodes.remove(&id) else { return };
        out.push(id);
        for child in entry.children {
            self.take(child, out);
        }
    }

    /// Free a group's children without freeing the group — `/g_freeAll`.
    pub fn free_all(&mut self, group: i32) -> Result<Vec<i32>, TreeError> {
        let entry = self.nodes.get(&group).ok_or(TreeError::UnknownNode(group))?;
        if !entry.is_group {
            return Err(TreeError::NotAGroup(group));
        }
        let children = entry.children.clone();
        let mut removed = Vec::new();
        for child in children {
            removed.extend(self.remove(child));
        }
        Ok(removed)
    }

    /// Every node in execution order: a depth-first walk from the root.
    ///
    /// Groups appear before their children, which is what lets a caller treat
    /// this as both a render order and a tree dump.
    pub fn order(&self) -> Vec<i32> {
        let mut out = Vec::new();
        self.walk(ROOT, &mut out);
        out
    }

    /// Execution order restricted to synths — what a renderer iterates.
    pub fn synth_order(&self) -> Vec<i32> {
        self.order().into_iter().filter(|id| !self.is_group(*id)).collect()
    }

    fn walk(&self, id: i32, out: &mut Vec<i32>) {
        let Some(entry) = self.nodes.get(&id) else { return };
        out.push(id);
        for child in &entry.children {
            self.walk(*child, out);
        }
    }

    /// The direct children of a group, in order.
    pub fn children(&self, id: i32) -> &[i32] {
        self.nodes.get(&id).map(|e| e.children.as_slice()).unwrap_or(&[])
    }

    /// A node's parent, or `None` for the root and for unknown ids.
    pub fn parent(&self, id: i32) -> Option<i32> {
        self.nodes.get(&id).and_then(|e| e.parent)
    }

    pub fn contains(&self, id: i32) -> bool {
        self.nodes.contains_key(&id)
    }

    pub fn is_group(&self, id: i32) -> bool {
        self.nodes.get(&id).map(|e| e.is_group).unwrap_or(false)
    }

    /// Live nodes including the root.
    pub fn len(&self) -> usize {
        self.nodes.len()
    }

    pub fn is_empty(&self) -> bool {
        self.nodes.len() <= 1
    }
}
