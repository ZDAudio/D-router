#pragma once

#include <cmath>

namespace dcr
{

    // A single eased scalar: `current` chases `target` by a fixed fraction each
    // step (exponential ease-out), snapping exactly onto target once it's within
    // `eps`.  This is the rail-collapse math (`x += diff * 0.30`) extracted so
    // every UI animation -- rail width, the active-tab indicator, panel fades,
    // hover glow -- shares one deterministic, unit-tested pattern.
    //
    // JUCE-free on purpose (lives in the pure-logic test net).  All use is on the
    // message thread; there is no thread-safety here and none is needed.
    struct Eased
    {
        double current = 0.0;
        double target = 0.0;

        void snap (double v) { current = target = v; }
        void to (double v) { target = v; }
        bool atRest() const { return current == target; }

        // Advance one frame.  Returns true while still moving (caller keeps the
        // driving timer alive), false once snapped onto target.  `factor` is the
        // per-frame approach fraction (0.30 ~= a 10-frame / ~167 ms ease @ 60 Hz);
        // `eps` is the snap threshold so we land exactly and the timer can stop.
        bool step (double factor = 0.30, double eps = 0.5)
        {
            const double diff = target - current;
            if (std::abs (diff) <= eps)
            {
                current = target;
                return false;
            }
            current += diff * factor;
            return true;
        }
    };

} // namespace dcr
