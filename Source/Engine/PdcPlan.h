#pragma once

#include <algorithm>
#include <cstddef>
#include <unordered_map>
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
    // realigns outputs to the slowest chain *within their alignment domain*:
    //
    //     compDelay[o] = domainMaxLatency[domain[o]] - outputLatency[o]
    //
    // `domain[o]` is an arbitrary label -- the engine uses one label per hardware
    // device, so a latent plugin on one device never delays another device's
    // outputs (they share neither a clock nor a listener position; cross-device
    // alignment would only add latency).  An empty or size-mismatched `domain`
    // means one global domain (align everything to the single slowest output).
    // When `enabled` is false (PDC off) every compDelay is 0 -- nothing is
    // delayed.  Latencies are floored at 0 and clamped to `cap`; if any exceeded
    // the cap the returned plan has clamped == true.  plan.maxLatency reports the
    // slowest chain across ALL domains (status display).  Pure / deterministic
    // -> unit-tested.
    inline PdcPlan computePdcPlan (const std::vector<int>& outputLatency,
        const std::vector<int>& domain,
        bool enabled,
        int cap)
    {
        PdcPlan plan;
        plan.compDelay.assign (outputLatency.size(), 0);
        if (!enabled || outputLatency.empty())
            return plan;

        const bool perDomain = domain.size() == outputLatency.size();

        std::vector<int> lat (outputLatency.size(), 0);
        std::unordered_map<int, int> domainMax;
        for (std::size_t o = 0; o < outputLatency.size(); ++o)
        {
            int l = outputLatency[o] < 0 ? 0 : outputLatency[o];
            if (l > cap)
            {
                l = cap;
                plan.clamped = true;
            }
            lat[o] = l;
            plan.maxLatency = std::max (plan.maxLatency, l);

            const int d = perDomain ? domain[o] : 0;
            auto it = domainMax.find (d);
            if (it == domainMax.end())
                domainMax.emplace (d, l);
            else
                it->second = std::max (it->second, l);
        }

        for (std::size_t o = 0; o < outputLatency.size(); ++o)
            plan.compDelay[o] = domainMax[perDomain ? domain[o] : 0] - lat[o];

        return plan;
    }

    // Single-domain convenience (legacy call shape): align every output to the
    // one slowest chain.
    inline PdcPlan computePdcPlan (const std::vector<int>& outputLatency,
        bool enabled,
        int cap)
    {
        return computePdcPlan (outputLatency, std::vector<int> {}, enabled, cap);
    }

} // namespace dcr
