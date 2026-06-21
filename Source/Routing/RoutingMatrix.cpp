#include "RoutingMatrix.h"

namespace dcr
{

    void RoutingMatrix::resize (int newIns, int newOuts)
    {
        numIns = newIns;
        numOuts = newOuts;

        std::vector<std::atomic<float>> nIn ((size_t) newIns);
        std::vector<std::atomic<float>> nOut ((size_t) newOuts);
        std::vector<std::atomic<float>> nXp ((size_t) newOuts * (size_t) newIns);
        std::vector<std::atomic<unsigned char>> nBlocked ((size_t) newOuts * (size_t) newIns);
        std::vector<std::atomic<unsigned char>> nInMute ((size_t) newIns);
        std::vector<std::atomic<unsigned char>> nOutMute ((size_t) newOuts);
        std::vector<std::atomic<unsigned char>> nInSolo ((size_t) newIns);
        for (auto& v : nIn)
            v.store (1.0f, std::memory_order_relaxed);
        for (auto& v : nOut)
            v.store (1.0f, std::memory_order_relaxed);
        for (auto& v : nXp)
            v.store (0.0f, std::memory_order_relaxed);
        for (auto& v : nBlocked)
            v.store (0, std::memory_order_relaxed);
        for (auto& v : nInMute)
            v.store (0, std::memory_order_relaxed);
        for (auto& v : nOutMute)
            v.store (0, std::memory_order_relaxed);
        for (auto& v : nInSolo)
            v.store (0, std::memory_order_relaxed);

        inputTrim = std::move (nIn);
        outputTrim = std::move (nOut);
        crosspoint = std::move (nXp);
        blocked = std::move (nBlocked);
        inputMute = std::move (nInMute);
        outputMute = std::move (nOutMute);
        inputSolo = std::move (nInSolo);
        bumpDirty();
    }

    void RoutingMatrix::setBlocked (int m, int n, bool b) noexcept
    {
        if (m >= 0 && m < numOuts && n >= 0 && n < numIns
            && (size_t) ((size_t) m * (size_t) numIns + (size_t) n) < blocked.size())
        {
            blocked[(size_t) m * (size_t) numIns + (size_t) n].store (b ? 1 : 0, std::memory_order_relaxed);
            if (b) // force the cell to silence the moment it becomes blocked
                crosspoint[(size_t) m * (size_t) numIns + (size_t) n].store (0.0f, std::memory_order_relaxed);
            bumpDirty();
        }
    }

    bool RoutingMatrix::isBlocked (int m, int n) const noexcept
    {
        if (m >= 0 && m < numOuts && n >= 0 && n < numIns
            && (size_t) ((size_t) m * (size_t) numIns + (size_t) n) < blocked.size())
            return blocked[(size_t) m * (size_t) numIns + (size_t) n].load (std::memory_order_relaxed) != 0;
        return false;
    }

    void RoutingMatrix::setInputMute (int n, bool on) noexcept
    {
        if (n >= 0 && n < numIns)
        {
            inputMute[(size_t) n].store (on ? 1 : 0, std::memory_order_relaxed);
            bumpDirty();
        }
    }
    void RoutingMatrix::setOutputMute (int m, bool on) noexcept
    {
        if (m >= 0 && m < numOuts)
        {
            outputMute[(size_t) m].store (on ? 1 : 0, std::memory_order_relaxed);
            bumpDirty();
        }
    }
    void RoutingMatrix::setInputSolo (int n, bool on) noexcept
    {
        if (n >= 0 && n < numIns)
        {
            inputSolo[(size_t) n].store (on ? 1 : 0, std::memory_order_relaxed);
            bumpDirty();
        }
    }
    bool RoutingMatrix::getInputMute (int n) const noexcept
    {
        return n >= 0 && n < numIns && inputMute[(size_t) n].load (std::memory_order_relaxed) != 0;
    }
    bool RoutingMatrix::getOutputMute (int m) const noexcept
    {
        return m >= 0 && m < numOuts && outputMute[(size_t) m].load (std::memory_order_relaxed) != 0;
    }
    bool RoutingMatrix::getInputSolo (int n) const noexcept
    {
        return n >= 0 && n < numIns && inputSolo[(size_t) n].load (std::memory_order_relaxed) != 0;
    }

    void RoutingMatrix::setInputTrim (int n, float g) noexcept
    {
        if (n >= 0 && n < numIns)
        {
            inputTrim[(size_t) n].store (g, std::memory_order_relaxed);
            bumpDirty();
        }
    }

    void RoutingMatrix::setOutputTrim (int m, float g) noexcept
    {
        if (m >= 0 && m < numOuts)
        {
            outputTrim[(size_t) m].store (g, std::memory_order_relaxed);
            bumpDirty();
        }
    }

    void RoutingMatrix::setCrosspoint (int m, int n, float g) noexcept
    {
        if (m >= 0 && m < numOuts && n >= 0 && n < numIns)
        {
            const size_t idx = (size_t) m * (size_t) numIns + (size_t) n;
            // Blocked cells (virtual-device self-loop) are forced to silence,
            // so a stray set (snapshot restore, drag) can't re-enable feedback.
            if (idx < blocked.size() && blocked[idx].load (std::memory_order_relaxed) != 0)
                g = 0.0f;
            crosspoint[idx].store (g, std::memory_order_relaxed);
            bumpDirty();
        }
    }

    float RoutingMatrix::getInputTrim (int n) const noexcept
    {
        return (n >= 0 && n < numIns) ? inputTrim[(size_t) n].load (std::memory_order_relaxed) : 0.0f;
    }

    float RoutingMatrix::getOutputTrim (int m) const noexcept
    {
        return (m >= 0 && m < numOuts) ? outputTrim[(size_t) m].load (std::memory_order_relaxed) : 0.0f;
    }

    float RoutingMatrix::getCrosspoint (int m, int n) const noexcept
    {
        if (m < 0 || m >= numOuts || n < 0 || n >= numIns)
            return 0.0f;
        return crosspoint[(size_t) m * (size_t) numIns + (size_t) n].load (std::memory_order_relaxed);
    }

    void RoutingMatrix::takeSnapshot (Snapshot& s) const
    {
        s.numIns = numIns;
        s.numOuts = numOuts;
        s.inputTrim.resize ((size_t) numIns);
        s.outputTrim.resize ((size_t) numOuts);
        s.crosspoint.resize ((size_t) numOuts * (size_t) numIns);
        s.inputMute.resize ((size_t) numIns);
        s.outputMute.resize ((size_t) numOuts);
        s.inputSolo.resize ((size_t) numIns);

        s.anySoloActive = false;
        for (int n = 0; n < numIns; ++n)
        {
            s.inputTrim[(size_t) n] = inputTrim[(size_t) n].load (std::memory_order_relaxed);
            s.inputMute[(size_t) n] = inputMute[(size_t) n].load (std::memory_order_relaxed);
            s.inputSolo[(size_t) n] = inputSolo[(size_t) n].load (std::memory_order_relaxed);
            if (s.inputSolo[(size_t) n])
                s.anySoloActive = true;
        }
        for (int m = 0; m < numOuts; ++m)
        {
            s.outputTrim[(size_t) m] = outputTrim[(size_t) m].load (std::memory_order_relaxed);
            s.outputMute[(size_t) m] = outputMute[(size_t) m].load (std::memory_order_relaxed);
        }
        for (size_t i = 0; i < s.crosspoint.size(); ++i)
            s.crosspoint[i] = crosspoint[i].load (std::memory_order_relaxed);
    }

} // namespace dcr
