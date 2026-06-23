#include "Engine/AppAudioWorker.h"

namespace dcr
{
AppAudioWorker::AppAudioWorker(bool mute, int nch)
    : muteOriginalOutput(mute), numChannels(nch) {}

AppAudioWorker::~AppAudioWorker()
{
    close();
}

bool AppAudioWorker::open(const EngineSettings& s)
{
    settings = s;
    return true;
}

void AppAudioWorker::close() {}
bool AppAudioWorker::attach(AudioObjectID)
{
    return false;
}
void AppAudioWorker::detach() {}
void AppAudioWorker::ioBlock(const AudioBufferList*, int) {}
} // namespace dcr
