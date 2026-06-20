#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace dcr {

// Single-channel integer-sample delay with a glitchless crossfade whenever the
// delay amount changes.  PDC uses one per output channel to realign the
// un-plugged outputs behind the slowest plugin.
//
// Lifecycle / threading:
//   - prepare() allocates to the largest delay you will ever request.  Call it
//     off the audio thread (engine configure()).
//   - process() and setTargetDelay() run on the audio thread ONLY -- no alloc,
//     no locks.  This class is NOT thread-safe; the message thread hands the
//     resolved target in through an atomic the *caller* owns (see
//     MatrixProcessor), never by touching this object directly.
//
// A delay change (plugin load/bypass, PDC enable/disable) ramps over a short
// linear crossfade between the old tap and the new tap so it doesn't click.
// Both taps come from the same history buffer, so during the (rare, ~ms) ramp
// you get a brief comb between two delays of the same signal -- far less
// objectionable than a click or a silence gap.  Re-targets are level-triggered:
// if the target moves again mid-ramp, the next block picks it up once the
// current ramp settles.
class PdcDelayLine
{
public:
    // maxDelaySamples: largest delay ever requested (storage ceiling).
    // blockSizeSamples: max samples per process() call (kept for callers).
    // rampSamples: crossfade length when the delay changes.
    void prepare (int maxDelaySamples, int blockSizeSamples, int rampSamples = 256)
    {
        maxDelay  = std::max (0, maxDelaySamples);
        blockSize = std::max (1, blockSizeSamples);
        rampLen   = std::max (1, rampSamples);
        cap       = maxDelay + 1;                 // holds taps 0..maxDelay
        buf.assign ((std::size_t) cap, 0.0f);
        writePos = 0;
        settled  = target = 0;
        ramping  = false;
        rampFrom = rampTo = rampPos = 0;
    }

    // Clear history to silence, keep capacity + the requested delay.  No ramp.
    void reset()
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        writePos = 0;
        settled  = target;
        ramping  = false;
        rampPos  = 0;
    }

    // Request a new delay (clamped to [0, maxDelay]).  Cheap; safe to call every
    // block with an unchanged value (no-op once settled).
    void setTargetDelay (int samples) noexcept
    {
        target = std::min (std::max (0, samples), maxDelay);
    }

    // Delay currently being produced (the ramp destination while ramping).
    int getCurrentDelay() const noexcept { return ramping ? rampTo : settled; }
    int getMaxDelay()     const noexcept { return maxDelay; }
    bool isRamping()      const noexcept { return ramping; }

    // Delay `n` samples of `data` in place.  RT-safe once prepared.
    void process (float* data, int n) noexcept
    {
        if (cap <= 0) return;

        // Fast path: fully idle (zero delay, nothing pending).  Keep the
        // history current with a bulk copy so a *future* delay still ramps in
        // cleanly, but leave the audio untouched.  This is the common case when
        // PDC is off or no plugin reports latency, so it must stay near-free.
        if (! ramping && target == 0 && settled == 0)
        {
            writeHistory (data, n);
            return;
        }

        // Begin a crossfade if the target moved and we're not already ramping.
        if (! ramping && target != settled)
        {
            ramping  = true;
            rampFrom = settled;
            rampTo   = target;
            rampPos  = 0;
        }

        for (int i = 0; i < n; ++i)
        {
            buf[(std::size_t) writePos] = data[i];   // write before reading: tap 0 == passthrough

            float out;
            if (ramping)
            {
                const float g = (float) rampPos / (float) rampLen;
                out = (1.0f - g) * readDelayed (rampFrom) + g * readDelayed (rampTo);
                if (++rampPos >= rampLen)
                {
                    ramping = false;
                    settled = rampTo;                // a target that moved again is caught next block
                }
            }
            else
            {
                out = readDelayed (settled);
            }

            data[i] = out;
            if (++writePos >= cap) writePos = 0;
        }
    }

private:
    // Sample written `d` steps ago (d == 0 is the one just written this iter).
    float readDelayed (int d) const noexcept
    {
        int idx = writePos - d;
        if (idx < 0) idx += cap;
        return buf[(std::size_t) idx];
    }

    // Bulk-write `n` samples into the circular history, advancing writePos
    // exactly as the per-sample loop would, so the fast and slow paths
    // interleave seamlessly.  Used by the idle fast path.
    void writeHistory (const float* data, int n) noexcept
    {
        int pos = writePos, remaining = n;
        const float* src = data;
        while (remaining > 0)
        {
            const int chunk = std::min (remaining, cap - pos);
            std::copy (src, src + chunk, buf.begin() + pos);
            pos += chunk; if (pos >= cap) pos -= cap;
            src += chunk; remaining -= chunk;
        }
        writePos = pos;
    }

    std::vector<float> buf;            // circular history, size = maxDelay + 1
    int  cap       = 0;
    int  writePos  = 0;
    int  maxDelay  = 0;
    int  blockSize = 0;

    int  settled   = 0;               // delay produced when not ramping
    int  target    = 0;               // requested delay

    bool ramping   = false;
    int  rampFrom  = 0;
    int  rampTo    = 0;
    int  rampPos   = 0;
    int  rampLen   = 256;
};

} // namespace dcr
