#include "Diagnostics/PerfMonitor.h"

#include "DSP/MultiChannelPluginHost.h"
#include "DSP/PluginHost.h"
#include "Engine/AudioEngine.h"
#include "Routing/InputGroupManager.h"
#include "Routing/OutputGroup.h"
#include "Routing/OutputGroupManager.h"

#include <algorithm>
#include <mach/mach.h>
#include <vector>

namespace dcr
{

    namespace
    {
        constexpr int kPeriodMs = 5000;
        constexpr int kSpikeWarnMs = 1500; // anything beyond period + this warns
        constexpr int kTopPlugins = 3;

        // Resident set size in MB via Mach kernel API.  cheap (one syscall).
        int getProcessRssMb()
        {
            mach_task_basic_info_data_t info;
            mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
            if (task_info (mach_task_self(),
                    MACH_TASK_BASIC_INFO,
                    (task_info_t) &info,
                    &count)
                != KERN_SUCCESS)
                return -1;
            return (int) (info.resident_size / (1024 * 1024));
        }

        struct PluginCpuEntry
        {
            juce::String label;
            float cpuFraction;
        };

        // Collect every loaded plugin's average CPU and keep the top-N by
        // cpu fraction.  Bypassed slots are excluded (their cpu is decaying).
        std::vector<PluginCpuEntry> collectTopPluginCpu (AudioEngine& engine, int n)
        {
            std::vector<PluginCpuEntry> all;

            // Per-channel single-instance hosts.
            const int nIn = engine.getRoutingMatrix().getNumInputs();
            const int nOut = engine.getRoutingMatrix().getNumOutputs();
            for (int ch = 0; ch < nIn; ++ch)
            {
                if (auto* h = engine.getInputPluginHost (ch))
                {
                    for (int slot = 0; slot < PluginHost::kNumSlots; ++slot)
                    {
                        auto* p = h->getPluginAt (slot);
                        if (p == nullptr || h->isBypassedAt (slot))
                            continue;
                        all.push_back ({ "IN ch." + juce::String (ch + 1)
                                             + "/s" + juce::String (slot + 1)
                                             + " " + p->getName(),
                            h->getCpuLoadAt (slot) });
                    }
                }
            }
            for (int ch = 0; ch < nOut; ++ch)
            {
                if (auto* h = engine.getPluginHost (ch))
                {
                    for (int slot = 0; slot < PluginHost::kNumSlots; ++slot)
                    {
                        auto* p = h->getPluginAt (slot);
                        if (p == nullptr || h->isBypassedAt (slot))
                            continue;
                        all.push_back ({ "OUT ch." + juce::String (ch + 1)
                                             + "/s" + juce::String (slot + 1)
                                             + " " + p->getName(),
                            h->getCpuLoadAt (slot) });
                    }
                }
            }

            // Group multi-channel chains.
            auto scanGroups = [&] (auto& mgr, const char* tag) {
                for (int gi = 0; gi < mgr.getNumGroups(); ++gi)
                {
                    auto* g = mgr.getGroup (gi);
                    if (g == nullptr)
                        continue;
                    for (size_t si = 0; si < g->pluginSlots.size(); ++si)
                    {
                        auto& slot = g->pluginSlots[si];
                        if (!slot)
                            continue;
                        auto* p = slot->getPlugin();
                        if (p == nullptr || slot->isBypassed())
                            continue;
                        all.push_back ({ juce::String (tag) + "GRP \"" + g->name
                                             + "\"/s" + juce::String ((int) si + 1)
                                             + " " + p->getName(),
                            slot->getCpuLoadAvg() });
                    }
                }
            };
            scanGroups (engine.getInputGroupManager(), "IN ");
            scanGroups (engine.getGroupManager(), "OUT ");

            std::partial_sort (all.begin(),
                all.begin() + std::min ((int) all.size(), n),
                all.end(),
                [] (auto& a, auto& b) { return a.cpuFraction > b.cpuFraction; });
            all.resize ((size_t) std::min ((int) all.size(), n));
            return all;
        }
    }

    PerfMonitor::PerfMonitor (AudioEngine& e) : engine (e)
    {
        lastTickMs = juce::Time::currentTimeMillis();
        lastBlocks = engine.getMatrixBlocksProcessed();
        lastStalled = engine.getMatrixBlocksStalled();
        lastXrunIn = engine.getTotalInputOverruns();
        lastXrunOut = engine.getTotalOutputUnderruns();
        startTimer (kPeriodMs);
    }

    PerfMonitor::~PerfMonitor() { stopTimer(); }

    void PerfMonitor::pause() { stopTimer(); }

    void PerfMonitor::resume()
    {
        // Reset the gap baseline so the first tick after the (expected) reconfigure
        // pause doesn't log a bogus "PERF SPIKE: message thread blocked" line for a
        // gap the pause itself created.  Engine counters (blocks/xruns) reset on
        // restart too, but emitSnapshot's delta logic already clamps a backwards
        // counter to 0, so only the time baseline needs refreshing here.
        lastTickMs = juce::Time::currentTimeMillis();
        startTimer (kPeriodMs);
    }

    void PerfMonitor::timerCallback() { emitSnapshot(); }

    void PerfMonitor::emitSnapshot()
    {
        const auto now = juce::Time::currentTimeMillis();
        const auto gap = (int) (now - lastTickMs);
        lastTickMs = now;

        // ---- Device liveness one-shot ----------------------------------------
        // Log each device's first audio callback (or warn if a device has been
        // open for >= 1 PERF tick without firing).  This catches the
        // "device opened cleanly but driver never delivers samples" silent
        // failure that otherwise just looks like 100% stalls.
        {
            auto live = engine.getDeviceLiveness();
            // Drop entries from previous engine config that aren't in the
            // current device list -- engine restart implicitly resets the map.
            for (auto it = seenFirstCallback.begin(); it != seenFirstCallback.end();)
            {
                const bool stillThere = std::any_of (live.begin(), live.end(), [&] (const auto& d) { return d.name == it->first; });
                if (!stillThere)
                    it = seenFirstCallback.erase (it);
                else
                    ++it;
            }
            for (auto& d : live)
            {
                auto& flagged = seenFirstCallback[d.name];
                if (flagged)
                    continue; // already reported for this device
                if (d.firstCallbackFired)
                {
                    juce::Logger::writeToLog ("device LIVE: '" + d.name
                                              + "' (first IO callback fired)");
                    flagged = true;
                }
                else if (gap >= 4500 && gap <= 6500)
                {
                    // Bail-and-warn after 2 ticks of silence (~10s) to avoid
                    // a false positive on the very first PERF tick after start.
                    static std::map<juce::String, int> missCount;
                    if (++missCount[d.name] >= 2)
                    {
                        juce::Logger::writeToLog ("device SILENT: '" + d.name
                                                  + "' opened but no IO callback after ~"
                                                  + juce::String (missCount[d.name] * 5)
                                                  + " seconds -- driver / SR mismatch?");
                        flagged = true;
                    }
                }
            }
        }

        // UI-thread freeze proxy: if the actual gap is meaningfully larger
        // than the requested period, the message thread spent that excess
        // time inside ONE callback -- a sync plugin call, a heavy paint,
        // a lock, etc.  Worth a separate big "SPIKE" line so it's easy to
        // grep for in the log.
        const int excess = gap - kPeriodMs;
        if (excess > kSpikeWarnMs)
            juce::Logger::writeToLog ("PERF SPIKE: message thread blocked for "
                                      + juce::String (excess) + " ms "
                                      + "(expected gap " + juce::String (kPeriodMs)
                                      + " ms, actual " + juce::String (gap) + ")");

        // Engine deltas.
        const auto blocks = engine.getMatrixBlocksProcessed();
        const auto stalled = engine.getMatrixBlocksStalled();
        const auto xrunIn = engine.getTotalInputOverruns();
        const auto xrunOut = engine.getTotalOutputUnderruns();
        const auto drops = engine.getMatrixOutputDrops();
        const auto dB = blocks > lastBlocks ? blocks - lastBlocks : 0;
        const auto dS = stalled > lastStalled ? stalled - lastStalled : 0;
        const auto dXi = xrunIn > lastXrunIn ? xrunIn - lastXrunIn : 0;
        const auto dXo = xrunOut > lastXrunOut ? xrunOut - lastXrunOut : 0;
        const auto dDr = drops > lastDrops ? drops - lastDrops : 0;
        lastBlocks = blocks;
        lastStalled = stalled;
        lastXrunIn = xrunIn;
        lastXrunOut = xrunOut;
        lastDrops = drops;

        const float cpuAvg = engine.getCpuLoadAvg() * 100.0f;
        const float cpuPeak = engine.getCpuLoadPeak() * 100.0f;
        // polls/block tells us how many wakes of the matrix thread it took per
        // real block.  Healthy ~ 1.0 + (block period / poll interval).  At
        // default 128 spl @ 48k = 2667us / 250us poll = ~10.  Anything wildly
        // above that means the matrix thread is hammering the rings while the
        // device callbacks lag -- the real "behind schedule" signal.
        const float pollsPerBlock = (dB > 0) ? (float) (dB + dS) / (float) dB : 0.0f;
        // Output ring fill is the LEADING indicator of an xrun.  We log both
        // the percentage (familiar) AND the absolute ms headroom.  The ms
        // number is the truer "time-to-xrun" because it doesn't get bigger
        // when the user shrinks the ring (which only changes capacity, not
        // current fill).  Warn at < 8 ms, crit at < 3 ms.
        const float outRingMin = engine.getMinOutputRingFillFraction() * 100.0f;
        const double outRingMinMs = engine.getMinOutputRingHeadroomMs();
        const int rssMb = getProcessRssMb();

        // Build top-plugin segment.
        juce::String topSeg;
        auto top = collectTopPluginCpu (engine, kTopPlugins);
        if (!top.empty())
        {
            topSeg << "  topPlugins=[";
            for (size_t i = 0; i < top.size(); ++i)
            {
                if (i > 0)
                    topSeg << ", ";
                topSeg << top[i].label << " " << juce::String (top[i].cpuFraction * 100.0f, 1) << "%";
            }
            topSeg << "]";
        }

        juce::String line;
        line << "PERF eng cpu=" << juce::String (cpuAvg, 1) << "%/" << juce::String (cpuPeak, 1) << "%"
             << " blocks=" << juce::String ((juce::int64) dB)
             << " polls/block=" << juce::String (pollsPerBlock, 1)
             << " outRing=" << juce::String (outRingMinMs, 1) << "ms"
             << "(" << juce::String (outRingMin, 0) << "%)"
             << " xrun=" << juce::String ((juce::int64) dXi) << "/" << juce::String ((juce::int64) dXo)
             << " drop=" << juce::String ((juce::int64) dDr);
        if (rssMb >= 0)
            line << " rss=" << juce::String (rssMb) << "MB";
        line << topSeg;
        juce::Logger::writeToLog (line);

        // Loud explicit warning only if absolute headroom is dangerously low.
        // 5 ms is below the duration of a single device callback at typical
        // 256-sample @ 48k = 5.3 ms, so anything under 5 ms means the next
        // callback will likely underrun.
        if (outRingMinMs < 5.0 && dB > 0)
            juce::Logger::writeToLog ("PERF WARN: output ring headroom "
                                      + juce::String (outRingMinMs, 1) + " ms -- xrun imminent");
    }

} // namespace dcr
