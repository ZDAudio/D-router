#pragma once

#include <algorithm>
#include <cmath>

// ===========================================================================
// Auto ring-buffer sizing -- JUCE-free deterministic math, unit-tested headless.
//
// Derives a device's SPSC ring sizes (+ output pre-fill) from the hardware
// latency THAT device reports, instead of the fixed user multipliers.  It stays
// inside the vetted Safe..Safest preset envelope:
//   - the "Safe" preset is the FLOOR (so auto is never riskier than the current
//     default), and
//   - extra headroom is added in proportion to how far the device's hardware
//     latency exceeds one buffer, capped at the "Safest" preset.
// Mirrors DeviceWorker's own formula (max of engine-block and device-buffer
// branches) so an auto size composes with the rest of the pipeline unchanged.
// ===========================================================================
namespace dcr::ringauto
{

    struct RingPlan
    {
        int inRingSamples = 0; // engine-rate samples / channel (pre pow2-round + cap)
        int outRingSamples = 0;
        int prefillBlocks = 8; // engine blocks of output silence pre-filled at open
    };

    //   engineBlock        : engine block size (samples)
    //   engineSr / devSr   : engine + device sample rates (Hz)
    //   devBuf             : device buffer size (samples @ devSr)
    //   hwInLatDev / hwOutLatDev : device hardware in/out latency (samples @ devSr)
    inline RingPlan computeAutoRingPlan (int engineBlock, double engineSr, int devBuf, double devSr, int hwInLatDev, int hwOutLatDev) noexcept
    {
        const int blk = std::max (1, engineBlock);
        const int db = std::max (1, devBuf);
        const double ratio = (devSr > 0.0 ? engineSr / devSr : 1.0); // engine spl per device spl
        const double devBufEng = (double) db * ratio;

        const auto iround = [] (double x) { return (int) std::lround (x); };
        const auto clampi = [] (int v, int lo, int hi) { return std::max (lo, std::min (hi, v)); };

        // Hardware latency beyond one device buffer, in buffer units.
        const double inSlack = std::max (0.0, (double) hwInLatDev / db);
        const double outSlack = std::max (0.0, (double) hwOutLatDev / db);

        // Effective multipliers, clamped to the [Safe, Safest] envelope:
        //   in:  Eng 3..4,  Dev 4..6      out: Eng 6..12, Dev 8..12
        const int inEng = clampi (3 + iround (inSlack * 0.5), 3, 4);
        const int inDev = clampi (4 + iround (inSlack), 4, 6);
        const int outEng = clampi (6 + iround (outSlack), 6, 12);
        const int outDev = clampi (8 + iround (outSlack), 8, 12);

        RingPlan p;
        p.inRingSamples = std::max (inEng * blk, (int) std::ceil (inDev * devBufEng));
        p.outRingSamples = std::max (outEng * blk, (int) std::ceil (outDev * devBufEng));

        // Pre-fill enough silence to cover the device's output latency, with the
        // Safe baseline (8 blocks) as a floor and 32 as a sane ceiling.
        const double outLatEng = (double) hwOutLatDev * ratio;
        p.prefillBlocks = clampi (std::max (8, (int) std::ceil (outLatEng / blk) + 6), 8, 32);
        return p;
    }

} // namespace dcr::ringauto
