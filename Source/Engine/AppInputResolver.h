#pragma once

// JUCE-free decision logic for auto-reattaching app-audio capture sources to
// running processes.  The CoreAudio glue (process enumeration, tap creation)
// lives elsewhere and is not headless-testable; THIS file is the deterministic
// core -- given the configured sources and the processes currently running, it
// emits the attach/detach commands needed to reconcile them.  Pure, no side
// effects.  Mirrors the codebase pattern of extracting JUCE-free logic
// (ResonanceMath, PdcPlan) for the headless suite.

#include <string>
#include <vector>

namespace dcr::appinput
{
    // A configured app-capture source as the resolver sees it.  bundleId is the
    // stable persistence key (e.g. "com.google.Chrome").  attachedProcessId is the
    // CoreAudio process-object id this source is currently bound to, or 0 when the
    // source is offline (attached to no process).
    struct Source
    {
        std::string bundleId;
        int attachedProcessId = 0; // 0 == not attached
    };

    // One process the watcher observed, mapped from bundle id to its CoreAudio
    // process-object id (always > 0 for a real process).
    struct RunningProcess
    {
        std::string bundleId;
        int processId = 0;
    };

    enum class CommandType {
        Attach,
        Detach
    };

    struct Command
    {
        CommandType type;
        int sourceIndex; // index into the sources vector passed to reconcile()
        int processId; // Attach: the process to bind; Detach: 0
    };

    // Reconcile configured sources against the running-process set.  Per source:
    //   running, source offline               -> Attach
    //   running, attached to the SAME pid      -> no-op (steady state)
    //   running, attached to a DIFFERENT pid   -> Detach + Attach (app relaunched)
    //   not running, source attached           -> Detach
    //   not running, source offline            -> no-op (stays offline/waiting)
    // Commands are emitted in source order; a relaunch emits Detach before Attach.
    inline std::vector<Command> reconcile (const std::vector<Source>& sources,
        const std::vector<RunningProcess>& running)
    {
        std::vector<Command> cmds;
        for (int i = 0; i < (int) sources.size(); ++i)
        {
            const auto& s = sources[(size_t) i];

            int foundPid = 0;
            for (const auto& rp : running)
                if (rp.bundleId == s.bundleId)
                {
                    foundPid = rp.processId;
                    break;
                }

            const bool sourceAttached = (s.attachedProcessId != 0);

            if (foundPid != 0)
            {
                if (!sourceAttached)
                {
                    cmds.push_back ({ CommandType::Attach, i, foundPid });
                }
                else if (s.attachedProcessId != foundPid)
                {
                    // App relaunched with a new pid: drop the stale tap, bind the new one.
                    cmds.push_back ({ CommandType::Detach, i, 0 });
                    cmds.push_back ({ CommandType::Attach, i, foundPid });
                }
                // else attached to the same pid -> steady state, nothing to do.
            }
            else if (sourceAttached)
            {
                cmds.push_back ({ CommandType::Detach, i, 0 });
            }
        }
        return cmds;
    }
} // namespace dcr::appinput
