#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <cstdint>
#include <map>

namespace dcr {

class AudioEngine;

// Periodic performance probe.  Emits one structured PERF line every
// ~5 seconds into the session log.  Cheap (one timer + a handful of
// atomic reads).  Three things it watches for, all reported in a way
// you can grep for in the log later when chasing a sluggish session:
//
//   1. Engine health.
//        PERF eng cpu=12.3%/61.0% blocks=14994 stalled=0(0.00%) ...
//      cpuAvg / peak from MatrixProcessor; deltas vs the previous tick
//      so the ratios reflect THIS interval, not lifetime cumulative.
//
//   2. Top plugin CPU consumers.
//        ... topPlugins: [ProQ3:43.2%, AUCompressor:8.1%, ...]
//      Reads per-slot cpuLoadAvg from every PluginHost / MultiChannel-
//      PluginHost and surfaces the worst 3.
//
//   3. UI-thread blockage.
//        PERF SPIKE message thread blocked for 740 ms (expected 5000)
//      The timer measures the actual gap between fires; if it's
//      meaningfully larger than the requested period, the message
//      thread spent that time inside a single callback (e.g. a slow
//      paint, a sync plugin-state read, a lock).
//
//   4. Process memory.
//        ... rss=312MB
//      Pulled from mach_task_self() so we can spot leaks across a long
//      session.
class PerfMonitor : private juce::Timer
{
public:
    explicit PerfMonitor (AudioEngine& engine);
    ~PerfMonitor() override;

private:
    void timerCallback() override;
    void emitSnapshot();

    AudioEngine& engine;

    // Delta tracking so the logged ratios reflect this interval only.
    uint64_t lastBlocks         = 0;
    uint64_t lastStalled        = 0;
    uint64_t lastXrunIn         = 0;
    uint64_t lastXrunOut        = 0;
    juce::int64 lastTickMs      = 0;

    // Per-device flag: have we already logged the "first callback fired"
    // (or the "never fired" warning) for this device name?  Resets on
    // engine restart implicitly because the device list changes.
    std::map<juce::String, bool> seenFirstCallback;
};

} // namespace dcr
