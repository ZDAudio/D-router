#pragma once

#include <atomic>

namespace dcr
{

    // Explicit lifecycle for a device reconfigure / snapshot apply (Phase C3).
    //
    // Replaces MainComponent's bare `std::atomic<bool> isReconfiguring` with a
    // named, ordered phase machine: there is one owner of "are we
    // reconfiguring", the stages are nameable, and an illegal/out-of-order
    // transition is rejected (and assertable) instead of silently corrupting
    // state.  The actual engine + worker-thread work stays in the caller; it
    // drives this machine through the phases at the sync points it already
    // serialises.
    //
    // Thread model (unchanged from the bool it replaces): `tryBegin`, `active`
    // and `phase` are atomic and may be read from any thread -- a CoreAudio
    // device-hotplug callback checks `active()` before kicking a reconfigure.
    // The phase is advanced on the message thread (claim, restore tail) and the
    // worker thread (drain, rebuild); those points are already ordered by the
    // existing callAsync hand-off, so there is a single writer at a time.
    class ReconfigurationController
    {
    public:
        // The forward-only stages of one reconfigure.  RestoringPlugins marks
        // that async plugin re-instantiation has been kicked off; the loads
        // themselves continue past `finish()` (they are aliveToken-guarded).
        enum class Phase {
            Idle,
            Draining, // fade out + stopProcessor (worker)
            Rebuilding, // engine.stop()/start() rebuilds devices (worker)
            RestoringMatrix, // restore gains/mutes/solo/crosspoints (message)
            RestoringPlugins, // kick async plugin restore (message)
            Running // reconfigure tail complete
        };

        // Claim the machine: Idle -> Draining.  Returns true on success; false
        // if a reconfigure is already in flight (mirrors the old
        // `isReconfiguring.exchange(true)` re-entry guard -- caller backs off).
        bool tryBegin() noexcept;

        // Move to `next` iff it is the immediate forward successor of the
        // current phase.  Returns false (and leaves the phase unchanged) on any
        // out-of-order request, so callers/tests can detect a broken sequence.
        bool advance (Phase next) noexcept;

        // End the reconfigure (success or abort): any phase -> Idle.
        void finish() noexcept;

        Phase phase() const noexcept { return phase_.load (std::memory_order_acquire); }
        bool active() const noexcept { return phase() != Phase::Idle; }

    private:
        static Phase successorOf (Phase p) noexcept;

        std::atomic<Phase> phase_ { Phase::Idle };
    };

}
