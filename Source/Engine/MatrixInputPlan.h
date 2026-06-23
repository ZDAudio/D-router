#pragma once

// JUCE-free rule for how the matrix treats one input channel each block.
//
// Hardware inputs GATE the matrix: if a hardware ring hasn't accumulated a full
// block yet, the matrix waits (stalls) -- it must not run ahead of real capture.
// App-audio inputs must NEVER stall the matrix: an app that is offline (its tap
// detached) or still warming up would otherwise freeze ALL audio.  An app input
// that isn't ready contributes silence for the block instead.
//
// Pure decision; MatrixProcessor calls it per input when deciding availability
// and reads (wiring is Part 3).  When `stalls` is true the caller bails the whole
// block and `action` is irrelevant.

namespace dcr
{
    enum class MatrixInputAction {
        Read, // read blockSize samples from this input's ring
        Silence // memset this input's block to zero
    };

    struct MatrixInputPlan
    {
        MatrixInputAction action;
        bool stalls; // true -> this input may hold up the whole block (hardware only)
    };

    // isAppInput : true for an app-audio capture input, false for a hardware input.
    // attached   : (app inputs) is a tap currently bound and producing?  ignored for hardware.
    // available  : samples currently readable in this input's ring.
    // blockSize  : the engine block the matrix wants to read.
    inline MatrixInputPlan planMatrixInput (bool isAppInput, bool attached, int available, int blockSize)
    {
        if (!isAppInput)
        {
            // Hardware: read when a full block is ready, otherwise stall the matrix.
            if (available >= blockSize)
                return { MatrixInputAction::Read, false };
            return { MatrixInputAction::Read, true }; // stall: action ignored by caller
        }

        // App: never stall.  Read only when attached AND a full block is ready.
        if (attached && available >= blockSize)
            return { MatrixInputAction::Read, false };
        return { MatrixInputAction::Silence, false };
    }
} // namespace dcr
