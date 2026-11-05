/*
    SuperCollider real time audio synthesis system
    Copyright (c) 2002 James McCartney. All rights reserved.
    http://www.audiosynth.com

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

#ifdef _WIN32
#    include "SC_Win32Utils.h"
#endif

#ifdef __APPLE__
#    include "../../common/SC_Apple.hpp"
#endif


#include "SC_World.h"
#include "SC_WorldOptions.h"
#include "SC_HiddenWorld.h"
#include "SC_InterfaceTable.h"
#include "SC_AllocPool.h"
#include "SC_GraphDef.h"
#include "SC_UnitDef.h"
#include "SC_BufGen.h"
#include "SC_Node.h"
#include "SC_CoreAudio.h"
#include "SC_Group.h"
#include "SC_Errors.h"
#include <stdio.h>
#include "SC_Prototypes.h"
#include "SC_Lock.h"
#include "SC_Lib_Cintf.h"
#include "../../common/SC_SndFileHelpers.hpp"
#include "../../common/Samp.hpp"
#include "../../common/SC_fftlib.hpp"
#include "guest_heap.h"

#ifdef CLOCKWORK_GUEST
// Called explicitly in World_New to guarantee tables are populated
// regardless of static constructor ordering.
extern "C" {
}
#endif

#ifdef _WIN32
#    include <direct.h>
#else
#    include <sys/param.h>
#    include <unistd.h> // for _POSIX_MEMLOCK
#endif

#include "malloc_aligned.hpp"

#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#    include <xmmintrin.h>
#elif defined(_M_ARM64) && defined(_MSC_VER)
#    include <intrin.h>
#endif

// undefine the shadowed scfft functions
#undef scfft_create
#undef scfft_dofft
#undef scfft_doifft
#undef scfft_destroy

#if (_POSIX_MEMLOCK - 0) >= 200112L
#    include <sys/resource.h>
#    include <sys/mman.h>
#endif

#ifndef CLOCKWORK_GUEST
InterfaceTable gInterfaceTable;
PrintFunc gPrint = nullptr;

extern HashTable<struct UnitDef, Malloc>* gUnitDefLib;
extern HashTable<struct BufGen, Malloc>* gBufGenLib;
extern HashTable<PlugInCmd, Malloc>* gPlugInCmds;
#endif

extern "C" {

#ifdef NO_LIBSNDFILE
typedef struct SF_INFO SF_INFO;
#endif

SCBool SendMsgToEngine(World* inWorld, FifoMsg* inMsg);
SCBool SendMsgFromEngine(World* inWorld, FifoMsg* inMsg);

}

////////////////////////////////////////////////////////////////////////////////

#ifdef CLOCKWORK_GUEST
// SuperSonic (plural guest): the heap of the World's instance, through the guest's doors (guest_doors.h:
// DspHost::alloc_bytes / free_bytes, clockwork's allocator over the span):
// lock-free, so a sequenced command's Stage2 may take from it on the audio
// thread, and the same on every target. sc_free hands nothing back that the
// client owns — a buffer bound to the assets area is the client's range, and the
// door knows so.
inline void* sc_malloc(World* inWorld, size_t size) { return supersonic_guest_alloc_bytes(inWorld, size); }
inline void sc_free(World* inWorld, void* ptr) { supersonic_guest_free_bytes(inWorld, ptr); }

// Backing areas for scsynth's real-time AllocPool (world->hw->mAllocPool, the
// per-block-hot graph state: Nodes, Graphs, per-synth wire buffers, Units).
// SuperSonic (plural guest): the pool asks with its World as the context. The first area is pre-sized at
// World_New (hw->mRtPoolFirstArea): the guest's fast memory (DspConfig::memory)
// when it holds the pool, so the hot state lives in the memory the host placed
// for exactly that, else the instance's heap. Growth areas
// (SCSYNTH_RT_POOL_GROWTH_SIZE; 0 on desktop, web and the NIF) come from the
// heap. A fast-memory area (hw->mRtPoolArenaArea) is the host's and is never
// freed.
static void* rt_pool_area_alloc(void* ctx, size_t size) {
    World* world = static_cast<World*>(ctx);
    if (void* first = world->hw->mRtPoolFirstArea) {
        world->hw->mRtPoolFirstArea = nullptr;
        return first;
    }
    return supersonic_guest_alloc_bytes(world, size);
}
static void rt_pool_area_free(void* ctx, void* ptr) {
    World* world = static_cast<World*>(ctx);
    if (ptr && ptr == world->hw->mRtPoolArenaArea) {
        world->hw->mRtPoolArenaArea = nullptr;
        return;
    }
    supersonic_guest_free_bytes(world, ptr);
}
#else
inline void* sc_malloc(World*, size_t size) { return nova::malloc_aligned(size); }
inline void sc_free(World*, void* ptr) { nova::free_aligned(ptr); }
#endif

void* sc_dbg_malloc(World* inWorld, size_t size, const char* tag, int line) {
    void* ptr = sc_malloc(inWorld, size);
    fprintf(stderr, "sc_dbg_malloc [%s:%d] %p %zu\n", tag, line, ptr, size);
#if SC_MEMORY_ALIGNMENT > 1
    if (((intptr_t)ptr % SC_MEMORY_ALIGNMENT) != 0) {
        fprintf(stderr, "sc_dbg_malloc [%s:%d] %p %zu: memory alignment error\n", tag, line, ptr, size);
        abort();
    }
#endif
    return ptr;
}

void sc_dbg_free(World* inWorld, void* ptr, const char* tag, int line) {
    fprintf(stderr, "sc_dbg_free [%s:%d]: %p\n", tag, line, ptr);
    sc_free(inWorld, ptr);
}

inline void* sc_zalloc(World* inWorld, size_t n, size_t size) {
    size *= n;
    if (size) {
        void* ptr = sc_malloc(inWorld, size);
        if (ptr) {
            memset(ptr, 0, size);
            return ptr;
        }
    }
    return nullptr;
}

void* sc_dbg_zalloc(World* inWorld, size_t n, size_t size, const char* tag, int line) {
    void* ptr = sc_zalloc(inWorld, n, size);
    fprintf(stderr, "sc_dbg_zalloc [%s:%d]: %p %zu %zu\n", tag, line, ptr, n, size);
    return ptr;
}

#if SC_DEBUG_MEMORY
#    define malloc_alig(world, size) sc_dbg_malloc((world), (size), __FUNCTION__, __LINE__)
#    define free_alig(world, ptr) sc_dbg_free((world), (ptr), __FUNCTION__, __LINE__)
#    define zalloc_(world, n, size) sc_dbg_zalloc((world), (n), (size), __FUNCTION__, __LINE__)
#else
#    define malloc_alig(world, size) sc_malloc((world), (size))
#    define free_alig(world, ptr) sc_free((world), (ptr))
#    define zalloc_(world, n, size) sc_zalloc((world), (n), (size))
#endif // SC_DEBUG_MEMORY

void* zalloc(World* inWorld, size_t n, size_t size) { return zalloc_(inWorld, n, size); }

void zfree(World* inWorld, void* ptr) { return free_alig(inWorld, ptr); }


////////////////////////////////////////////////////////////////////////////////

// Set denormal FTZ mode on CPUs that need/support it.
// Without this, denormal floats in audio DSP (common as synths decay to
// silence) incur ~100x slowdown on many cores, causing audio glitches.
void sc_SetDenormalFlags() {
#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _mm_setcsr(_mm_getcsr() | 0x40); // DAZ
#elif defined(_M_ARM64) && defined(_MSC_VER)
    // AArch64 Windows: set FZ (Flush-to-Zero) bit 24 in FPCR.
    // 0x5A20 is the AArch64 system register encoding for FPCR
    // (op0=3, op1=3, CRn=4, CRm=4, op2=0) per ARM Architecture Reference Manual.
    // Verified empirically: denormal * 0.5 yields 0.0 after this call.
    unsigned __int64 fpcr = _ReadStatusReg(0x5A20);
    fpcr |= (1ULL << 24);  // FZ bit
    _WriteStatusReg(0x5A20, fpcr);
#elif defined(__aarch64__)
    // AArch64 GCC/Clang: set FZ bit in FPCR via inline asm
    uint64_t fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ULL << 24);  // FZ bit
    __asm__ __volatile__("msr fpcr, %0" :: "r"(fpcr));
#elif defined(__VFP_FP__)
    // 32-bit ARM Cortex A8 with VFP: enable flush-to-zero on VFP unit.
    // NEON already flushes denormals, so no NEON-specific settings needed.
    /* This code is from math-neon/math_runfast.c
    Copyright (c) 2015 Lachlan Tychsen-Smith (lachlan.ts@gmail.com)
    MIT License
     */
    static const unsigned int x = 0x04086060;
    static const unsigned int y = 0x03000000;
    int r;
    asm volatile("fmrx	%0, fpscr			\n\t" // r0 = FPSCR
                 "and	%0, %0, %1			\n\t" // r0 = r0 & 0x04086060
                 "orr	%0, %0, %2			\n\t" // r0 = r0 | 0x03000000
                 "fmxr	fpscr, %0			\n\t" // FPSCR = r0
                 : "=r"(r)
                 : "r"(x), "r"(y));
#endif
}

////////////////////////////////////////////////////////////////////////////////

// Scope publishing goes through the guest's doors to the host (scsynth_scope.h).
#include "scsynth_scope.h"
#include "scsynth_options.h"

// C API version that takes the SF_INFO by pointer
SCErr sndfileFormatInfoFromStrings(SF_INFO* info, const char* headerFormat, const char* sampleFormat) {
    assert(info != nullptr);
#ifdef NO_LIBSNDFILE
    return kSCErr_Failed;
#else
    return sndfileFormatInfoFromStrings(*info, headerFormat, sampleFormat);
#endif
}

#ifdef CLOCKWORK_GUEST
// SuperSonic (plural guest): the table is a constant, the same entries
// upstream's InterfaceTable_Init (below) assigns at load; a World takes a
// copy and names itself in it (World_New). An entry upstream adds goes here
// too, in declaration order.
const InterfaceTable gInterfaceTable = {
    .mSineSize = kSineSize,
    .mSineWavetable = gSineWavetable,
    .mSine = gSine,
    .mCosecant = gInvSine,
    .fPrint = &scprintf,
    .fRanSeed = &server_timeseed,
    .fDefineUnit = &UnitDef_Create,
    .fDefinePlugInCmd = &PlugIn_DefineCmd,
    .fDefineUnitCmd = &UnitDef_AddCmd,
    .fDefineUnitCmdEx = &UnitDef_AddCmdEx,
    .fDefineBufGen = &BufGen_Create,
    .fClearUnitOutputs = &Unit_ZeroOutputs,
    .fNRTAlloc = &malloc,
    .fNRTRealloc = &realloc,
    .fNRTFree = &free,
    .fRTAlloc = &World_Alloc,
    .fRTRealloc = &World_Realloc,
    .fRTFree = &World_Free,
    .fNodeRun = &Node_SetRun,
    .fNodeEnd = &Node_End,
    .fSendTrigger = &Node_SendTrigger,
    .fSendNodeReply = &Node_SendReply,
    .fSendMsgFromRT = &SendMsgFromEngine,
    .fSendMsgToRT = &SendMsgToEngine,
    .fSndFileFormatInfoFromStrings = &sndfileFormatInfoFromStrings,
    .fGetNode = &World_GetNode,
    .fGetGraph = &World_GetGraph,
    .fNRTLock = &World_NRTLock,
    .fNRTUnlock = &World_NRTUnlock,
    .fGroup_DeleteAll = &Group_DeleteAll,
    .fDoneAction = &Unit_DoneAction,
    .fDoAsynchronousCommand = &PerformAsynchronousCommand,
    .fDoAsynchronousCommandEx = &PerformAsynchronousCommandEx,
    .fDoAsyncUnitCommand = &PerformAsyncUnitCommand,
    .fBufAlloc = &bufAlloc,
    .fSCfftCreate = &scfft_create,
    .fSCfftDoFFT = &scfft_dofft,
    .fSCfftDoIFFT = &scfft_doifft,
    .fSCfftDestroy = &scfft_destroy,
    .fGetScopeBuffer = &supersonic_scope_get,
    .fPushScopeBuffer = &supersonic_scope_push,
    .fReleaseScopeBuffer = &supersonic_scope_release,
    .mWorld = nullptr,
};
#else
void InterfaceTable_Init() {
    InterfaceTable* ft = &gInterfaceTable;
    ft->mSine = gSine;
    ft->mCosecant = gInvSine;
    ft->mSineSize = kSineSize;
    ft->mSineWavetable = gSineWavetable;

    ft->fPrint = &scprintf;

    ft->fRanSeed = &server_timeseed;

    ft->fNodeEnd = &Node_End;

    ft->fDefineUnit = &UnitDef_Create;
    ft->fDefineBufGen = &BufGen_Create;
    ft->fClearUnitOutputs = &Unit_ZeroOutputs;

    ft->fNRTAlloc = &malloc;
    ft->fNRTRealloc = &realloc;
    ft->fNRTFree = &free;

    ft->fRTAlloc = &World_Alloc;
    ft->fRTRealloc = &World_Realloc;
    ft->fRTFree = &World_Free;

    ft->fNodeRun = &Node_SetRun;

    ft->fSendTrigger = &Node_SendTrigger;
    ft->fSendNodeReply = &Node_SendReply;


    ft->fDefineUnitCmd = &UnitDef_AddCmd;
    ft->fDefineUnitCmdEx = &UnitDef_AddCmdEx;
    ft->fDefinePlugInCmd = &PlugIn_DefineCmd;

    ft->fSendMsgFromRT = &SendMsgFromEngine;
    ft->fSendMsgToRT = &SendMsgToEngine;

    ft->fSndFileFormatInfoFromStrings = &sndfileFormatInfoFromStrings;

    ft->fGetNode = &World_GetNode;
    ft->fGetGraph = &World_GetGraph;

    ft->fNRTLock = &World_NRTLock;
    ft->fNRTUnlock = &World_NRTUnlock;

    ft->fGroup_DeleteAll = &Group_DeleteAll;
    ft->fDoneAction = &Unit_DoneAction;
    ft->fDoAsynchronousCommand = &PerformAsynchronousCommand;
    ft->fDoAsynchronousCommandEx = &PerformAsynchronousCommandEx;
    ft->fDoAsyncUnitCommand = &PerformAsyncUnitCommand;
    ft->fBufAlloc = &bufAlloc;

    ft->fSCfftCreate = &scfft_create;
    ft->fSCfftDestroy = &scfft_destroy;
    ft->fSCfftDoFFT = &scfft_dofft;
    ft->fSCfftDoIFFT = &scfft_doifft;

    ft->fGetScopeBuffer = &supersonic_scope_get;
    ft->fPushScopeBuffer = &supersonic_scope_push;
    ft->fReleaseScopeBuffer = &supersonic_scope_release;
}
#endif // CLOCKWORK_GUEST

void initializeScheduler();

namespace scsynth {
void startAsioThread();
void stopAsioThread();
bool asioThreadStarted();
}

World* World_New(WorldOptions* inOptions, SupersonicGuest* inGuest, char* outReason, size_t reasonBytes) {
    // SuperSonic (plural guest): why this World_New returned null, for the host to pass on: without it the
    // caller has "World_New returned null" and nothing to act on. Written into
    // the caller's buffer where the failure is known — before the throw, since
    // what() does not survive the catch-all the wasm build lands in, and in
    // the typed handler where it can be asked. The first reason wins.
    if (outReason && reasonBytes)
        outReason[0] = '\0';
    const auto world_new_failed = [outReason, reasonBytes](const char* why) {
        if (outReason && reasonBytes && !outReason[0] && why)
            snprintf(outReason, reasonBytes, "%s", why);
    };
#if (_POSIX_MEMLOCK - 0) >= 200112L
    if (inOptions->mMemoryLocking && inOptions->mRealTime) {
        bool lock_memory = false;

        rlimit limit;

        int failure = getrlimit(RLIMIT_MEMLOCK, &limit);
        if (failure) {
            // getrlimit failed
        } else {
            if (limit.rlim_cur == RLIM_INFINITY and limit.rlim_max == RLIM_INFINITY)
                lock_memory = true;

            if (lock_memory) {
                if (mlockall(MCL_FUTURE) != -1) {
                    // memory locking enabled
                }
            }
        }
    }
#endif

    World* world = nullptr;
#if defined(CLOCKWORK_GUEST)
    // The working memory the host gave this guest, and what is left of it for the pool once the tables are placed.
    uint32_t guestArenaBytes = 0;
    void* guestArena = nullptr;
#endif

    try {
        // SuperSonic (plural guest): the World and its HiddenWorld first, from the instance's heap: the
        // World names its instance (hw->mGuest), and everything else it owns
        // is allocated through the World (guest_doors.h). A shell that cannot
        // be had is no World at all: given back, and nothing to clean up.
        world = (World*)supersonic_guest_alloc_bytes_for(inGuest, sizeof(World));
        if (!world) {
            const char* msg = "World_New: no memory for the World";
            world_new_failed(msg);
            return nullptr;
        }
        memset(world, 0, sizeof(World));
        world->hw = (HiddenWorld*)supersonic_guest_alloc_bytes_for(inGuest, sizeof(HiddenWorld));
        if (!world->hw) {
            supersonic_guest_free_bytes_for(inGuest, world);
            world = nullptr;
            const char* msg = "World_New: no memory for the World's tables";
            world_new_failed(msg);
            return nullptr;
        }
        memset(world->hw, 0, sizeof(HiddenWorld));
        world->hw->mGuest = inGuest;
        world->hw->mGuestMalloc.mWorld = world;
#if defined(CLOCKWORK_GUEST)
        // SuperSonic (plural guest): the working memory the host gave this guest: the real-time pool's.
        guestArena = const_cast<void*>(supersonic_guest_memory(world, &guestArenaBytes));
#endif
        // SuperSonic (plural guest): the World's own copy of the interface table, naming it, and its own
        // registries from the plugins' loads (SuperSonic, plural guest: upstream
        // does this once per process).
        world->hw->mInterfaceTable = gInterfaceTable;
        world->hw->mInterfaceTable.mWorld = world;
        world->ft = &world->hw->mInterfaceTable;
        initialize_library(world);
        initializeScheduler();

#if defined(CLOCKWORK_GUEST)
        {
            /*
             * The pool's first area, pre-sized here and handed over through
             * rt_pool_area_alloc: the guest's arena when it holds the whole
             * pool, else the instance's heap.
             *
             * A POOL THAT CANNOT GROW MUST NOT BE QUIETLY SHRUNK. With growth
             * at 0 — desktop, the NIF and web — a pool that cannot be had at
             * the size asked for is a boot that fails with a reason, not one
             * that plays with less memory than it said. Measured 2026-09-01:
             * a 128 MB request served out of a 32 MB span booted cleanly,
             * played, and reported no error anywhere.
             */
            // The working memory, less the tables placed at its start above.
            uint32_t arenaBytes = guestArenaBytes;
            void* arena = guestArena;
            const size_t want  = (size_t)inOptions->mRealTimeMemorySize * 1024;
            const size_t total = want + kAreaOverhead;
            void* area = nullptr;
            if (arena && (size_t)arenaBytes >= total) {
                area = arena;
                world->hw->mRtPoolArenaArea = arena;
            } else {
                area = supersonic_guest_alloc_bytes(world, total);
            }
            if (!area) {
                char msg[200];
                snprintf(msg, sizeof(msg),
                         "World_New: RT pool of %zu bytes was asked for and the guest's "
                         "memory cannot hold it - size the memory for it "
                         "(scsynth_heap_bytes) or lower realTimeMemorySize",
                         want);
                // LOGGED AND KEPT BEFORE IT IS THROWN, because what() does not
                // survive the trip. Catch-by-type needs RTTI the wasm build
                // does not carry, so this lands in World_New's catch-all, which
                // has an exception it cannot ask anything of.
                scprintf(world, "%s\n", msg);
                world_new_failed(msg);
                throw std::runtime_error(msg);
            }
            world->hw->mRtPoolFirstArea = area;
            world->hw->mAllocPool = guest_new<AllocPool>(world, rt_pool_area_alloc, rt_pool_area_free, world, want,
                                                         SCSYNTH_RT_POOL_GROWTH_SIZE);
        }
#else
        world->hw->mAllocPool = guest_new<AllocPool>(
            world, [](void*, size_t size) { return malloc(size); }, [](void*, void* ptr) { free(ptr); }, nullptr,
            inOptions->mRealTimeMemorySize * 1024, 0);
#endif
#ifndef SC_LEAN_TARGET
        world->hw->mQuitProgram = new sc::sync::semaphore(0);
#endif
        world->hw->mTerminating = false;

        HiddenWorld* hw = world->hw;
        // The instance's own, in the guest's memory (guest_heap.h; upstream:
        // new, on the system allocator).
        hw->mGraphDefLib = guest_new<GrafDefTable>(world, &hw->mGuestMalloc, inOptions->mMaxGraphDefs, false);
        hw->mNodeLib = guest_new<IntHashTable<Node, AllocPool>>(world, hw->mAllocPool, inOptions->mMaxNodes, false);
        hw->mUsers = guest_new<Clients>(world, std::less<ReplyAddress>(), GuestAllocator<ReplyAddress>(world));
        hw->mMaxUsers = inOptions->mMaxLogins;
        hw->mAvailableClientIDs = guest_new<ClientIDs>(world, GuestAllocator<int>(world));
        for (int i = 0; i < hw->mMaxUsers; i++) {
            hw->mAvailableClientIDs->push_back(i);
        }
        hw->mClientIDdict = guest_new<ClientIDDict>(world, std::less<ReplyAddress>(),
                                                     GuestAllocator<std::pair<const ReplyAddress, uint32>>(world));
        hw->mHiddenID = -8;
        hw->mRecentID = -8;
        hw->mSeedBase = inOptions->mDeterministicSeed;


        world->mNumUnits = 0;
        world->mNumGraphs = 0;
        world->mNumGroups = 0;

        world->mBufCounter = 0;
        world->mBufLength = inOptions->mBufLength;
        world->mSampleOffset = 0;
        world->mSubsampleOffset = 0.f;
        world->mNumAudioBusChannels = inOptions->mNumAudioBusChannels;
        world->mNumControlBusChannels = inOptions->mNumControlBusChannels;
        world->mNumInputs = inOptions->mNumInputBusChannels;
        world->mNumOutputs = inOptions->mNumOutputBusChannels;

        world->mVerbosity = inOptions->mVerbosity;
        world->mErrorNotification = 1; // i.e., 0x01 | 0x02
        world->mLocalErrorNotification = 0;

        // Control busses are process-local on every runtime: the host owns
        // whatever shared segment there is, and the engine publishes nothing
        // of its own into one.
        world->mControlBus = (float*)zalloc(world, world->mNumControlBusChannels, sizeof(float));

        world->mNumSharedControls = 0;
        world->mSharedControls = inOptions->mSharedControls;

        int numsamples = world->mBufLength * world->mNumAudioBusChannels;
        world->mAudioBus = (float*)zalloc(world, numsamples, sizeof(float));

        world->mAudioBusTouched = (int32*)zalloc(world, inOptions->mNumAudioBusChannels, sizeof(int32));
        world->mControlBusTouched = (int32*)zalloc(world, inOptions->mNumControlBusChannels, sizeof(int32));

        world->mNumSndBufs = inOptions->mNumBuffers;
        world->mSndBufs = (SndBuf*)zalloc(world, world->mNumSndBufs, sizeof(SndBuf));
        world->mSndBufsNonRealTimeMirror = (SndBuf*)zalloc(world, world->mNumSndBufs, sizeof(SndBuf));
        world->mSndBufUpdates = (SndBufUpdates*)zalloc(world, world->mNumSndBufs, sizeof(SndBufUpdates));

        GroupNodeDef_Init(world);

        int err = Group_New(world, 0, &world->mTopGroup);
        if (err)
            throw err;

        world->mRealTime = inOptions->mRealTime;

        world->mNumRGens = inOptions->mNumRGens;
        world->mRGen = guest_new_array<RGen>(world, world->mNumRGens);
        for (uint32 i = 0; i < world->mNumRGens; ++i) {
            world->mRGen[i].init(server_timeseed(world));
        }

        world->mNRTLock = guest_new<SC_Lock>(world);
        world->mDriverLock = guest_new<SC_Lock>(world);

        if (inOptions->mPassword) {
            strncpy(world->hw->mPassword, inOptions->mPassword, 31);
            world->hw->mPassword[31] = 0;
        } else {
            world->hw->mPassword[0] = 0;
        }
#ifdef SC_BELA
        world->hw->mBelaAnalogInputChannels = inOptions->mBelaAnalogInputChannels;
        world->hw->mBelaAnalogOutputChannels = inOptions->mBelaAnalogOutputChannels;
        world->hw->mBelaDigitalChannels = inOptions->mBelaDigitalChannels;
        world->hw->mBelaHeadphoneLevel = inOptions->mBelaHeadphoneLevel;
        world->hw->mBelaPgaGainLeft = inOptions->mBelaPgaGainLeft;
        world->hw->mBelaPgaGainRight = inOptions->mBelaPgaGainRight;
        world->hw->mBelaSpeakerMuted = inOptions->mBelaSpeakerMuted;
        world->hw->mBelaDacLevel = inOptions->mBelaDacLevel;
        world->hw->mBelaAdcLevel = inOptions->mBelaAdcLevel;
        world->hw->mBelaNumMuxChannels = inOptions->mBelaNumMuxChannels;
        world->hw->mBelaPru = inOptions->mBelaPru;
        world->mBelaMaxScopeChannels = inOptions->mBelaMaxScopeChannels;
#endif // SC_BELA

#ifdef __APPLE__
        world->hw->mInputStreamsEnabled = inOptions->mInputStreamsEnabled;
        world->hw->mOutputStreamsEnabled = inOptions->mOutputStreamsEnabled;
#endif
        world->hw->mInDeviceName = inOptions->mInDeviceName;
        world->hw->mOutDeviceName = inOptions->mOutDeviceName;
        hw->mMaxWireBufs = inOptions->mMaxWireBufs;
        hw->mWireBufSpace = nullptr;

        world->mRendezvous = inOptions->mRendezvous;

        world->mRestrictedPath = inOptions->mRestrictedPath;

        sc_SetDenormalFlags();

        if (world->mRealTime) {
            hw->mAudioDriver = SC_NewAudioDriver(world);
            hw->mAudioDriver->SetPreferredHardwareBufferFrameSize(inOptions->mPreferredHardwareBufferFrameSize);
            hw->mAudioDriver->SetPreferredSampleRate(inOptions->mPreferredSampleRate);
#ifdef __APPLE__
            hw->mAudioDriver->SetSafetyClipThreshold(inOptions->mSafetyClipThreshold);
#endif

            if (!hw->mAudioDriver->Setup()) {
                return nullptr;
            }
            if (!hw->mAudioDriver->Start()) {
                return nullptr;
            }

#ifdef __APPLE__
            SC::Apple::disableAppNap();
#endif


        } else {
            hw->mAudioDriver = nullptr;
        }

        if (!scsynth::asioThreadStarted()) {
            scsynth::startAsioThread();
        }

    } catch (std::exception& exc) {
        fprintf(stderr, "[World_New] EXCEPTION: %s\n", exc.what()); fflush(stderr);
        world_new_failed(exc.what());
        World_Cleanup(world, true);
        return nullptr;
    } catch (...) {
        /*
         * SAME AS THE TYPED HANDLER, and it used to fall through to
         * `return world` below.
         *
         * A World that threw on its way up is not a World. Returning the
         * half-built one handed the caller an object whose later allocations
         * had never run, so the failure surfaced somewhere else entirely:
         * EngineCore_New reported "bus-touched arrays not allocated", which is
         * true and says nothing about why.
         *
         * It is not a hypothetical path. Catch-by-type needs RTTI the wasm
         * build does not carry, so on that target EVERY exception raised in
         * here lands in this handler rather than the one above — measured
         * 2026-09-01, with an RT pool that could not fit its arena throwing a
         * std::runtime_error and the engine coming back "successfully" built.
         */
        fprintf(stderr, "[World_New] UNKNOWN EXCEPTION\n"); fflush(stderr);
        World_Cleanup(world, true);
        return nullptr;
    }
    return world;
}

int World_CopySndBuf(World* world, uint32 index, SndBuf* outBuf, bool onlyIfChanged, bool* outDidChange) {
    // index == mNumSndBufs is one past the mSndBuf*/mSndBufUpdates arrays (>=).
    if (index >= world->mNumSndBufs)
        return kSCErr_IndexOutOfRange;

    SndBufUpdates* updates = world->mSndBufUpdates + index;
    bool didChange = updates->reads != updates->writes;

    if (!onlyIfChanged || didChange) {
        reinterpret_cast<SC_Lock*>(world->mNRTLock)->lock();

        SndBuf* buf = world->mSndBufsNonRealTimeMirror + index;

        if (buf->data && buf->samples) {
            uint32 bufSize = buf->samples * sizeof(float);
            if (buf->samples != outBuf->samples) {
                free_alig(world, outBuf->data);
                outBuf->data = (float*)malloc_alig(world, bufSize);
            }
            memcpy(outBuf->data, buf->data, bufSize);
            outBuf->channels = buf->channels;
            outBuf->samples = buf->samples;
            outBuf->frames = buf->frames;
            outBuf->mask = buf->mask;
            outBuf->mask1 = buf->mask1;
        } else {
            free_alig(world, outBuf->data);
            outBuf->data = nullptr;
            outBuf->channels = 0;
            outBuf->samples = 0;
            outBuf->frames = 0;
            outBuf->mask = 0;
            outBuf->mask1 = 0;
        }

        outBuf->samplerate = buf->samplerate;
        outBuf->sampledur = buf->sampledur;
        outBuf->coord = buf->coord;
        outBuf->sndfile = nullptr;

        updates->reads = updates->writes;

        reinterpret_cast<SC_Lock*>(world->mNRTLock)->unlock();
    }

    if (outDidChange)
        *outDidChange = didChange;

    return kSCErr_None;
}

void PerformOSCBundle(World* inWorld, OSC_Packet* inPacket);

// Non-real-time synthesis functions - not needed for Emscripten/AudioWorklet builds
#ifndef NO_LIBSNDFILE

bool nextOSCPacket(FILE* file, OSC_Packet* packet, int64& outTime) {
    int32 msglen;
    if (fread(&msglen, 1, sizeof(int32), file) != sizeof(int32))
        return true;
    // msglen is in network byte order
    msglen = OSCint((char*)&msglen);
    if (msglen > 1073741824) {
        throw std::runtime_error("OSC packet too long. > 2^30 bytes\n");
    }
    packet->mData = (char*)realloc((void*)packet->mData, (size_t)msglen);
    if (!packet->mData)
        throw std::runtime_error("nextOSCPacket: realloc failed...\n");

    size_t read = fread(packet->mData, 1, msglen, file);
    if (read != msglen)
        throw std::runtime_error("nextOSCPacket: invalid read of OSC packet\n");

    if (strcmp(packet->mData, "#bundle") != 0)
        throw std::runtime_error("OSC packet not a bundle\n");

    packet->mSize = msglen;

    outTime = OSCtime(packet->mData + 8);
    return false;
}


void World_NonRealTimeSynthesis(struct World* world, WorldOptions* inOptions) noexcept(false) {
    int bufLength = world->mBufLength;
    int fileBufFrames = inOptions->mPreferredHardwareBufferFrameSize;
    if (fileBufFrames <= 0)
        fileBufFrames = 8192;
    int bufMultiple = (fileBufFrames + bufLength - 1) / bufLength;
    fileBufFrames = bufMultiple * bufLength;

    // batch process non real time audio
    if (!inOptions->mNonRealTimeOutputFilename)
        throw std::runtime_error("Non real time output filename is NULL.\n");

    SF_INFO inputFileInfo, outputFileInfo;
    float* inputFileBuf = nullptr;
    float* outputFileBuf = nullptr;
    int numInputChannels = 0;
    int numOutputChannels;

    outputFileInfo.samplerate = inOptions->mPreferredSampleRate;
    numOutputChannels = outputFileInfo.channels = world->mNumOutputs;
    sndfileFormatInfoFromStrings(&outputFileInfo, inOptions->mNonRealTimeOutputHeaderFormat,
                                 inOptions->mNonRealTimeOutputSampleFormat);

    world->hw->mNRTOutputFile = sndfileOpenFromCStr(inOptions->mNonRealTimeOutputFilename, SFM_WRITE, &outputFileInfo);
    sf_command(world->hw->mNRTOutputFile, SFC_SET_CLIPPING, nullptr, SF_TRUE);

    if (!world->hw->mNRTOutputFile)
        throw std::runtime_error("Couldn't open non real time output file.\n");

    outputFileBuf = (float*)calloc(1, world->mNumOutputs * fileBufFrames * sizeof(float));

    if (inOptions->mNonRealTimeInputFilename) {
        world->hw->mNRTInputFile = sndfileOpenFromCStr(inOptions->mNonRealTimeInputFilename, SFM_READ, &inputFileInfo);
        if (!world->hw->mNRTInputFile)
            throw std::runtime_error("Couldn't open non real time input file.\n");

        inputFileBuf = (float*)calloc(1, inputFileInfo.channels * fileBufFrames * sizeof(float));

        numInputChannels = world->mNumInputs = inputFileInfo.channels; // force it.

    } else {
        world->hw->mNRTInputFile = nullptr;
    }

    FILE* cmdFile;
    if (inOptions->mNonRealTimeCmdFilename) {
#    ifdef _WIN32
        cmdFile = fopen(inOptions->mNonRealTimeCmdFilename, "rb");
#    else
        cmdFile = fopen(inOptions->mNonRealTimeCmdFilename, "r");
#    endif
    } else
        cmdFile = stdin;
    if (!cmdFile)
        throw std::runtime_error("Couldn't open non real time command file.\n");

    OSC_Packet packet;
    memset(&packet, 0, sizeof(packet));
    packet.mData = (char*)malloc(8192);
    packet.mIsBundle = true;
    packet.mReplyAddr.mReplyFunc = null_reply_func;

    int64 schedTime;
    if (nextOSCPacket(cmdFile, &packet, schedTime))
        throw std::runtime_error("command file empty.\n");
    int64 prevTime = schedTime;

    World_SetSampleRate(world, inOptions->mPreferredSampleRate);
    World_Start(world);

    int64 oscTime = 0;
    double oscToSeconds = 1. / pow(2., 32.);
    double oscToSamples = inOptions->mPreferredSampleRate * oscToSeconds;
    int64 oscInc = (int64)((double)bufLength / oscToSamples);

    if (inOptions->mVerbosity >= 0) {
        printf("start time %g\n", schedTime * oscToSeconds);
    }

    bool run = true;
    int inBufStep = numInputChannels * bufLength;
    int outBufStep = numOutputChannels * bufLength;
    float* inputBuses = world->mAudioBus + world->mNumOutputs * bufLength;
    float* outputBuses = world->mAudioBus;
    int32* inputTouched = world->mAudioBusTouched + world->mNumOutputs;
    int32* outputTouched = world->mAudioBusTouched;
    for (; run;) {
        int bufFramesCalculated = 0;
        float* inBufPos = inputFileBuf;
        float* outBufPos = outputFileBuf;

        if (world->hw->mNRTInputFile) {
            int framesRead = sf_readf_float(world->hw->mNRTInputFile, inputFileBuf, fileBufFrames);
            if (framesRead < fileBufFrames) {
                memset(inputFileBuf + framesRead * numInputChannels, 0,
                       (fileBufFrames - framesRead) * numInputChannels * sizeof(float));
            }
        }

        for (int i = 0; i < bufMultiple && run; ++i) {
            int bufCounter = world->mBufCounter;

            // deinterleave input to input buses
            if (inputFileBuf) {
                float* inBus = inputBuses;
                for (int j = 0; j < numInputChannels; ++j, inBus += bufLength) {
                    float* inFileBufPtr = inBufPos + j;
                    for (int k = 0; k < bufLength; ++k) {
                        inBus[k] = *inFileBufPtr;
                        inFileBufPtr += numInputChannels;
                    }
                    inputTouched[j] = bufCounter;
                }
            }

            // execute ready commands
            int64 nextTime = oscTime + oscInc;

            while (schedTime <= nextTime) {
                float diffTime = (float)(schedTime - oscTime) * oscToSamples + 0.5;
                float diffTimeFloor = floor(diffTime);
                world->mSampleOffset = (int)diffTimeFloor;
                world->mSubsampleOffset = diffTime - diffTimeFloor;

                if (world->mSampleOffset < 0)
                    world->mSampleOffset = 0;
                else if (world->mSampleOffset >= bufLength)
                    world->mSampleOffset = bufLength - 1;


                PerformOSCBundle(world, &packet);
                if (nextOSCPacket(cmdFile, &packet, schedTime)) {
                    run = false;
                    break;
                }
                if (inOptions->mVerbosity >= 0) {
                    printf("nextOSCPacket %g\n", schedTime * oscToSeconds);
                }
                if (schedTime < prevTime) {
                    scprintf(world, "ERROR: Packet time stamps out-of-order.\n");
                    run = false;
                    goto Bail;
                }
                prevTime = schedTime;
            }

            World_Run(world);

            // interleave output to output buffer
            float* outBus = outputBuses;
            for (int j = 0; j < numOutputChannels; ++j, outBus += bufLength) {
                float* outFileBufPtr = outBufPos + j;
                if (outputTouched[j] == bufCounter) {
                    for (int k = 0; k < bufLength; ++k) {
                        *outFileBufPtr = outBus[k];
                        outFileBufPtr += numOutputChannels;
                    }
                } else {
                    for (int k = 0; k < bufLength; ++k) {
                        *outFileBufPtr = 0.f;
                        outFileBufPtr += numOutputChannels;
                    }
                }
            }
            bufFramesCalculated += bufLength;
            inBufPos += inBufStep;
            outBufPos += outBufStep;
            world->mBufCounter++;
            oscTime = nextTime;
        }

    Bail:
        // write output
        sf_writef_float(world->hw->mNRTOutputFile, outputFileBuf, bufFramesCalculated);
    }

    if (cmdFile != stdin)
        fclose(cmdFile);
    sf_close(world->hw->mNRTOutputFile);
    world->hw->mNRTOutputFile = nullptr;

    if (world->hw->mNRTInputFile) {
        sf_close(world->hw->mNRTInputFile);
        world->hw->mNRTInputFile = nullptr;
    }

    free(packet.mData);
    World_Cleanup(world, true);
}
#endif // !NO_LIBSNDFILE

void World_WaitForQuit(struct World* inWorld, bool unload_plugins) {
    try {
#ifndef SC_LEAN_TARGET
        inWorld->hw->mQuitProgram->wait();
#endif
        World_Cleanup(inWorld, unload_plugins);
    } catch (...) {
        // Exception during quit
    }
}

void World_SetSampleRate(World* inWorld, double inSampleRate) {
    inWorld->mSampleRate = inSampleRate;
    Rate_Init(&inWorld->mFullRate, inSampleRate, inWorld->mBufLength);
    Rate_Init(&inWorld->mBufRate, inSampleRate / inWorld->mBufLength, 1);
}

////////////////////////////////////////////////////////////////////////////////

void* World_Alloc(World* inWorld, size_t inByteSize) {
    // A wasm-only bounds check used to sit here, comparing the result against
    // the externally supplied RT pool. It was the only thing standing between
    // an out-of-pool allocation and silence, because on that target the pool
    // came from outside any allocator that could bound it. It does not now:
    // the areas behind mAllocPool are taken from clockwork::mem, which fails a
    // request that does not fit rather than serving it from somewhere else.
    return inWorld->hw->mAllocPool->Alloc(inByteSize);
}

void* World_Realloc(World* inWorld, void* inPtr, size_t inByteSize) {
    return inWorld->hw->mAllocPool->Realloc(inPtr, inByteSize);
}

size_t World_TotalFree(World* inWorld) { return inWorld->hw->mAllocPool->TotalFree(); }

size_t World_LargestFreeChunk(World* inWorld) { return inWorld->hw->mAllocPool->LargestFreeChunk(); }

void World_Free(World* inWorld, void* inPtr) {
    inWorld->hw->mAllocPool->Free(inPtr);
}

////////////////////////////////////////////////////////////////////////////////

int32* GetKey(GraphDef* inGraphDef) { return inGraphDef->mNodeDef.mName; }

int32 GetHash(GraphDef* inGraphDef) { return inGraphDef->mNodeDef.mHash; }

void World_AddGraphDef(World* inWorld, GraphDef* inGraphDef) {
    bool added = inWorld->hw->mGraphDefLib->Add(inGraphDef);
    if (!added)
        scprintf(inWorld,
            "ERROR: Could not add SynthDef %s.\nTry adjusting ServerOptions:maxSynthDefs or the -d cmdline flag.\n",
            (char*)inGraphDef->mNodeDef.mName);
    for (uint32 i = 0; i < inGraphDef->mNumVariants; ++i) {
        GraphDef* var = inGraphDef->mVariants + i;
        added = inWorld->hw->mGraphDefLib->Add(var);
        if (!added)
            scprintf(inWorld,
                "ERROR: Could not add SynthDef %s.\nTry adjusting ServerOptions:maxSynthDefs or the -d cmdline flag.\n",
                (char*)var->mNodeDef.mName);
    }
}

void World_RemoveGraphDef(World* inWorld, GraphDef* inGraphDef) {
    for (uint32 i = 0; i < inGraphDef->mNumVariants; ++i) {
        GraphDef* var = inGraphDef->mVariants + i;
        inWorld->hw->mGraphDefLib->Remove(var);
    }
    inWorld->hw->mGraphDefLib->Remove(inGraphDef);
}

/** @brief implements the \c /d_freeAll command */
void World_FreeAllGraphDefs(World* inWorld) {
    GrafDefTable* lib = inWorld->hw->mGraphDefLib;
    int size = lib->TableSize();
    GraphDef* defList = nullptr;

    for (int i = 0; i < size; ++i) {
        GraphDef* def = lib->AtIndex(i);
        if (def) {
            assert(def->mRefCount > 0);
            if (--def->mRefCount == 0) {
                // add to list
                assert(def->mNext == nullptr);
                def->mNext = defList;
                defList = def;
            }
        }
    }

    lib->MakeEmpty();

    if (defList) {
        // free all released GraphDefs at once: see DeleteGraphDefMsg (SC_HiddenWorld.h)
        DeleteGraphDefMsg msg;
        msg.mWorld = inWorld;
        msg.mDefList = defList;
        msg.Perform();
    }
}

GraphDef* World_GetGraphDef(World* inWorld, int32* inKey) { return inWorld->hw->mGraphDefLib->Get(inKey); }

////////////////////////////////////////////////////////////////////////////////

int32* GetKey(UnitDef* inUnitDef) { return inUnitDef->mUnitDefName; }

int32 GetHash(UnitDef* inUnitDef) { return inUnitDef->mHash; }

bool AddUnitDef(World* inWorld, UnitDef* inUnitDef) { return inWorld->hw->mUnitDefLib->Add(inUnitDef); }

bool RemoveUnitDef(World* inWorld, UnitDef* inUnitDef) { return inWorld->hw->mUnitDefLib->Remove(inUnitDef); }

UnitDef* GetUnitDef(World* inWorld, int32* inKey) {
    return inWorld->hw->mUnitDefLib->Get(inKey);
}

////////////////////////////////////////////////////////////////////////////////

int32* GetKey(BufGen* inBufGen) { return inBufGen->mBufGenName; }

int32 GetHash(BufGen* inBufGen) { return inBufGen->mHash; }

bool AddBufGen(World* inWorld, BufGen* inBufGen) { return inWorld->hw->mBufGenLib->Add(inBufGen); }

bool RemoveBufGen(World* inWorld, BufGen* inBufGen) { return inWorld->hw->mBufGenLib->Remove(inBufGen); }

BufGen* GetBufGen(World* inWorld, int32* inKey) { return inWorld->hw->mBufGenLib->Get(inKey); }

////////////////////////////////////////////////////////////////////////////////

int32* GetKey(PlugInCmd* inPlugInCmd) { return inPlugInCmd->mCmdName; }

int32 GetHash(PlugInCmd* inPlugInCmd) { return inPlugInCmd->mHash; }

bool AddPlugInCmd(World* inWorld, PlugInCmd* inPlugInCmd) { return inWorld->hw->mPlugInCmds->Add(inPlugInCmd); }

bool RemovePlugInCmd(World* inWorld, PlugInCmd* inPlugInCmd) {
    return inWorld->hw->mPlugInCmds->Remove(inPlugInCmd);
}

PlugInCmd* GetPlugInCmd(World* inWorld, int32* inKey) { return inWorld->hw->mPlugInCmds->Get(inKey); }

////////////////////////////////////////////////////////////////////////////////

int32 GetKey(Node* inNode) { return inNode->mID; }

int32 GetHash(Node* inNode) { return inNode->mHash; }

bool World_AddNode(World* inWorld, Node* inNode) { return inWorld->hw->mNodeLib->Add(inNode); }

bool World_RemoveNode(World* inWorld, Node* inNode) { return inWorld->hw->mNodeLib->Remove(inNode); }

Node* World_GetNode(World* inWorld, int32 inID) {
    if (inID == -1)
        inID = inWorld->hw->mRecentID;
    return inWorld->hw->mNodeLib->Get(inID);
}

Graph* World_GetGraph(World* inWorld, int32 inID) {
    if (inID == -1)
        inID = inWorld->hw->mRecentID;
    Node* node = World_GetNode(inWorld, inID);
    if (!node)
        return nullptr;
    return node->mIsGroup ? nullptr : (Graph*)node;
}

Group* World_GetGroup(World* inWorld, int32 inID) {
    Node* node = World_GetNode(inWorld, inID);
    if (!node)
        return nullptr;
    return node->mIsGroup ? (Group*)node : nullptr;
}

////////////////////////////////////////////////////////////////////////////////


void World_Run(World* inWorld) {
    // Validate world before running
    if (!inWorld) {
        scprintf(inWorld, "ERROR: World_Run called with null World");
        return;
    }
    if (!inWorld->mTopGroup) {
        scprintf(inWorld, "ERROR: World_Run: mTopGroup is null");
        return;
    }

    // run top group
    Node* node = (Node*)inWorld->mTopGroup;
    if (!node->mCalcFunc) {
        scprintf(inWorld, "ERROR: World_Run: TopGroup has null mCalcFunc");
        return;
    }

    (*node->mCalcFunc)(node);
}

void World_Start(World* inWorld) {
    inWorld->mBufCounter = 0;
    for (uint32 i = 0; i < inWorld->mNumAudioBusChannels; ++i)
        inWorld->mAudioBusTouched[i] = -1;
    for (uint32 i = 0; i < inWorld->mNumControlBusChannels; ++i)
        inWorld->mControlBusTouched[i] = -1;

    inWorld->hw->mWireBufSpace =
        (float*)malloc_alig(inWorld, inWorld->hw->mMaxWireBufs * inWorld->mBufLength * sizeof(float));

    // A graph's block is at most the whole wire buffer space (Graph_Ctor).
    const size_t portBufferSize = (size_t)inWorld->hw->mMaxWireBufs * inWorld->mBufLength * sizeof(float);
    Wire& missingInput = inWorld->hw->mMissingInputWire;
    missingInput.mFromUnit = nullptr;
    missingInput.mCalcRate = calc_ScalarRate;
    missingInput.mBuffer = (float*)malloc_alig(inWorld, portBufferSize);
    missingInput.mScalarValue = 0.f;
    if (missingInput.mBuffer)
        memset(missingInput.mBuffer, 0, portBufferSize);
    inWorld->mMissingInput = &missingInput;
    inWorld->mMissingOutput = (float*)malloc_alig(inWorld, portBufferSize);

    inWorld->hw->mTriggers.MakeEmpty();
    inWorld->hw->mNodeMsgs.MakeEmpty();
    inWorld->hw->mNodeEnds.MakeEmpty();
    inWorld->mRunning = true;
}

void World_Cleanup(World* world, bool unload_plugins) {
    if (!world)
        return;

    if (scsynth::asioThreadStarted()) {
        scsynth::stopAsioThread();
    }

    HiddenWorld* hw = world->hw;
    assert(hw != nullptr);

    // A World_New that threw after mRealTime was set but before the driver
    // was made has no driver to stop.
    if (hw && world->mRealTime && hw->mAudioDriver)
        hw->mAudioDriver->Stop();

    world->mRunning = false;

    if (world->mTopGroup)
        Group_DeleteAll(world->mTopGroup);

    // Unload plugins after all nodes are destroyed, so that unit commands
    // and destructor functions are still available during node cleanup.
    // The registries are this World's (SuperSonic, plural guest): freed with
    // it, whatever unload_plugins says.
    deinitialize_library(world);

    // Guard against partially-constructed World (e.g. World_New catch block)
    if (world->mDriverLock)
        reinterpret_cast<SC_Lock*>(world->mDriverLock)->lock();
    if (hw) {
        sc_free(world, hw->mWireBufSpace);
        sc_free(world, hw->mMissingInputWire.mBuffer);
        sc_free(world, world->mMissingOutput);
        delete hw->mAudioDriver;
        hw->mAudioDriver = nullptr;
    }
    if (world->mNRTLock)
        guest_delete(world, reinterpret_cast<SC_Lock*>(world->mNRTLock));
    if (world->mDriverLock) {
        reinterpret_cast<SC_Lock*>(world->mDriverLock)->unlock();
        guest_delete(world, reinterpret_cast<SC_Lock*>(world->mDriverLock));
    }
    // A World_New that threw before the top group was made — a real-time
    // pool the heap could not hold, say — has no group to free, and no pool
    // to free it from.
    if (world->mTopGroup)
        World_Free(world, world->mTopGroup);

    for (uint32 i = 0; i < world->mNumSndBufs; ++i) {
        SndBuf* nrtbuf = world->mSndBufsNonRealTimeMirror + i;
        SndBuf* rtbuf = world->mSndBufs + i;

        // Bytes in the assets area are the client's (an asset, or a /b_allocPtr
        // region): clockwork releases every asset when the instance goes,
        // and the client reclaims. Only heap memory is ours to free.
        if (nrtbuf->data && !supersonic_assets_contains(world, nrtbuf->data))
            free_alig(world, nrtbuf->data);
        if (rtbuf->data && rtbuf->data != nrtbuf->data && !supersonic_assets_contains(world, rtbuf->data))
            free_alig(world, rtbuf->data);

#ifndef NO_LIBSNDFILE
        if (nrtbuf->sndfile)
            sf_close(GETSNDFILE(nrtbuf));
        if (rtbuf->sndfile && rtbuf->sndfile != nrtbuf->sndfile)
            sf_close(GETSNDFILE(rtbuf));
#endif
    }

    free_alig(world, world->mSndBufsNonRealTimeMirror);
    free_alig(world, world->mSndBufs);

    free_alig(world, world->mControlBusTouched);
    free_alig(world, world->mAudioBusTouched);
    free_alig(world, world->mControlBus);
    free_alig(world, world->mAudioBus);
    guest_delete_array(world, world->mRGen);
    // SuperSonic (plural guest): the shell goes last, through the instance it names: the World's own
    // doors need hw until then.
    SupersonicGuest* guest = hw ? hw->mGuest : nullptr;
    if (hw) {
#ifndef NO_LIBSNDFILE
        if (hw->mNRTInputFile)
            sf_close(hw->mNRTInputFile);
        if (hw->mNRTOutputFile)
            sf_close(hw->mNRTOutputFile);
        if (hw->mNRTCmdFile)
            fclose(hw->mNRTCmdFile);
#endif
        guest_delete(world, hw->mUsers);
        guest_delete(world, hw->mAvailableClientIDs);
        guest_delete(world, hw->mClientIDdict);
        guest_delete(world, hw->mNodeLib);
        guest_delete(world, hw->mGraphDefLib);
#ifndef SC_LEAN_TARGET
        delete hw->mQuitProgram;
#endif
        guest_delete(world, hw->mAllocPool);
        supersonic_guest_free_bytes_for(guest, hw);
    }
    supersonic_guest_free_bytes_for(guest, world);
}


void World_NRTLock(World* world) { reinterpret_cast<SC_Lock*>(world->mNRTLock)->lock(); }

void World_NRTUnlock(World* world) { reinterpret_cast<SC_Lock*>(world->mNRTLock)->unlock(); }

////////////////////////////////////////////////////////////////////////////////

inline int32 BUFMASK(int32 x) { return (1 << (31 - CLZ(x))) - 1; }

SCErr bufAlloc(World* inWorld, SndBuf* buf, int numChannels, int numFrames, double sampleRate) {
    // numChannels/numFrames arrive straight from OSC (/b_alloc, /b_allocRead,
    // …). Their product must be computed in 64-bit: a 32-bit numFrames *
    // numChannels can overflow to a small or negative value, under-allocating
    // the buffer while frames/channels still advertise the large dimensions —
    // a heap OOB in any UGen that indexes frame*channels. The upper bound also
    // keeps numSamples*sizeof(float) from wrapping a 32-bit size_t on wasm32
    // and keeps the count inside SndBuf's int fields.
    if (numChannels < 1 || numFrames < 1)
        return kSCErr_Failed;
    int64 numSamples = (int64)numFrames * (int64)numChannels;
    if (numSamples > (int64)0x7FFFFFFF / (int64)sizeof(float))
        return kSCErr_Failed;
    // A request that cannot be met leaves the buffer as it was, its samples and
    // the counts that describe them together.
    float* data = (float*)zalloc(inWorld, (size_t)numSamples, sizeof(float));
    if (!data)
        return kSCErr_Failed;

    buf->data = data;
    buf->channels = numChannels;
    buf->frames = numFrames;
    buf->samples = (int)numSamples;
    buf->mask = BUFMASK((int32)numSamples); // for delay lines
    buf->mask1 = buf->mask - 1; // for oscillators
    buf->samplerate = sampleRate;
    buf->sampledur = 1. / sampleRate;

    return kSCErr_None;
}

#include "scsynthsend.h"

void TriggerMsg::Perform() {
    small_scpacket packet;
    packet.adds("/tr");
    packet.maketags(4);
    packet.addtag(',');
    packet.addtag('i');
    packet.addtag('i');
    packet.addtag('f');
    packet.addi(mNodeID);
    packet.addi(mTriggerID);
    packet.addf(mValue);

    for (auto addr : *mWorld->hw->mUsers)
        SendReply(&addr, packet.data(), packet.size());
}

static void NodeReplyMsg_RTFree(FifoMsg* msg) {
    World_Free(msg->mWorld, msg->mData);
}

void NodeReplyMsg::Perform() {
    small_scpacket packet;
    packet.adds(mCmdName, mCmdNameSize);
    packet.maketags(3 + mNumArgs);
    packet.addtag(',');
    packet.addtag('i');
    packet.addi(mNodeID);
    packet.addtag('i');
    packet.addi(mID);
    for (int i = 0; i < mNumArgs; ++i) {
        packet.addtag('f');
        packet.addf(mValues[i]);
    }

    for (auto addr : *mWorld->hw->mUsers)
        SendReply(&addr, packet.data(), packet.size());

    // Free memory in realtime thread
    if (AudioDriver(mWorld)) {
        FifoMsg msg;
        msg.Set(mWorld, NodeReplyMsg_RTFree, nullptr, mRTMemory);
        AudioDriver(mWorld)->SendMsgToEngine(msg);
    } else {
        // No audio driver (SAB/NRT mode) — free directly
        World_Free(mWorld, mRTMemory);
    }
}


void NodeEndMsg::Perform() {
    small_scpacket packet;
    switch (mState) {
    case kNode_Go:
        packet.adds("/n_go");
        break;
    case kNode_End:
        packet.adds("/n_end");
        break;
    case kNode_On:
        packet.adds("/n_on");
        break;
    case kNode_Off:
        packet.adds("/n_off");
        break;
    case kNode_Move:
        packet.adds("/n_move");
        break;
    case kNode_Info:
        packet.adds("/n_info");
        break;
    }
    if (mIsGroup) {
        packet.maketags(8);
        packet.addtag(',');
        packet.addtag('i');
        packet.addtag('i');
        packet.addtag('i');
        packet.addtag('i');
        packet.addtag('i');
        packet.addtag('i');
        packet.addtag('i');
        packet.addi(mNodeID);
        packet.addi(mGroupID);
        packet.addi(mPrevNodeID);
        packet.addi(mNextNodeID);
        packet.addi(mIsGroup);
        packet.addi(mHeadID);
        packet.addi(mTailID);
    } else {
        packet.maketags(6);
        packet.addtag(',');
        packet.addtag('i');
        packet.addtag('i');
        packet.addtag('i');
        packet.addtag('i');
        packet.addtag('i');
        packet.addi(mNodeID);
        packet.addi(mGroupID);
        packet.addi(mPrevNodeID);
        packet.addi(mNextNodeID);
        packet.addi(mIsGroup);
    }

    for (auto addr : *mWorld->hw->mUsers)
        SendReply(&addr, packet.data(), packet.size());
}

void DeleteGraphDefMsg::Perform() {
    for (GraphDef* def = mDefList; def != nullptr;) {
        GraphDef* next = def->mNext;

        // notify clients
        small_scpacket packet;
        packet.adds("/d_removed");
        packet.maketags(2);
        packet.addtag(',');
        packet.addtag('s');
        packet.adds((char*)def->mNodeDef.mName);

        for (const auto& addr : *mWorld->hw->mUsers) {
            // TODO: fix signature of SendReply and ReplyFunc
            SendReply(const_cast<ReplyAddress*>(&addr), packet.data(), packet.size());
        }

        GraphDef_Free(mWorld, def);

        def = next;
    }
}

void NotifyNoArgs(World* inWorld, char* inString) {
    small_scpacket packet;
    packet.adds(inString);

    for (auto addr : *inWorld->hw->mUsers)
        SendReply(&addr, packet.data(), packet.size());
}


SCBool SendMsgToEngine(World* inWorld, FifoMsg* inMsg) {
    if (!inWorld->hw->mAudioDriver) return false;
    return inWorld->hw->mAudioDriver->SendMsgToEngine(*inMsg);
}

SCBool SendMsgFromEngine(World* inWorld, FifoMsg* inMsg) {
    if (!inWorld->hw->mAudioDriver) return false;
    return inWorld->hw->mAudioDriver->SendMsgFromEngine(*inMsg);
}

#ifndef CLOCKWORK_GUEST
void SetPrintFunc(PrintFunc func) { gPrint = func; }
#endif
