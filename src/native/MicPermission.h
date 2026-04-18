/*
 * MicPermission.h — macOS microphone permission check
 */
#pragma once

#ifdef __APPLE__

#include <string>

namespace MicPermission {

// Returns a string describing the current mic authorization status:
//   "notDetermined" — permission has not been requested yet
//   "restricted"    — access restricted (e.g. parental controls)
//   "denied"        — user has denied access
//   "authorized"    — user has granted access
//   "unknown"       — unexpected status
std::string status();

// Request microphone access. Returns immediately; user sees prompt
// asynchronously. Log the eventual result via logRequestResult.
void requestAccess();

} // namespace MicPermission

#endif
