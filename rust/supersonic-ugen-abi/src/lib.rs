//! The memory layout the C++ ugens are compiled against.
//!
//! In the target design the Rust host owns these structs and the ugens are C
//! leaf functions on the other side of the boundary. Before any of that can
//! happen, Rust's idea of where each field lives has to agree with the
//! compiler's — byte for byte, not approximately.
//!
//! This kind of mistake does not surface the way the graph work's mistakes do.
//! A wrong buffer index shows up as a diff in the rendered audio; a wrong
//! struct offset shows up as a ugen reading `mBufLength` out of the middle of
//! a pointer, and whether that crashes or quietly produces plausible-sounding
//! audio is luck. So every struct here is asserted against
//! `supersonic_layout_dump` — the compiler's own `offsetof`/`sizeof` — in
//! `tests/layout.rs`, field by field.
//!
//! Nothing here reimplements engine *behaviour*. These are declarations of an
//! ABI: what the ugens already assume, written down where Rust can see it.
//!
//! Layouts verified on x86_64 Linux, gcc 13.3, **in the scsynth profile**. The
//! test is the definition of "verified"; a different target may lay these out
//! differently and would need its own run.
//!
//! The profile matters as much as the target. Building the same headers with
//! `SUPERNOVA` defined gives `SndBuf` two more fields (`isLocal` and an
//! `rw_spinlock`), so a Rust struct written against one profile and linked
//! against the other is silently the wrong size, with no compile error
//! anywhere. `supersonic_layout_dump` is built with the same definitions as the
//! engine it is compared against, which is what keeps the two in step.

#![allow(non_snake_case)]

use std::os::raw::{c_char, c_int, c_void};

/// `int32` calculation rate. Matches `SC_Rate.h`.
pub const CALC_SCALAR_RATE: i32 = 0;
pub const CALC_BUF_RATE: i32 = 1;
pub const CALC_FULL_RATE: i32 = 2;
pub const CALC_DEMAND_RATE: i32 = 3;

/// One connection between units, or one constant.
///
/// A scalar-rate wire's `mBuffer` points at its own `mScalarValue`; an
/// audio-rate wire's points into the interconnect buffer space.
#[repr(C)]
#[derive(Debug)]
pub struct Wire {
    pub mFromUnit: *mut Unit,
    pub mCalcRate: i32,
    pub mBuffer: *mut f32,
    pub mScalarValue: f32,
}

/// Per-rate timing constants, precomputed once per World.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct Rate {
    pub mSampleRate: f64,
    pub mSampleDur: f64,
    pub mBufDuration: f64,
    pub mBufRate: f64,
    pub mSlopeFactor: f64,
    pub mRadiansPerSample: f64,
    pub mBufLength: c_int,
    pub mFilterLoops: c_int,
    pub mFilterRemain: c_int,
    pub mFilterSlope: f64,
}

/// A sound buffer. `mask`/`mask1` are the power-of-two wrap masks the delay
/// lines and interpolating oscillators index with.
#[repr(C)]
#[derive(Debug)]
pub struct SndBuf {
    pub samplerate: f64,
    pub sampledur: f64,
    pub data: *mut f32,
    pub channels: c_int,
    pub samples: c_int,
    pub frames: c_int,
    pub mask: c_int,
    pub mask1: c_int,
    pub coord: c_int,
    pub sndfile: *mut c_void,
}

pub type UnitCtorFunc = Option<unsafe extern "C" fn(*mut Unit)>;
pub type UnitDtorFunc = Option<unsafe extern "C" fn(*mut Unit)>;
pub type UnitCalcFunc = Option<unsafe extern "C" fn(*mut Unit, i32)>;
pub type NodeCalcFunc = Option<unsafe extern "C" fn(*mut Node, c_int)>;

/// One ugen instance.
///
/// `mInBuf`/`mOutBuf` are the fast path — caches of each wire's `mBuffer`, so
/// the calc functions never chase `mInput[i]->mBuffer`. Both must be kept
/// consistent with the wires by whoever builds the unit.
#[repr(C)]
#[derive(Debug)]
pub struct Unit {
    pub mWorld: *mut World,
    pub mUnitDef: *mut c_void,
    pub mParent: *mut Graph,
    pub mNumInputs: u32,
    pub mNumOutputs: u32,
    pub mCalcRate: i16,
    /// Selector for the ops ugens (which operator this BinaryOpUGen is).
    pub mSpecialIndex: i16,
    pub mParentIndex: i16,
    /// Set by a ugen to signal `doneAction` handling.
    pub mDone: i16,
    pub mInput: *mut *mut Wire,
    pub mOutput: *mut *mut Wire,
    pub mRate: *mut Rate,
    /// `SC_Unit_Extensions*` — the header calls it "future proofing and
    /// backwards compatibility"; nothing in this engine sets it. Opaque, but
    /// it must be *here*: leaving it out silently shifts every later field by
    /// eight bytes.
    pub mExtensions: *mut c_void,
    pub mInBuf: *mut *mut f32,
    pub mOutBuf: *mut *mut f32,
    pub mCalcFunc: UnitCalcFunc,
    pub mBufLength: i32,
}

/// The node-tree base of both graphs and groups.
#[repr(C)]
#[derive(Debug)]
pub struct Node {
    pub mID: i32,
    pub mHash: i32,
    pub mWorld: *mut World,
    pub mDef: *mut c_void,
    pub mCalcFunc: NodeCalcFunc,
    pub mPrev: *mut Node,
    pub mNext: *mut Node,
    pub mParent: *mut c_void,
    pub mIsGroup: i32,
}

/// What the host hands `World_New`.
///
/// No padding is written out by hand anywhere in this file, and that is
/// deliberate. These structs are compiled for x86-64 and for wasm32, where
/// pointers are half the width; a padding field sized for one target silently
/// moves every later field on the other. `repr(C)` computes it correctly for
/// whichever is being built. The layout oracle checks the native answer — the
/// WebAssembly one is checked by the browser suite, which is where a wrong
/// offset showed up as an engine that never replied to anything.
///
/// Laid out exactly as the C++ struct, gaps included: the padding at offsets
/// 52-63, 65-111 and 120-199 holds fields this engine does not read — shared
/// controls, the non-realtime filenames, verbosity, the plugin path — and the
/// oracle checks the offsets of the ones it does.
#[repr(C)]
#[derive(Debug)]
pub struct WorldOptions {
    pub mPassword: *const c_char,
    pub mNumBuffers: u32,
    pub mMaxLogins: u32,
    pub mMaxNodes: u32,
    pub mMaxGraphDefs: u32,
    pub mMaxWireBufs: u32,
    pub mNumAudioBusChannels: u32,
    pub mNumInputBusChannels: u32,
    pub mNumOutputBusChannels: u32,
    pub mNumControlBusChannels: u32,
    pub mBufLength: u32,
    pub mRealTimeMemorySize: u32,
    pub mNumSharedControls: c_int,
    // No padding here: mNumSharedControls ends at 56, which a pointer is
    // already aligned for. Guessing otherwise pushed every later field eight
    // bytes along, and the oracle said so on the first run.
    pub mSharedControls: *mut f32,
    pub mRealTime: bool,
    pub mMemoryLocking: bool,
    pub mSafetyClipThreshold: f32,
    pub mNonRealTimeCmdFilename: *const c_char,
    pub mNonRealTimeInputFilename: *const c_char,
    pub mNonRealTimeOutputFilename: *const c_char,
    pub mNonRealTimeOutputHeaderFormat: *const c_char,
    pub mNonRealTimeOutputSampleFormat: *const c_char,
    pub mPreferredSampleRate: u32,
    pub mNumRGens: u32,
    pub mPreferredHardwareBufferFrameSize: u32,
    pub mLoadGraphDefs: u32,
    pub mInputStreamsEnabled: *const c_char,
    pub mOutputStreamsEnabled: *const c_char,
    pub mInDeviceName: *const c_char,
    pub mVerbosity: c_int,
    pub mRendezvous: bool,
    pub mUGensPluginPath: *const c_char,
    pub mOutDeviceName: *const c_char,
    pub mRestrictedPath: *const c_char,
    pub mSharedMemoryID: c_int,
    /// Shared memory the caller already made and keeps ownership of, so it can
    /// survive a world being destroyed and rebuilt.
    pub mExternalSharedMemory: *mut c_void,
}

/// Where a reply goes.
///
/// SuperSonic identifies the target by the token in `mReplyData` and routes it
/// with `mReplyFunc`; the network address is a placeholder on every build. So
/// a reply is one indirect call, and the engine needs to know nothing about
/// the transport.
#[repr(C)]
#[derive(Debug)]
pub struct ReplyAddress {
    pub mAddressPlaceholder: [u32; 4],
    pub mProtocol: c_int,
    pub mPort: c_int,
    pub mSocket: c_int,
    pub mReplyFunc: ReplyFunc,
    pub mReplyData: *mut c_void,
}

/// Hands one reply to the transport.
pub type ReplyFunc =
    Option<unsafe extern "C" fn(addr: *mut ReplyAddress, buf: *mut c_char, size: c_int)>;

/// A packet as it arrives from the host: the bytes, and where any reply goes.
///
/// `mReplyAddr` is a `ReplyAddress` by value rather than a pointer, and there
/// is a `bool` between it and `mSize`. Both were guessed wrongly once, and the
/// result was replies sent to an address made of the first eight bytes of a
/// struct — so this is under the layout oracle like everything else.
#[repr(C)]
pub struct OSC_Packet {
    pub mData: *mut c_char,
    pub mSize: c_int,
    pub mIsBundle: bool,
    pub mReplyAddr: ReplyAddress,
}

/// The argument reader handed to a bufgen.
///
/// Every accessor on the C++ side is inline in `sc_msg_iter.h`, so they read
/// whatever memory this points at and nothing links against a definition. That
/// is what lets Rust construct one: the layout is the entire contract.
///
/// `data` points at an OSC message from its type-tag string onwards — the
/// comma, the tags, then the padded argument data.
#[repr(C)]
#[derive(Debug)]
pub struct sc_msg_iter {
    pub data: *const c_char,
    pub rdpos: *const c_char,
    pub endpos: *const c_char,
    pub tags: *const c_char,
    pub size: c_int,
    pub count: c_int,
}

/// A command a plugin registers, reached by `/cmd`.
pub type PlugInCmdFunc = Option<
    unsafe extern "C" fn(world: *mut World, user_data: *mut c_void,
                         args: *mut sc_msg_iter, reply: *mut c_void),
>;

/// A command a ugen registers, reached by `/u_cmd`.
pub type UnitCmdFunc = Option<
    unsafe extern "C" fn(unit: *mut Unit, args: *mut sc_msg_iter),
>;

/// The same, for a command that also wants somewhere to reply to.
pub type UnitCmdFuncEx = Option<
    unsafe extern "C" fn(unit: *mut Unit, args: *mut sc_msg_iter, reply: *mut c_void),
>;

/// Fills a buffer on command: `/b_gen`'s `sine1`, `cheby`, `copy` and the rest.
///
/// Defined by the plugins rather than by the server, which is why these stay
/// where they are — they belong to the same body of code as the ugens.
pub type BufGenFunc =
    Option<unsafe extern "C" fn(world: *mut World, buf: *mut SndBuf, msg: *mut sc_msg_iter)>;

/// What a node's `mDef` points at.
///
/// The name is a packed byte buffer written through an `int32` array, which is
/// how the engine has always stored it; anything reading it treats the address
/// as a C string.
#[repr(C)]
pub struct NodeDef {
    pub mName: [i32; 64],
    pub mHash: i32,
    pub mAllocSize: usize,
}

impl NodeDef {
    /// A definition header carrying `name`, truncated to what the field holds.
    pub fn named(name: &str) -> NodeDef {
        let mut def = NodeDef { mName: [0; 64], mHash: 0, mAllocSize: 0 };
        let bytes = name.as_bytes();
        let room = 64 * 4 - 1;
        let n = bytes.len().min(room);
        unsafe {
            std::ptr::copy_nonoverlapping(
                bytes.as_ptr(),
                def.mName.as_mut_ptr() as *mut u8,
                n,
            );
        }
        def
    }
}

/// A node that contains other nodes: the two ends of a doubly-linked child
/// list, with each child's links living in its own [`Node`].
///
/// Containment is not decoration. The order this list is walked decides the
/// order buses accumulate, and float addition is not associative, so the tree
/// is part of the audio result rather than a way of organising it.
#[repr(C)]
#[derive(Debug)]
pub struct Group {
    pub mNode: Node,
    pub mHead: *mut Node,
    pub mTail: *mut Node,
}

/// The handle a ugen gets from `getScopeBuffer`.
///
/// `internalData` is first, and a ugen tests the handle by that field, so a
/// mirror with the fields the other way round hands back something that is
/// never valid. That is not hypothetical — see the layout oracle.
#[repr(C)]
pub struct ScopeBufferHnd {
    pub internalData: *mut c_void,
    pub data: *mut f32,
    pub channels: u32,
    pub maxFrames: u32,
}

/// How many times a buffer has been read and written.
///
/// A ugen that caches a buffer pointer compares these against what it saw
/// last time to notice the buffer changed underneath it.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SndBufUpdates {
    pub reads: c_int,
    pub writes: c_int,
}

/// A graph runs at a smaller block size than its world.
pub const K_GRAPH_REBLOCK: u32 = 0x1;
/// A graph runs at a higher sample rate than its world.
pub const K_GRAPH_RESAMPLE: u32 = 0x2;

/// A running synth: the wires, controls and units of one instantiated graph.
#[repr(C)]
#[derive(Debug)]
pub struct Graph {
    pub mNode: Node,
    pub mRefCount: i32,
    pub mNumTicks: u16,
    pub mTickCounter: u16,
    pub mFlags: u32,
    pub mFullRate: *mut Rate,
    pub mBufRate: *mut Rate,
    pub mNumWires: u32,
    pub mNumControls: u32,
    pub mWire: *mut Wire,
    pub mControls: *mut f32,
    pub mMapControls: *mut *mut f32,
    pub mAudioBusOffsets: *mut i32,
    pub mControlRates: *mut c_int,
    pub mNumUnits: u32,
    pub mNumCalcUnits: u32,
    pub mUnits: *mut *mut Unit,
    pub mCalcUnits: *mut *mut Unit,
    pub mSampleOffset: c_int,
    pub mSubsampleOffset: f32,
    /// The RNG stream this synth's ugens draw from — one of the World's.
    pub mRGen: *mut RGen,
    pub mLocalAudioBusUnit: *mut Unit,
    pub mLocalControlBusUnit: *mut Unit,
    pub mLocalSndBufs: *mut SndBuf,
    pub localBufNum: c_int,
    pub localMaxBufNum: c_int,
    /// Opaque tail pointer. Like `Unit::mExtensions`, its only significance to
    /// the Rust side is that it exists and occupies eight bytes.
    pub mPrivate: *mut c_void,
}

/// Three-component Tausworthe generator state. Seeding is what
/// `g_ss_deterministic_seed` pins for offline renders.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct RGen {
    pub s1: u32,
    pub s2: u32,
    pub s3: u32,
}

/// The engine state a ugen can see.
///
/// `hw` (HiddenWorld) is deliberately opaque — plugins are not given its
/// layout, and the Rust host has no reason to reproduce it.
#[repr(C)]
#[derive(Debug)]
pub struct World {
    pub hw: *mut c_void,
    pub ft: *mut c_void,
    pub mSampleRate: f64,
    pub mBufLength: i32,
    pub mBufCounter: i32,
    pub mNumAudioBusChannels: u32,
    pub mNumControlBusChannels: u32,
    pub mNumInputs: u32,
    pub mNumOutputs: u32,
    pub mAudioBus: *mut f32,
    pub mControlBus: *mut f32,
    pub mAudioBusTouched: *mut i32,
    pub mControlBusTouched: *mut i32,
    pub mNumSndBufs: u32,
    pub mSndBufs: *mut SndBuf,
    pub mSndBufsNonRealTimeMirror: *mut SndBuf,
    pub mSndBufUpdates: *mut c_void,
    pub mTopGroup: *mut c_void,
    pub mFullRate: Rate,
    pub mBufRate: Rate,
    pub mNumRGens: u32,
    pub mRGen: *mut RGen,
    pub mNumUnits: u32,
    pub mNumGraphs: u32,
    pub mNumGroups: u32,
    pub mSampleOffset: i32,
    pub mNRTLock: *mut c_void,
    pub mNumSharedControls: u32,
    pub mSharedControls: *mut f32,
    /// `SCBool` — one byte, not Rust's `bool` by luck: assert, don't assume.
    pub mRealTime: u8,
    pub mRunning: u8,
    pub mDumpOSC: i32,
    pub mDriverLock: *mut c_void,
    pub mSubsampleOffset: f32,
    pub mVerbosity: i32,
    pub mErrorNotification: i32,
    pub mLocalErrorNotification: i32,
    pub mRendezvous: u8,
    pub mRestrictedPath: *const c_char,
}

// ── InterfaceTable ───────────────────────────────────────────────────────────
//
// The engine's side of the plugin boundary: everything a ugen can call. In the
// target design Rust *provides* this table and the C++ ugens call into it,
// which is what makes the ugens leaves rather than participants.
//
// Only the entries a leaf ugen actually needs are typed here. The rest are
// declared as opaque pointers: they are all function pointers of the same
// width, so the layout is identical, and typing forty signatures nobody calls
// would add forty chances to get one subtly wrong. Each stays checked for
// offset and size by `tests/layout.rs`, so an untyped entry is still a
// verified entry — it is only its *signature* that is deferred, and calling
// one requires typing it first.

/// `fDefineUnit` — how a plugin registers a ugen. This is the entry that makes
/// Rust the owner of the registry: the ugens hand their ctor/dtor here, and
/// what Rust does with them is Rust's business.
pub type DefineUnitFn = Option<
    unsafe extern "C" fn(
        inUnitClassName: *const c_char,
        inAllocSize: usize,
        inCtor: UnitCtorFunc,
        inDtor: UnitDtorFunc,
        inFlags: u32,
    ) -> u8,
>;

pub type RanSeedFn = Option<unsafe extern "C" fn() -> i32>;
pub type ClearUnitOutputsFn = Option<unsafe extern "C" fn(*mut Unit, i32)>;
pub type RTAllocFn = Option<unsafe extern "C" fn(*mut World, usize) -> *mut c_void>;
pub type RTFreeFn = Option<unsafe extern "C" fn(*mut World, *mut c_void)>;
pub type DoneActionFn = Option<unsafe extern "C" fn(i32, *mut Unit)>;

#[repr(C)]
pub struct InterfaceTable {
    /// Size of the shared sine table, and the tables themselves. Oscillator
    /// ugens read these directly rather than through a call.
    pub mSineSize: u32,
    pub mSineWavetable: *mut f32,
    pub mSine: *mut f32,
    pub mCosecant: *mut f32,

    pub fPrint: *mut c_void,
    pub fRanSeed: RanSeedFn,

    pub fDefineUnit: DefineUnitFn,
    pub fDefinePlugInCmd: *mut c_void,
    pub fDefineUnitCmd: *mut c_void,
    pub fDefineUnitCmdEx: *mut c_void,
    pub fDefineBufGen: *mut c_void,

    pub fClearUnitOutputs: ClearUnitOutputsFn,

    pub fNRTAlloc: *mut c_void,
    pub fNRTRealloc: *mut c_void,
    pub fNRTFree: *mut c_void,

    pub fRTAlloc: RTAllocFn,
    pub fRTRealloc: *mut c_void,
    pub fRTFree: RTFreeFn,

    pub fNodeRun: *mut c_void,
    pub fNodeEnd: *mut c_void,
    pub fSendTrigger: *mut c_void,
    pub fSendNodeReply: *mut c_void,
    pub fSendMsgFromRT: *mut c_void,
    pub fSendMsgToRT: *mut c_void,
    pub fSndFileFormatInfoFromStrings: *mut c_void,
    pub fGetNode: *mut c_void,
    pub fGetGraph: *mut c_void,
    pub fNRTLock: *mut c_void,
    pub fNRTUnlock: *mut c_void,
    pub fGroup_DeleteAll: *mut c_void,

    /// Called by a ugen when its `doneAction` fires — the hook through which a
    /// finished envelope frees its synth.
    pub fDoneAction: DoneActionFn,

    pub fDoAsynchronousCommand: *mut c_void,
    pub fDoAsynchronousCommandEx: *mut c_void,
    pub fDoAsyncUnitCommand: *mut c_void,
    pub fBufAlloc: *mut c_void,

    pub fSCfftCreate: *mut c_void,
    pub fSCfftDoFFT: *mut c_void,
    pub fSCfftDoIFFT: *mut c_void,
    pub fSCfftDestroy: *mut c_void,

    pub fGetScopeBuffer: *mut c_void,
    pub fPushScopeBuffer: *mut c_void,
    pub fReleaseScopeBuffer: *mut c_void,
}
