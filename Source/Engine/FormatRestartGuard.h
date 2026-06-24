#pragma once

namespace dcr
{

    // Defense-in-depth for the device-format-change watchdog.
    //
    // With rate-following open() (see DeviceRateChoice.h) an OS-driven sample
    // rate change converges in a single preserve-state restart, so the engine no
    // longer fights another app for a shared device.  This guard is the backstop
    // for the remaining failure mode: a *pathological* device (or driver) that
    // flaps its nominal rate on its own.  It rate-limits restarts so the watchdog
    // can never spin the engine.
    //
    // Pure / JUCE-free: the caller supplies a monotonic millisecond clock, so it
    // is deterministically unit-testable (see CoreLogicTests).
    struct FormatRestartGuard
    {
        // Tunables (milliseconds / counts).
        double minIntervalMs = 1200.0; // debounce: coalesce a burst of changes
        double windowMs = 10000.0; // rolling window for the burst cap
        int maxInWindow = 4; // restarts allowed within one window
        double cooldownMs = 30000.0; // quiet spell that forgets all history

        // State.
        double windowStartMs = -1.0;
        double lastRestartMs = -1.0;
        int countInWindow = 0;
        bool backedOff = false;

        // Call when a format change is pending and the engine is idle.  Returns
        // true if a restart should be issued now, false to skip this tick.
        bool allowRestart (double nowMs)
        {
            // Self-heal: a long quiet spell forgets past flapping so a later,
            // unrelated change (e.g. the call ending) is handled normally.
            if (lastRestartMs >= 0.0 && (nowMs - lastRestartMs) > cooldownMs)
            {
                windowStartMs = -1.0;
                countInWindow = 0;
                backedOff = false;
            }

            if (backedOff)
                return false;

            if (lastRestartMs >= 0.0 && (nowMs - lastRestartMs) < minIntervalMs)
                return false; // debounce

            if (windowStartMs < 0.0 || (nowMs - windowStartMs) > windowMs)
            {
                windowStartMs = nowMs; // open a fresh window
                countInWindow = 0;
            }

            if (countInWindow >= maxInWindow)
            {
                backedOff = true; // flapping — stop fighting until it settles
                return false;
            }

            ++countInWindow;
            lastRestartMs = nowMs;
            return true;
        }

        // True once we've given up due to flapping (until cooldown / reset()).
        bool isBackedOff() const { return backedOff; }

        // Re-arm (e.g. on a clean user-initiated reconfigure or a manual Reset).
        void reset()
        {
            windowStartMs = -1.0;
            lastRestartMs = -1.0;
            countInWindow = 0;
            backedOff = false;
        }
    };

} // namespace dcr
