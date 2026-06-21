#include "PanicController.h"

#include "RoutingMatrix.h"

#include <algorithm>

namespace dcr
{

    bool PanicController::engage (RoutingMatrix& m)
    {
        const int nIn = m.getNumInputs();
        const int nOut = m.getNumOutputs();
        if (nIn == 0 && nOut == 0)
            return false;

        savedInputMutes.assign ((size_t) nIn, 0);
        savedOutputMutes.assign ((size_t) nOut, 0);
        for (int n = 0; n < nIn; ++n)
        {
            savedInputMutes[(size_t) n] = m.getInputMute (n) ? 1 : 0;
            m.setInputMute (n, true);
        }
        for (int o = 0; o < nOut; ++o)
        {
            savedOutputMutes[(size_t) o] = m.getOutputMute (o) ? 1 : 0;
            m.setOutputMute (o, true);
        }
        state_ = State::Active;
        return true;
    }

    void PanicController::release (RoutingMatrix& m)
    {
        // min() guards against the channel count having shrunk since engage.
        const int nIn = std::min ((int) savedInputMutes.size(), m.getNumInputs());
        const int nOut = std::min ((int) savedOutputMutes.size(), m.getNumOutputs());
        for (int n = 0; n < nIn; ++n)
            m.setInputMute (n, savedInputMutes[(size_t) n] != 0);
        for (int o = 0; o < nOut; ++o)
            m.setOutputMute (o, savedOutputMutes[(size_t) o] != 0);
        savedInputMutes.clear();
        savedOutputMutes.clear();
        state_ = State::Inactive;
    }

    void PanicController::noteUserMuteChanged() noexcept
    {
        if (state_ != State::Active)
            return;
        state_ = State::Inactive;
        savedInputMutes.clear();
        savedOutputMutes.clear();
    }

    bool PanicController::reset() noexcept
    {
        if (state_ == State::Inactive && savedInputMutes.empty() && savedOutputMutes.empty())
            return false;
        state_ = State::Inactive;
        savedInputMutes.clear();
        savedOutputMutes.clear();
        return true;
    }

}
