#pragma once

#include <atomic>
#include <vector>

namespace dcr
{

    // 3-layer gain matrix: inputTrim[n] * crosspoint[m][n] * outputTrim[m].
    // Setter side: any thread (UI). Getter side: matrix-processing thread.
    // Smoothing is applied per-block by the consumer using takeSnapshot().
    class RoutingMatrix
    {
    public:
        void resize (int numInputs, int numOutputs);

        int getNumInputs() const noexcept { return numIns; }
        int getNumOutputs() const noexcept { return numOuts; }

        // Linear gain (1.0 == unity).
        void setInputTrim (int input, float gain) noexcept;
        void setOutputTrim (int output, float gain) noexcept;
        void setCrosspoint (int output, int input, float gain) noexcept;

        float getInputTrim (int input) const noexcept;
        float getOutputTrim (int output) const noexcept;
        float getCrosspoint (int output, int input) const noexcept;

        // Blocked crosspoints: setCrosspoint() forces these to 0 and the UI
        // greys them out.  Used to stop a virtual loopback device routing its
        // own input back to its own output on the same channel (feedback).
        // Applied by the engine after resize(); resize() clears all blocks.
        void setBlocked (int output, int input, bool blocked) noexcept;
        bool isBlocked (int output, int input) const noexcept;

        void setInputMute (int input, bool on) noexcept;
        void setOutputMute (int output, bool on) noexcept;
        void setInputSolo (int input, bool on) noexcept;
        bool getInputMute (int input) const noexcept;
        bool getOutputMute (int output) const noexcept;
        bool getInputSolo (int input) const noexcept;

        // Snapshot (called once per processing block by consumer).
        struct Snapshot
        {
            int numIns = 0;
            int numOuts = 0;
            std::vector<float> inputTrim; // size numIns
            std::vector<float> outputTrim; // size numOuts
            std::vector<float> crosspoint; // row-major numOuts * numIns
            std::vector<unsigned char> inputMute; // size numIns
            std::vector<unsigned char> outputMute; // size numOuts
            std::vector<unsigned char> inputSolo; // size numIns
            bool anySoloActive = false;
            float at (int out, int in) const noexcept { return crosspoint[(size_t) out * (size_t) numIns + (size_t) in]; }
        };

        void takeSnapshot (Snapshot& s) const;

        // Monotonically increases on every mutating call. The audio thread reads
        // this to decide whether to rebuild its cached snapshot / sparse route
        // list. Wraps at 2^64 - fine.
        uint64_t getDirtyGeneration() const noexcept
        {
            return dirtyGen.load (std::memory_order_acquire);
        }

        // Bump the dirty generation WITHOUT writing any cell.  A Router-mode group
        // fader/mute change affects the RT mix via the group managers'
        // channelRouterGain[] (not a matrix cell), so it must still force the
        // matrix processor to recompute its cached gains on the next block.
        void touch() noexcept { bumpDirty(); }

    private:
        void bumpDirty() noexcept
        {
            dirtyGen.fetch_add (1, std::memory_order_release);
        }

        int numIns = 0;
        int numOuts = 0;
        std::vector<std::atomic<float>> inputTrim;
        std::vector<std::atomic<float>> outputTrim;
        std::vector<std::atomic<float>> crosspoint; // row-major numOuts * numIns
        std::vector<std::atomic<unsigned char>> blocked; // row-major numOuts * numIns
        std::vector<std::atomic<unsigned char>> inputMute;
        std::vector<std::atomic<unsigned char>> outputMute;
        std::vector<std::atomic<unsigned char>> inputSolo;
        std::atomic<uint64_t> dirtyGen { 1 };
    };

} // namespace dcr
