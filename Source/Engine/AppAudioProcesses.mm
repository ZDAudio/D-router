#include "Engine/AppAudioProcesses.h"

namespace dcr
{
AppAudioProcesses::AppAudioProcesses() = default;
AppAudioProcesses::~AppAudioProcesses() = default;

std::vector<AppAudioProcesses::Entry> AppAudioProcesses::enumerate() const
{
    return {};
}

AudioObjectID AppAudioProcesses::resolve(const std::string&) const
{
    return kAudioObjectUnknown;
}
} // namespace dcr
