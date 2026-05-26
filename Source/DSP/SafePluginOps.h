#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace dcr::safe {

// Exception-safe wrappers around the plugin-state APIs.  Both
// AudioPluginInstance::getStateInformation and setStateInformation can throw
// NSException (any Cocoa interaction inside the AU bridge) or std::exception
// (rare).  Calling these from a .cpp file (without @try/@catch) would let
// the exception unwind through C++ frames and terminate the process.
//
// The implementation lives in SafePluginOps.mm so non-ObjC translation units
// can call these helpers without having to be .mm themselves.
//
// Returns true on success, false if anything threw.  On failure the
// destination MemoryBlock is left empty / unchanged.

bool getStateInformation (juce::AudioPluginInstance& p, juce::MemoryBlock& dest) noexcept;
bool setStateInformation (juce::AudioPluginInstance& p, const void* data, int sizeInBytes) noexcept;

// Same idea for getPluginDescription / createXml / loadFromXml -- snapshot
// save/restore touches these and any of them can NSException.
bool getPluginDescriptionXml (juce::AudioPluginInstance& p, juce::String& destXml) noexcept;
bool loadPluginDescriptionFromXml (const juce::String& xml, juce::PluginDescription& destDesc) noexcept;

} // namespace dcr::safe
