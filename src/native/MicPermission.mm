/*
 * MicPermission.mm — macOS microphone permission check via AVFoundation
 */
#ifdef __APPLE__

#include "MicPermission.h"
#import <AVFoundation/AVFoundation.h>
#include <cstdio>

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

void requestAccess() {
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                             completionHandler:^(BOOL granted) {
        fprintf(stderr, "[mic-permission] request result: %s\n",
                granted ? "GRANTED" : "DENIED");
        fflush(stderr);
    }];
}

} // namespace MicPermission

#endif
