#include "Diagnostics/AppNap.h"

#import <Foundation/Foundation.h>

namespace dcr
{

AppNapDisabler::AppNapDisabler()
{
    // UserInitiatedAllowingIdleSystemSleep keeps us out of App Nap while
    // still letting the Mac idle-sleep normally; LatencyCritical marks this
    // as real-time audio work so the high-resolution timers our engine
    // watchdog relies on aren't throttled in the background.
    const NSActivityOptions opts = NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical;
    id token = [[NSProcessInfo processInfo]
        beginActivityWithOptions:opts
                          reason:@"D-Router real-time audio routing engine"];
    // Retain across this object's lifetime (this file is compiled with ARC;
    // CFBridgingRetain hands the +1 back to us as a raw pointer, balanced by
    // CFBridgingRelease in the destructor -- same pattern as AppAudioWorker).
    activityToken = (void*)CFBridgingRetain(token);
}

AppNapDisabler::~AppNapDisabler()
{
    if (activityToken == nullptr)
        return;
    id token = (id)CFBridgingRelease(activityToken);
    [[NSProcessInfo processInfo] endActivity:token];
    activityToken = nullptr;
}

} // namespace dcr
