//! Print this crate's struct layout in the same format the C++ oracle uses.
//!
//! Built for whichever target is being checked — the point is the ones that
//! are not x86-64, where pointer width differs and an assumption made on the
//! host silently moves every field.
use std::mem::{align_of, size_of};
use supersonic_ugen_abi::*;

macro_rules! s {
    ($T:ty) => {
        println!("struct {} size={} align={}", stringify!($T), size_of::<$T>(), align_of::<$T>());
    };
}
macro_rules! f {
    ($T:ty, $field:ident) => {{
        let u = std::mem::MaybeUninit::<$T>::uninit();
        let base = u.as_ptr() as usize;
        let off = unsafe { std::ptr::addr_of!((*u.as_ptr()).$field) as usize } - base;
        println!("field {}.{} offset={}", stringify!($T), stringify!($field), off);
    }};
}

fn main() {
    s!(Wire);
    f!(Wire, mFromUnit);
    f!(Wire, mCalcRate);
    f!(Wire, mScalarValue);
    f!(Wire, mBuffer);

    s!(Unit);
    f!(Unit, mWorld);
    f!(Unit, mUnitDef);
    f!(Unit, mParent);
    f!(Unit, mNumInputs);
    f!(Unit, mNumOutputs);
    f!(Unit, mCalcRate);
    f!(Unit, mSpecialIndex);
    f!(Unit, mParentIndex);
    f!(Unit, mDone);
    f!(Unit, mInput);
    f!(Unit, mOutput);
    f!(Unit, mInBuf);
    f!(Unit, mOutBuf);
    f!(Unit, mCalcFunc);
    f!(Unit, mBufLength);

    s!(Graph);
    f!(Graph, mNode);
    f!(Graph, mWire);
    f!(Graph, mUnits);
    f!(Graph, mCalcUnits);

    s!(World);
    f!(World, mAudioBus);
    f!(World, mNumOutputs);
    f!(World, mBufLength);
    f!(World, mSndBufs);

    s!(SndBuf);
    f!(SndBuf, data);
    f!(SndBuf, samples);
}
