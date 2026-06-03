/*
 * SC_PlatformProfile.inc — re-includable capability table (single source of truth).
 *
 * Input:  exactly one of SCP_TARGET_DESKTOP / SCP_TARGET_WASM / SCP_TARGET_ESP32.
 * Output: SCP_HOSTED_OS / SCP_BYTE_ATOMICS / SCP_TIERED_MEMORY  (each 0 or 1).
 *
 * Re-includable BY DESIGN (no include guard): the outputs are #undef'd at the top
 * so a host build can include this once per forced target and validate every
 * profile at compile time (see test/native/test_platform.cpp). SC_Platform.h
 * includes it once — target chosen from the real platform macros — and maps the
 * SCP_* outputs to the public SC_HAS_* capabilities.
 *
 * To add a target: add one row here and a selection branch in SC_Platform.h.
 */
#undef SCP_HOSTED_OS
#undef SCP_BYTE_ATOMICS
#undef SCP_TIERED_MEMORY

#if defined(SCP_TARGET_DESKTOP)
// Full desktop/native OS: boost::asio networking, cross-process shared memory,
// host-filesystem synthdef loading; std::atomic_flag; a single RAM tier.
#    define SCP_HOSTED_OS     1
#    define SCP_BYTE_ATOMICS  1
#    define SCP_TIERED_MEMORY 0
#elif defined(SCP_TARGET_WASM)
// Browser / AudioWorklet: self-driven, OSC exchanged in-process (no sockets, no
// host FS); byte atomics are fine; a single heap (SAB pool / inline passthrough).
#    define SCP_HOSTED_OS     0
#    define SCP_BYTE_ATOMICS  1
#    define SCP_TIERED_MEMORY 0
#elif defined(SCP_TARGET_ESP32)
// ESP32-S3: self-driven; Xtensa has 32-bit S32C1I but no byte test-and-set
// (std::atomic_flag won't link); fast internal SRAM + slow PSRAM => two-tier
// placement allocator.
#    define SCP_HOSTED_OS     0
#    define SCP_BYTE_ATOMICS  0
#    define SCP_TIERED_MEMORY 1
#else
#    error "SC_PlatformProfile.inc: define exactly one SCP_TARGET_*"
#endif
