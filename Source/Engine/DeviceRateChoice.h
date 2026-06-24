#pragma once

#include <cmath>
#include <vector>

namespace dcr
{

    // Choose the nominal sample rate to open a hardware device at.
    //
    // The engine used to *force* every device to the engine rate on open.  That
    // makes us a bad citizen on a SHARED device: when another app (e.g. a
    // voice/communication app starting a call) sets the device to its own rate,
    // forcing it back triggers a tug-of-war — the OS flips the rate, our
    // format-change watchdog restarts us, open() forces it back, the other app
    // flips it again — a restart loop that breaks the other app's stream and
    // thrashes CoreAudio.
    //
    // Fix: ADOPT the device's current nominal rate rather than fighting for it.
    // The engine still runs internally at engineRate; the per-channel SRC bridges
    // the difference (device rate <-> engine rate).  We only fall back to
    // engineRate when the device didn't report a usable current rate.
    //
    // `available` is the device's supported-rate list (may be empty).  Pure /
    // JUCE-free so it is unit-tested headless (see CoreLogicTests).
    inline double chooseDeviceSampleRate (const std::vector<double>& available,
        double engineRate,
        double currentDeviceRate)
    {
        // Prefer what the device is already set to (coexist); fall back to the
        // engine rate only if the device gave us nothing usable.
        const double target = (currentDeviceRate > 0.0) ? currentDeviceRate : engineRate;

        if (available.empty())
            return target;

        // An exact (within 1 Hz) supported match wins — no SRC-ratio surprises.
        for (double r : available)
            if (std::abs (r - target) < 1.0)
                return r;

        // Otherwise the closest supported rate to the target.
        double best = available.front();
        for (double r : available)
            if (std::abs (r - target) < std::abs (best - target))
                best = r;
        return best;
    }

} // namespace dcr
