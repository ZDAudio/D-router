#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace dcr
{

    // Default ceiling for a single output's compensable plugin latency, in engine
    // samples (1 s @ 48 kHz).  Delay-line storage is pre-sized to this, so it is a
    // hard cap, not a soft target.  Far beyond any realistic insert chain (the
    // built-in spectral plugins top out near 2048 samples); a plan that exceeds it
    // is clamped AND flagged so the engine can log it -- never a silent
    // mis-alignment.  Pure-logic so the test target can include this header.
    inline constexpr int kMaxPdcSamples = 48000;

    struct PdcPlan
    {
        std::vector<int> compDelay; // per output: samples to delay it by
        int maxLatency = 0; // the latency every output is aligned to
        bool clamped = false; // an output's latency exceeded the cap
    };

    // Given each output's total plugin latency (per-output inserts + its group
    // insert, engine samples), produce the per-output compensation delay that
    // realigns every output to the slowest one:
    //
    //     compDelay[o] = maxLatency - outputLatency[o]
    //
    // so that outputLatency[o] + compDelay[o] == maxLatency for every output.
    // When `enabled` is false (PDC off) every compDelay is 0 -- nothing is delayed.
    // Latencies are floored at 0 and clamped to `cap`; if any exceeded the cap the
    // returned plan has clamped == true.  Pure / deterministic -> unit-tested.
    inline PdcPlan computePdcPlan (const std::vector<int>& outputLatency,
        bool enabled,
        int cap)
    {
        PdcPlan plan;
        plan.compDelay.assign (outputLatency.size(), 0);
        if (!enabled || outputLatency.empty())
            return plan;

        auto clampLat = [&plan, cap] (int raw) -> int {
            int l = raw < 0 ? 0 : raw;
            if (l > cap)
            {
                l = cap;
                plan.clamped = true;
            }
            return l;
        };

        int maxLat = 0;
        for (int raw : outputLatency)
            maxLat = std::max (maxLat, clampLat (raw));
        plan.maxLatency = maxLat;

        for (std::size_t o = 0; o < outputLatency.size(); ++o)
            plan.compDelay[o] = maxLat - clampLat (outputLatency[o]);

        return plan;
    }

} // namespace dcr
