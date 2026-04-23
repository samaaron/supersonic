/*
 * MicPermission.mm — macOS microphone permission check via AVFoundation
 */
#ifdef __APPLE__

#include "MicPermission.h"
#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#include <libproc.h>
#include <unistd.h>
#include <cstdio>

// Private API from libsystem_secinit. Given a PID, returns the PID macOS
// considers "responsible" for it (for TCC / LaunchServices attribution).
// Returns 0 on error, or the input pid if no responsible-process mapping.
extern "C" pid_t responsibility_get_pid_responsible_for_pid(pid_t);

namespace MicPermission {

std::string status() {
    AVAuthorizationStatus s =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    switch (s) {
        case AVAuthorizationStatusNotDetermined: return "notDetermined";
        case AVAuthorizationStatusRestricted:    return "restricted";
        case AVAuthorizationStatusDenied:        return "denied";
        case AVAuthorizationStatusAuthorized:    return "authorized";
        default:                                 return "unknown";
    }
}

void logDiagnostics() {
    pid_t self = getpid();
    char selfPath[PROC_PIDPATHINFO_MAXSIZE] = {0};
    proc_pidpath(self, selfPath, sizeof(selfPath));

    NSBundle* main = [NSBundle mainBundle];
    const char* bundleID = [[main bundleIdentifier] UTF8String];
    const char* bundlePath = [[main bundlePath] UTF8String];

    pid_t respPid = responsibility_get_pid_responsible_for_pid(self);
    char respPath[PROC_PIDPATHINFO_MAXSIZE] = {0};
    if (respPid > 0) proc_pidpath(respPid, respPath, sizeof(respPath));

    // Always-on: mic-permission diagnostics at boot are essential for
    // triaging "live_audio is silent" reports. 4 lines once per process,
    // negligible overhead, invaluable in user bug reports.
    fprintf(stderr, "[tcc-diag] self.pid=%d self.path=%s\n", self, selfPath);
    fprintf(stderr, "[tcc-diag] self.bundleID=%s self.bundlePath=%s\n",
            bundleID ? bundleID : "(nil)", bundlePath ? bundlePath : "(nil)");
    fprintf(stderr, "[tcc-diag] responsible.pid=%d responsible.path=%s\n",
            respPid, respPath[0] ? respPath : "(unknown)");
    fprintf(stderr, "[tcc-diag] mic.status=%s\n", status().c_str());
    fflush(stderr);
}

} // namespace MicPermission

#endif
