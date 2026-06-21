// Phase-machine tests for ReconfigurationController (Phase C3).  These live in
// the JUCE-linked target because the controller now owns the reconfigure
// payload (PendingSnapshotApply / PendingPluginLoad), which carry JUCE types --
// the phase logic itself is still pure.
#include "Engine/ReconfigurationController.h"

#include <juce_core/juce_core.h>

using dcr::ReconfigurationController;
using Phase = dcr::ReconfigurationController::Phase;

struct ReconfigurationControllerTests : juce::UnitTest
{
    ReconfigurationControllerTests() : juce::UnitTest ("ReconfigurationController") {}

    void runTest() override
    {
        beginTest ("tryBegin claims once and rejects re-entry");
        {
            ReconfigurationController rc;
            expect (!rc.active());
            expect (rc.phase() == Phase::Idle);
            expect (rc.tryBegin());
            expect (rc.active());
            expect (rc.phase() == Phase::Draining);
            expect (!rc.tryBegin()); // already in flight
            expect (rc.phase() == Phase::Draining);
        }

        beginTest ("advance walks the phases in order; Running is terminal");
        {
            ReconfigurationController rc;
            expect (rc.tryBegin());
            expect (rc.advance (Phase::Rebuilding));
            expect (rc.phase() == Phase::Rebuilding);
            expect (rc.advance (Phase::RestoringMatrix));
            expect (rc.advance (Phase::RestoringPlugins));
            expect (rc.advance (Phase::Running));
            expect (rc.phase() == Phase::Running);
            expect (!rc.advance (Phase::Running)); // terminal
        }

        beginTest ("advance rejects out-of-order and from-Idle requests");
        {
            ReconfigurationController rc;
            expect (rc.tryBegin()); // Draining
            expect (!rc.advance (Phase::RestoringMatrix)); // skipped Rebuilding
            expect (rc.phase() == Phase::Draining);

            ReconfigurationController idle;
            expect (!idle.advance (Phase::Draining));
            expect (idle.phase() == Phase::Idle);
        }

        beginTest ("finish resets to Idle and the controller is reclaimable");
        {
            ReconfigurationController rc;
            expect (rc.tryBegin());
            expect (rc.advance (Phase::Rebuilding));
            rc.finish();
            expect (!rc.active());
            expect (rc.phase() == Phase::Idle);
            expect (rc.tryBegin()); // reusable for the next reconfigure
            expect (rc.phase() == Phase::Draining);
        }

        beginTest ("payload is default-empty and survives across phase changes");
        {
            ReconfigurationController rc;
            expect (!rc.snapshot().valid);
            expect (rc.pluginQueue().empty());
            expect (rc.pluginCursor() == 0);

            rc.snapshot().valid = true;
            rc.pluginCursor() = 3;
            expect (rc.tryBegin());
            expect (rc.advance (Phase::Rebuilding));
            // The payload is independent of the phase machine.
            expect (rc.snapshot().valid);
            expect (rc.pluginCursor() == 3);
        }
    }
};

static ReconfigurationControllerTests reconfigurationControllerTests;
