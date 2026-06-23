#include "Engine/AppAudioProcesses.h"

#import <AppKit/AppKit.h> // NSRunningApplication, NSWorkspace
#import <Foundation/Foundation.h>

namespace
{
// Read an array of AudioObjectIDs from a global hardware property.
std::vector<AudioObjectID> readObjectList(AudioObjectPropertySelector sel)
{
    AudioObjectPropertyAddress addr{
        sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) != noErr)
        return {};
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
    if (ids.empty())
        return {};
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, ids.data()) != noErr)
        return {};
    return ids;
}

int readPid(AudioObjectID proc)
{
    AudioObjectPropertyAddress addr{
        kAudioProcessPropertyPID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    pid_t pid = -1;
    UInt32 size = sizeof(pid);
    AudioObjectGetPropertyData(proc, &addr, 0, nullptr, &size, &pid);
    return (int)pid;
}

std::string cfToString(CFStringRef cf)
{
    if (cf == nullptr)
        return {};
    if (const char* c = CFStringGetCStringPtr(cf, kCFStringEncodingUTF8))
        return c;
    char buf[512] = {0};
    if (CFStringGetCString(cf, buf, sizeof(buf), kCFStringEncodingUTF8))
        return buf;
    return {};
}

std::string readBundleId(AudioObjectID proc)
{
    AudioObjectPropertyAddress addr{
        kAudioProcessPropertyBundleID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFStringRef cf = nullptr;
    UInt32 size = sizeof(cf);
    if (AudioObjectGetPropertyData(proc, &addr, 0, nullptr, &size, &cf) != noErr || cf == nullptr)
        return {};
    std::string out = cfToString(cf);
    CFRelease(cf); // CoreAudio hands back a +1 CFString (Create rule)
    return out;
}

bool readRunningOutput(AudioObjectID proc)
{
    AudioObjectPropertyAddress addr{
        kAudioProcessPropertyIsRunningOutput, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 v = 0, size = sizeof(v);
    AudioObjectGetPropertyData(proc, &addr, 0, nullptr, &size, &v);
    return v != 0;
}
} // namespace

namespace dcr
{
AppAudioProcesses::AppAudioProcesses()
{
    NSNotificationCenter* nc = [[NSWorkspace sharedWorkspace] notificationCenter];
    AppAudioProcesses* self = this;
    auto handler = ^(NSNotification*) {
      if (self->onProcessesChanged)
          self->onProcessesChanged();
    };
    id tokenLaunch = [nc addObserverForName:NSWorkspaceDidLaunchApplicationNotification
                                     object:nil
                                      queue:[NSOperationQueue mainQueue]
                                 usingBlock:handler];
    id tokenQuit = [nc addObserverForName:NSWorkspaceDidTerminateApplicationNotification
                                   object:nil
                                    queue:[NSOperationQueue mainQueue]
                               usingBlock:handler];
    observer = (void*)CFBridgingRetain(@[ tokenLaunch, tokenQuit ]);
}

AppAudioProcesses::~AppAudioProcesses()
{
    if (observer != nullptr)
    {
        NSArray* tokens = (__bridge_transfer NSArray*)observer;
        NSNotificationCenter* nc = [[NSWorkspace sharedWorkspace] notificationCenter];
        for (id t in tokens)
            [nc removeObserver:t];
        observer = nullptr;
    }
}

std::vector<AppAudioProcesses::Entry> AppAudioProcesses::enumerate() const
{
    std::vector<Entry> out;
    for (AudioObjectID proc : readObjectList(kAudioHardwarePropertyProcessObjectList))
    {
        Entry e;
        e.processObject = proc;
        e.pid = readPid(proc);
        e.bundleId = readBundleId(proc);
        e.runningOutput = readRunningOutput(proc);

        if (e.pid > 0)
            if (NSRunningApplication* app =
                    [NSRunningApplication runningApplicationWithProcessIdentifier:e.pid])
                if (NSString* n = app.localizedName)
                    e.displayName = n.UTF8String;

        if (!e.bundleId.empty())
            out.push_back(std::move(e));
    }
    return out;
}

AudioObjectID AppAudioProcesses::resolve(const std::string& bundleId) const
{
    for (const auto& e : enumerate())
        if (e.bundleId == bundleId)
            return e.processObject;
    return kAudioObjectUnknown;
}
} // namespace dcr
