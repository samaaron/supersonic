//! Assert every struct and field against the compiler's own numbers.
//!
//! `supersonic_layout_dump` prints `sizeof`/`alignof`/`offsetof` for the
//! plugin-interface structs the ugens are compiled against. This test recreates
//! the same numbers from the Rust definitions and requires them to match
//! exactly. Any field the oracle reports and this test does not know about is
//! also a failure — a silently unchecked field is the one that would move.
//!
//!     cmake --build build/fs --target supersonic_layout_dump
//!     cd rust && cargo test -p supersonic-ugen-abi

use std::collections::BTreeMap;
use std::mem::{align_of, offset_of, size_of};
use std::path::{Path, PathBuf};
use std::process::Command;

use supersonic_ugen_abi::*;

fn oracle_binary() -> Option<PathBuf> {
    if let Ok(p) = std::env::var("NRT_LAYOUT_DUMP") {
        let p = PathBuf::from(p);
        return p.exists().then_some(p);
    }
    let p = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../build/fs/supersonic_layout_dump");
    p.exists().then_some(p)
}

/// `struct` line -> (size, align); `field` line -> (offset, size).
fn parse(text: &str) -> (BTreeMap<String, (usize, usize)>, BTreeMap<String, (usize, usize)>) {
    let mut structs = BTreeMap::new();
    let mut fields = BTreeMap::new();
    let num = |t: &str, key: &str| -> Option<usize> {
        t.split_whitespace().find_map(|x| x.strip_prefix(key)).and_then(|v| v.parse().ok())
    };
    for line in text.lines() {
        let mut parts = line.split_whitespace();
        match (parts.next(), parts.next()) {
            (Some("struct"), Some(name)) => {
                structs.insert(
                    name.to_string(),
                    (num(line, "size=").unwrap(), num(line, "align=").unwrap()),
                );
            }
            (Some("field"), Some(name)) => {
                fields.insert(
                    name.to_string(),
                    (num(line, "offset=").unwrap(), num(line, "size=").unwrap()),
                );
            }
            _ => {}
        }
    }
    (structs, fields)
}

macro_rules! check_struct {
    ($structs:expr, $seen:expr, $T:ty) => {{
        let name = stringify!($T);
        let (size, align) = *$structs
            .get(name)
            .unwrap_or_else(|| panic!("oracle has no struct {name}"));
        assert_eq!(size_of::<$T>(), size, "{name}: size");
        assert_eq!(align_of::<$T>(), align, "{name}: alignment");
        $seen.push(name.to_string());
    }};
}

macro_rules! check_field {
    ($fields:expr, $seen:expr, $T:ty, $f:ident) => {{
        let key = format!("{}.{}", stringify!($T), stringify!($f));
        let (offset, size) = *$fields
            .get(&key)
            .unwrap_or_else(|| panic!("oracle has no field {key}"));
        assert_eq!(offset_of!($T, $f), offset, "{key}: offset");
        // Comparing the field's own width too catches a same-offset,
        // wrong-width field — an i32 where the C++ has an i64, say, which the
        // offsets alone would not reveal until the next field moved.
        assert_eq!(field_size!($T, $f), size, "{key}: size");
        $seen.push(key);
    }};
}

/// Width of one field, without constructing a value of the struct.
macro_rules! field_size {
    ($T:ty, $f:ident) => {{
        fn width<T>(_: *const T) -> usize {
            std::mem::size_of::<T>()
        }
        let uninit = std::mem::MaybeUninit::<$T>::uninit();
        // Raw pointer to a field of a live (if uninitialised) allocation: no
        // read happens, so nothing here touches uninitialised memory.
        let p = unsafe { std::ptr::addr_of!((*uninit.as_ptr()).$f) };
        width(p)
    }};
}

#[test]
fn layout_matches_the_compiler() {
    let Some(bin) = oracle_binary() else {
        eprintln!("SKIP: build/fs/supersonic_layout_dump not built — layout check skipped");
        return;
    };
    let out = Command::new(&bin).output().expect("run layout_dump");
    let (structs, fields) = parse(&String::from_utf8_lossy(&out.stdout));
    assert!(!structs.is_empty(), "oracle produced no output");

    let mut seen_structs: Vec<String> = Vec::new();
    let mut seen_fields: Vec<String> = Vec::new();

    check_struct!(structs, seen_structs, Wire);
    check_field!(fields, seen_fields, Wire, mFromUnit);
    check_field!(fields, seen_fields, Wire, mCalcRate);
    check_field!(fields, seen_fields, Wire, mBuffer);
    check_field!(fields, seen_fields, Wire, mScalarValue);

    check_struct!(structs, seen_structs, Rate);
    check_field!(fields, seen_fields, Rate, mSampleRate);
    check_field!(fields, seen_fields, Rate, mSampleDur);
    check_field!(fields, seen_fields, Rate, mBufDuration);
    check_field!(fields, seen_fields, Rate, mBufRate);
    check_field!(fields, seen_fields, Rate, mSlopeFactor);
    check_field!(fields, seen_fields, Rate, mRadiansPerSample);
    check_field!(fields, seen_fields, Rate, mBufLength);
    check_field!(fields, seen_fields, Rate, mFilterLoops);
    check_field!(fields, seen_fields, Rate, mFilterRemain);
    check_field!(fields, seen_fields, Rate, mFilterSlope);

    check_struct!(structs, seen_structs, SndBuf);
    check_field!(fields, seen_fields, SndBuf, samplerate);
    check_field!(fields, seen_fields, SndBuf, sampledur);
    check_field!(fields, seen_fields, SndBuf, data);
    check_field!(fields, seen_fields, SndBuf, channels);
    check_field!(fields, seen_fields, SndBuf, samples);
    check_field!(fields, seen_fields, SndBuf, frames);
    check_field!(fields, seen_fields, SndBuf, mask);
    check_field!(fields, seen_fields, SndBuf, mask1);
    check_field!(fields, seen_fields, SndBuf, coord);
    check_field!(fields, seen_fields, SndBuf, sndfile);

    check_struct!(structs, seen_structs, Unit);
    check_field!(fields, seen_fields, Unit, mWorld);
    check_field!(fields, seen_fields, Unit, mUnitDef);
    check_field!(fields, seen_fields, Unit, mParent);
    check_field!(fields, seen_fields, Unit, mNumInputs);
    check_field!(fields, seen_fields, Unit, mNumOutputs);
    check_field!(fields, seen_fields, Unit, mCalcRate);
    check_field!(fields, seen_fields, Unit, mSpecialIndex);
    check_field!(fields, seen_fields, Unit, mParentIndex);
    check_field!(fields, seen_fields, Unit, mDone);
    check_field!(fields, seen_fields, Unit, mInput);
    check_field!(fields, seen_fields, Unit, mOutput);
    check_field!(fields, seen_fields, Unit, mRate);
    check_field!(fields, seen_fields, Unit, mExtensions);
    check_field!(fields, seen_fields, Unit, mInBuf);
    check_field!(fields, seen_fields, Unit, mOutBuf);
    check_field!(fields, seen_fields, Unit, mCalcFunc);
    check_field!(fields, seen_fields, Unit, mBufLength);

    check_struct!(structs, seen_structs, Node);
    check_field!(fields, seen_fields, Node, mID);
    check_field!(fields, seen_fields, Node, mHash);
    check_field!(fields, seen_fields, Node, mWorld);
    check_field!(fields, seen_fields, Node, mDef);
    check_field!(fields, seen_fields, Node, mCalcFunc);
    check_field!(fields, seen_fields, Node, mPrev);
    check_field!(fields, seen_fields, Node, mNext);
    check_field!(fields, seen_fields, Node, mParent);
    check_field!(fields, seen_fields, Node, mIsGroup);

    check_struct!(structs, seen_structs, WorldOptions);
    for f in [
        "mNumBuffers", "mMaxLogins", "mMaxNodes", "mMaxGraphDefs", "mMaxWireBufs",
        "mNumAudioBusChannels", "mNumInputBusChannels", "mNumOutputBusChannels",
        "mNumControlBusChannels", "mBufLength", "mRealTimeMemorySize", "mRealTime",
        "mPreferredSampleRate", "mNumRGens",
    ] {
        let key = format!("WorldOptions.{f}");
        let want = *fields.get(&key).unwrap_or_else(|| panic!("oracle has no field {key}"));
        assert_eq!(world_options_offset(f), want.0, "{key}: offset");
        seen_fields.push(format!("WorldOptions.{f}"));
    }

    check_struct!(structs, seen_structs, OSC_Packet);
    check_field!(fields, seen_fields, OSC_Packet, mData);
    check_field!(fields, seen_fields, OSC_Packet, mSize);
    check_field!(fields, seen_fields, OSC_Packet, mIsBundle);
    check_field!(fields, seen_fields, OSC_Packet, mReplyAddr);

    check_struct!(structs, seen_structs, ReplyAddress);
    check_field!(fields, seen_fields, ReplyAddress, mProtocol);
    check_field!(fields, seen_fields, ReplyAddress, mPort);
    check_field!(fields, seen_fields, ReplyAddress, mSocket);
    check_field!(fields, seen_fields, ReplyAddress, mReplyFunc);
    check_field!(fields, seen_fields, ReplyAddress, mReplyData);

    check_struct!(structs, seen_structs, sc_msg_iter);
    check_field!(fields, seen_fields, sc_msg_iter, data);
    check_field!(fields, seen_fields, sc_msg_iter, rdpos);
    check_field!(fields, seen_fields, sc_msg_iter, endpos);
    check_field!(fields, seen_fields, sc_msg_iter, tags);
    check_field!(fields, seen_fields, sc_msg_iter, size);
    check_field!(fields, seen_fields, sc_msg_iter, count);

    check_struct!(structs, seen_structs, Group);
    check_field!(fields, seen_fields, Group, mNode);
    check_field!(fields, seen_fields, Group, mHead);
    check_field!(fields, seen_fields, Group, mTail);

    check_struct!(structs, seen_structs, ScopeBufferHnd);
    check_field!(fields, seen_fields, ScopeBufferHnd, internalData);
    check_field!(fields, seen_fields, ScopeBufferHnd, data);
    check_field!(fields, seen_fields, ScopeBufferHnd, channels);
    check_field!(fields, seen_fields, ScopeBufferHnd, maxFrames);

    check_struct!(structs, seen_structs, Graph);
    check_field!(fields, seen_fields, Graph, mNode);
    check_field!(fields, seen_fields, Graph, mRefCount);
    check_field!(fields, seen_fields, Graph, mNumTicks);
    check_field!(fields, seen_fields, Graph, mTickCounter);
    check_field!(fields, seen_fields, Graph, mFlags);
    check_field!(fields, seen_fields, Graph, mFullRate);
    check_field!(fields, seen_fields, Graph, mBufRate);
    check_field!(fields, seen_fields, Graph, mNumWires);
    check_field!(fields, seen_fields, Graph, mNumControls);
    check_field!(fields, seen_fields, Graph, mWire);
    check_field!(fields, seen_fields, Graph, mControls);
    check_field!(fields, seen_fields, Graph, mMapControls);
    check_field!(fields, seen_fields, Graph, mAudioBusOffsets);
    check_field!(fields, seen_fields, Graph, mControlRates);
    check_field!(fields, seen_fields, Graph, mNumUnits);
    check_field!(fields, seen_fields, Graph, mNumCalcUnits);
    check_field!(fields, seen_fields, Graph, mUnits);
    check_field!(fields, seen_fields, Graph, mCalcUnits);
    check_field!(fields, seen_fields, Graph, mSampleOffset);
    check_field!(fields, seen_fields, Graph, mSubsampleOffset);
    check_field!(fields, seen_fields, Graph, mRGen);
    check_field!(fields, seen_fields, Graph, mLocalAudioBusUnit);
    check_field!(fields, seen_fields, Graph, mLocalControlBusUnit);
    check_field!(fields, seen_fields, Graph, mLocalSndBufs);
    check_field!(fields, seen_fields, Graph, localBufNum);
    check_field!(fields, seen_fields, Graph, localMaxBufNum);
    check_field!(fields, seen_fields, Graph, mPrivate);

    check_struct!(structs, seen_structs, RGen);
    check_field!(fields, seen_fields, RGen, s1);
    check_field!(fields, seen_fields, RGen, s2);
    check_field!(fields, seen_fields, RGen, s3);

    check_struct!(structs, seen_structs, World);
    check_field!(fields, seen_fields, World, hw);
    check_field!(fields, seen_fields, World, ft);
    check_field!(fields, seen_fields, World, mSampleRate);
    check_field!(fields, seen_fields, World, mBufLength);
    check_field!(fields, seen_fields, World, mBufCounter);
    check_field!(fields, seen_fields, World, mNumAudioBusChannels);
    check_field!(fields, seen_fields, World, mNumControlBusChannels);
    check_field!(fields, seen_fields, World, mNumInputs);
    check_field!(fields, seen_fields, World, mNumOutputs);
    check_field!(fields, seen_fields, World, mAudioBus);
    check_field!(fields, seen_fields, World, mControlBus);
    check_field!(fields, seen_fields, World, mAudioBusTouched);
    check_field!(fields, seen_fields, World, mControlBusTouched);
    check_field!(fields, seen_fields, World, mNumSndBufs);
    check_field!(fields, seen_fields, World, mSndBufs);
    check_field!(fields, seen_fields, World, mSndBufsNonRealTimeMirror);
    check_field!(fields, seen_fields, World, mSndBufUpdates);
    check_field!(fields, seen_fields, World, mTopGroup);
    check_field!(fields, seen_fields, World, mFullRate);
    check_field!(fields, seen_fields, World, mBufRate);
    check_field!(fields, seen_fields, World, mNumRGens);
    check_field!(fields, seen_fields, World, mRGen);
    check_field!(fields, seen_fields, World, mNumUnits);
    check_field!(fields, seen_fields, World, mNumGraphs);
    check_field!(fields, seen_fields, World, mNumGroups);
    check_field!(fields, seen_fields, World, mSampleOffset);
    check_field!(fields, seen_fields, World, mNRTLock);
    check_field!(fields, seen_fields, World, mNumSharedControls);
    check_field!(fields, seen_fields, World, mSharedControls);
    check_field!(fields, seen_fields, World, mRealTime);
    check_field!(fields, seen_fields, World, mRunning);
    check_field!(fields, seen_fields, World, mDumpOSC);
    check_field!(fields, seen_fields, World, mDriverLock);
    check_field!(fields, seen_fields, World, mSubsampleOffset);
    check_field!(fields, seen_fields, World, mVerbosity);
    check_field!(fields, seen_fields, World, mErrorNotification);
    check_field!(fields, seen_fields, World, mLocalErrorNotification);
    check_field!(fields, seen_fields, World, mRendezvous);
    check_field!(fields, seen_fields, World, mRestrictedPath);

    check_struct!(structs, seen_structs, InterfaceTable);
    check_field!(fields, seen_fields, InterfaceTable, mSineSize);
    check_field!(fields, seen_fields, InterfaceTable, mSineWavetable);
    check_field!(fields, seen_fields, InterfaceTable, mSine);
    check_field!(fields, seen_fields, InterfaceTable, mCosecant);
    check_field!(fields, seen_fields, InterfaceTable, fPrint);
    check_field!(fields, seen_fields, InterfaceTable, fRanSeed);
    check_field!(fields, seen_fields, InterfaceTable, fDefineUnit);
    check_field!(fields, seen_fields, InterfaceTable, fDefinePlugInCmd);
    check_field!(fields, seen_fields, InterfaceTable, fDefineUnitCmd);
    check_field!(fields, seen_fields, InterfaceTable, fDefineUnitCmdEx);
    check_field!(fields, seen_fields, InterfaceTable, fDefineBufGen);
    check_field!(fields, seen_fields, InterfaceTable, fClearUnitOutputs);
    check_field!(fields, seen_fields, InterfaceTable, fNRTAlloc);
    check_field!(fields, seen_fields, InterfaceTable, fNRTRealloc);
    check_field!(fields, seen_fields, InterfaceTable, fNRTFree);
    check_field!(fields, seen_fields, InterfaceTable, fRTAlloc);
    check_field!(fields, seen_fields, InterfaceTable, fRTRealloc);
    check_field!(fields, seen_fields, InterfaceTable, fRTFree);
    check_field!(fields, seen_fields, InterfaceTable, fNodeRun);
    check_field!(fields, seen_fields, InterfaceTable, fNodeEnd);
    check_field!(fields, seen_fields, InterfaceTable, fSendTrigger);
    check_field!(fields, seen_fields, InterfaceTable, fSendNodeReply);
    check_field!(fields, seen_fields, InterfaceTable, fSendMsgFromRT);
    check_field!(fields, seen_fields, InterfaceTable, fSendMsgToRT);
    check_field!(fields, seen_fields, InterfaceTable, fSndFileFormatInfoFromStrings);
    check_field!(fields, seen_fields, InterfaceTable, fGetNode);
    check_field!(fields, seen_fields, InterfaceTable, fGetGraph);
    check_field!(fields, seen_fields, InterfaceTable, fNRTLock);
    check_field!(fields, seen_fields, InterfaceTable, fNRTUnlock);
    check_field!(fields, seen_fields, InterfaceTable, fGroup_DeleteAll);
    check_field!(fields, seen_fields, InterfaceTable, fDoneAction);
    check_field!(fields, seen_fields, InterfaceTable, fDoAsynchronousCommand);
    check_field!(fields, seen_fields, InterfaceTable, fDoAsynchronousCommandEx);
    check_field!(fields, seen_fields, InterfaceTable, fDoAsyncUnitCommand);
    check_field!(fields, seen_fields, InterfaceTable, fBufAlloc);
    check_field!(fields, seen_fields, InterfaceTable, fSCfftCreate);
    check_field!(fields, seen_fields, InterfaceTable, fSCfftDoFFT);
    check_field!(fields, seen_fields, InterfaceTable, fSCfftDoIFFT);
    check_field!(fields, seen_fields, InterfaceTable, fSCfftDestroy);
    check_field!(fields, seen_fields, InterfaceTable, fGetScopeBuffer);
    check_field!(fields, seen_fields, InterfaceTable, fPushScopeBuffer);
    check_field!(fields, seen_fields, InterfaceTable, fReleaseScopeBuffer);

    // Nothing the oracle reports may go unchecked: an unchecked field is
    // exactly the one that moves without anyone noticing.
    let unchecked_structs: Vec<&String> =
        structs.keys().filter(|k| !seen_structs.contains(k)).collect();
    let unchecked_fields: Vec<&String> =
        fields.keys().filter(|k| !seen_fields.contains(k)).collect();
    assert!(unchecked_structs.is_empty(), "structs not checked: {unchecked_structs:?}");
    assert!(unchecked_fields.is_empty(), "fields not checked: {unchecked_fields:?}");

    eprintln!(
        "verified {} struct(s) and {} field(s) against the compiler",
        seen_structs.len(),
        seen_fields.len()
    );
}

/// Offsets of the WorldOptions fields this engine reads. Written out rather
/// than reached by macro because the struct has padding the macro would have
/// to name, and naming padding to check real fields helps nobody.
fn world_options_offset(field: &str) -> usize {
    let o = std::mem::MaybeUninit::<WorldOptions>::uninit();
    let base = o.as_ptr() as usize;
    unsafe {
        let p = o.as_ptr();
        match field {
            "mNumBuffers" => std::ptr::addr_of!((*p).mNumBuffers) as usize - base,
            "mMaxLogins" => std::ptr::addr_of!((*p).mMaxLogins) as usize - base,
            "mMaxNodes" => std::ptr::addr_of!((*p).mMaxNodes) as usize - base,
            "mMaxGraphDefs" => std::ptr::addr_of!((*p).mMaxGraphDefs) as usize - base,
            "mMaxWireBufs" => std::ptr::addr_of!((*p).mMaxWireBufs) as usize - base,
            "mNumAudioBusChannels" => std::ptr::addr_of!((*p).mNumAudioBusChannels) as usize - base,
            "mNumInputBusChannels" => std::ptr::addr_of!((*p).mNumInputBusChannels) as usize - base,
            "mNumOutputBusChannels" => std::ptr::addr_of!((*p).mNumOutputBusChannels) as usize - base,
            "mNumControlBusChannels" => std::ptr::addr_of!((*p).mNumControlBusChannels) as usize - base,
            "mBufLength" => std::ptr::addr_of!((*p).mBufLength) as usize - base,
            "mRealTimeMemorySize" => std::ptr::addr_of!((*p).mRealTimeMemorySize) as usize - base,
            "mRealTime" => std::ptr::addr_of!((*p).mRealTime) as usize - base,
            "mPreferredSampleRate" => std::ptr::addr_of!((*p).mPreferredSampleRate) as usize - base,
            "mNumRGens" => std::ptr::addr_of!((*p).mNumRGens) as usize - base,
            other => panic!("unknown field {other}"),
        }
    }
}
