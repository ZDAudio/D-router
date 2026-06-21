#include "ReconfigurationController.h"

namespace dcr
{

    ReconfigurationController::Phase ReconfigurationController::successorOf (Phase p) noexcept
    {
        switch (p)
        {
            case Phase::Idle:
                return Phase::Draining;
            case Phase::Draining:
                return Phase::Rebuilding;
            case Phase::Rebuilding:
                return Phase::RestoringMatrix;
            case Phase::RestoringMatrix:
                return Phase::RestoringPlugins;
            case Phase::RestoringPlugins:
                return Phase::Running;
            case Phase::Running:
                return Phase::Running; // terminal -- no further forward step
        }
        return Phase::Running;
    }

    bool ReconfigurationController::tryBegin() noexcept
    {
        Phase expected = Phase::Idle;
        return phase_.compare_exchange_strong (expected, Phase::Draining, std::memory_order_acq_rel);
    }

    bool ReconfigurationController::advance (Phase next) noexcept
    {
        const Phase cur = phase_.load (std::memory_order_acquire);
        // Idle is reached only via finish(); Running is terminal -- neither has
        // a legal forward advance().
        if (cur == Phase::Idle || cur == Phase::Running || next != successorOf (cur))
            return false;
        phase_.store (next, std::memory_order_release);
        return true;
    }

    void ReconfigurationController::finish() noexcept
    {
        phase_.store (Phase::Idle, std::memory_order_release);
    }

}
