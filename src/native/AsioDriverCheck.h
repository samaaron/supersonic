/*
 * AsioDriverCheck.h — Reject ASIO drivers this process could never load
 *
 * JUCE enumerates ASIO devices from HKEY_LOCAL_MACHINE\software\asio,
 * which lists every *installed* driver and says nothing about whether
 * the driver's DLL can be instantiated here. A driver built for another
 * architecture — an x64 ASIO DLL on an ARM64 host, the usual case since
 * most vendors ship x86/x64 only — can never load in-process:
 * CoCreateInstance fails with ERROR_BAD_EXE_FORMAT. JUCE reports that as
 * a bare "No such device", indistinguishable from unplugged hardware.
 *
 * Worse, such a driver still reaches the device list: the probe returns
 * no channel counts, the 0 -> 2-out/1-in fallback in listDevices invents
 * plausible ones, and the entry is broadcast as a working device the user
 * can select but never open.
 *
 * The check below asks the Windows loader — the authority on whether an
 * image is loadable here, and correct by construction for ARM64EC/ARM64X
 * hybrids that a hand-rolled PE machine-type comparison would misjudge.
 */
#pragma once

#include <set>
#include <string>

namespace sonicpi::device {

// Display names (exactly as JUCE reports them, i.e. the driver's
// `description` value, falling back to its registry key name) of ASIO
// drivers whose DLL the Windows loader definitively rejects.
//
// Deliberately conservative — a driver is named here only when Windows
// itself refused the image with ERROR_BAD_EXE_FORMAT. Anything missing,
// unreadable, unregistered, or failing for any other reason is treated as
// usable, so a working driver is never hidden by a failed lookup.
//
// Always empty off Windows.
std::set<std::string> unloadableAsioDrivers();

} // namespace sonicpi::device
