/*
 * MicPermission.h — macOS microphone permission check
 */
#pragma once

#ifdef __APPLE__

#include <string>

namespace MicPermission {

// Returns the current mic authorization status:
//   "notDetermined" — permission has not been requested yet
//   "restricted"    — access restricted (e.g. parental controls)
//   "denied"        — user has denied access
//   "authorized"    — user has granted access
//   "unknown"       — unexpected status
std::string status();

// Dump TCC-relevant diagnostics to stderr: our PID, bundle ID (if any),
// responsible PID, responsible process path, and current mic status.
// Used to observe how macOS attributes our process for TCC lookups.
void logDiagnostics();

} // namespace MicPermission

#endif
