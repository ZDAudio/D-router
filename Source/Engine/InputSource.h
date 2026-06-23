#pragma once

namespace dcr
{
    class FloatRingBuffer;

    // The minimal surface the matrix needs from a global input.  Both DeviceWorker
    // (hardware) and AppAudioWorker (process tap) implement it, so MatrixProcessor
    // consumes them uniformly.  isAppInput()/isAttached() drive the never-stall rule
    // (see MatrixInputPlan.h): app inputs contribute silence when detached/underfull
    // instead of stalling the whole matrix.
    class InputSource
    {
    public:
        virtual ~InputSource() = default;
        virtual FloatRingBuffer* getInputRing (int ch) noexcept = 0;
        virtual bool isAppInput() const noexcept = 0;
        virtual bool isAttached() const noexcept = 0;
    };
} // namespace dcr
