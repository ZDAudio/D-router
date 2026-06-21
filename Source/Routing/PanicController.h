#pragma once

#include <vector>

namespace dcr
{

    class RoutingMatrix;

    // Explicit owner of the PANIC feature's state (Phase C1).  Before this, the
    // panic flag and the saved pre-panic mute vectors lived loose in
    // MainComponent and were written from five sites; concentrating them here
    // gives the feature one writer and makes its transitions testable headless.
    //
    // Message-thread only (mirrors the RoutingMatrix UI-setter contract); no
    // locks, no allocation beyond the saved-state vectors.
    class PanicController
    {
    public:
        enum class State {
            Inactive,
            Active
        };

        State state() const noexcept { return state_; }
        bool isActive() const noexcept { return state_ == State::Active; }

        // First press: snapshot every channel's current mute, then mute all of
        // them.  No-op (stays Inactive, returns false) on an empty matrix.
        // Returns true iff it transitioned to Active.
        bool engage (RoutingMatrix& m);

        // Second press: restore the snapshot, drop it, return to Inactive.
        // Safe no-op when already Inactive.
        void release (RoutingMatrix& m);

        // The user manually toggled a mute while panic was active, so the saved
        // pre-panic snapshot is now stale: forget it and return to Inactive.
        // The matrix is intentionally left as the user just set it.  No-op when
        // Inactive.
        void noteUserMuteChanged() noexcept;

        // The matrix is about to be rebuilt (device reconfigure): the saved
        // indices would point at the old layout, so drop all panic state
        // WITHOUT touching the matrix.  Returns true iff anything was cleared
        // (so the caller can refresh the button only when needed).
        bool reset() noexcept;

    private:
        State state_ = State::Inactive;
        std::vector<unsigned char> savedInputMutes;
        std::vector<unsigned char> savedOutputMutes;
    };

}
