#pragma once

namespace dcr
{

    // RAII guard that, while alive, keeps macOS "App Nap" from suspending or
    // throttling this process when it is in the background.
    //
    // D-Router deliberately keeps its audio engine and message-thread timers
    // running while the window is hidden to the menu bar (see Main.cpp) -- so the
    // user can route audio while working in another app.  App Nap defeats that:
    // once we're backgrounded and occluded, macOS throttles our timers and run
    // loop.  Field logs showed the message thread stalled for minutes at a time
    // (PERF SPIKE ~295 s) while the user was in a call app, freezing the engine
    // watchdog / metrics.
    //
    // Holds an NSProcessInfo activity with UserInitiated + LatencyCritical (the
    // latter marks this as real-time audio work so high-resolution timers are
    // honoured).  It does NOT disable idle system sleep -- the Mac may still
    // sleep normally when the user walks away.  No-op on non-Apple builds.
    class AppNapDisabler
    {
    public:
        AppNapDisabler();
        ~AppNapDisabler();

        AppNapDisabler (const AppNapDisabler&) = delete;
        AppNapDisabler& operator= (const AppNapDisabler&) = delete;

        // True if an activity token was actually acquired (Apple platforms only).
        bool isActive() const noexcept { return activityToken != nullptr; }

    private:
        void* activityToken = nullptr; // retained id (NSProcessInfo activity token)
    };

} // namespace dcr
