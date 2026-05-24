#include "Persistence/SettingsStore.h"

#include <juce_data_structures/juce_data_structures.h>

namespace dcr {

namespace
{
    const juce::Identifier rootId             ("dcr_settings");
    const juce::Identifier engineSampleRate   ("engineSampleRate");
    const juce::Identifier engineBlockSize    ("engineBlockSize");
    const juce::Identifier inputRingMultEng   ("inputRingMultEng");
    const juce::Identifier inputRingMultDev   ("inputRingMultDev");
    const juce::Identifier outputRingMultEng  ("outputRingMultEng");
    const juce::Identifier outputRingMultDev  ("outputRingMultDev");
    const juce::Identifier outputPreFillBlocks ("outputPreFillBlocks");
    const juce::Identifier srcQuality         ("srcQuality");
    const juce::Identifier srcComplexity      ("srcComplexity");
    const juce::Identifier matrixThreadSleepMicros ("matrixThreadSleepMicros");
    const juce::Identifier matrixDrainPerWake ("matrixDrainPerWake");
    const juce::Identifier gainSmoothingMs    ("gainSmoothingMs");
    const juce::Identifier meterTimerHz       ("meterTimerHz");
    const juce::Identifier meterDecayFactor   ("meterDecayFactor");
    const juce::Identifier statusTimerMs      ("statusTimerMs");
    const juce::Identifier accentColor        ("accentColorRGB");
    const juce::Identifier warningColor       ("warningColorRGB");
    const juce::Identifier criticalColor      ("criticalColorRGB");
}

juce::File SettingsStore::getFile()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("dcorerouter");
    dir.createDirectory();
    return dir.getChildFile ("settings.xml");
}

EngineSettings SettingsStore::load()
{
    EngineSettings s;
    auto file = getFile();
    if (! file.existsAsFile()) return s;
    auto xml = juce::parseXML (file);
    if (xml == nullptr) return s;
    auto t = juce::ValueTree::fromXml (*xml);
    if (! t.hasType (rootId)) return s;

    s.engineSampleRate      = (double) t.getProperty (engineSampleRate,      s.engineSampleRate);
    s.engineBlockSize       = (int)    t.getProperty (engineBlockSize,       s.engineBlockSize);
    s.inputRingMultEng      = (int)    t.getProperty (inputRingMultEng,      s.inputRingMultEng);
    s.inputRingMultDev      = (int)    t.getProperty (inputRingMultDev,      s.inputRingMultDev);
    s.outputRingMultEng     = (int)    t.getProperty (outputRingMultEng,     s.outputRingMultEng);
    s.outputRingMultDev     = (int)    t.getProperty (outputRingMultDev,     s.outputRingMultDev);
    s.outputPreFillBlocks   = (int)    t.getProperty (outputPreFillBlocks,   s.outputPreFillBlocks);
    s.srcQuality            = (unsigned int) (int) t.getProperty (srcQuality,    (int) s.srcQuality);
    s.srcComplexity         = (unsigned int) (int) t.getProperty (srcComplexity, (int) s.srcComplexity);
    s.matrixThreadSleepMicros = (int) t.getProperty (matrixThreadSleepMicros, s.matrixThreadSleepMicros);
    s.matrixDrainPerWake    = (int)    t.getProperty (matrixDrainPerWake,    s.matrixDrainPerWake);
    s.gainSmoothingMs       = (int)    t.getProperty (gainSmoothingMs,       s.gainSmoothingMs);
    s.meterTimerHz          = (int)    t.getProperty (meterTimerHz,          s.meterTimerHz);
    s.meterDecayFactor      = (float) (double) t.getProperty (meterDecayFactor, (double) s.meterDecayFactor);
    s.statusTimerMs         = (int)    t.getProperty (statusTimerMs,         s.statusTimerMs);
    s.accentColorRGB        = (unsigned int) (int) t.getProperty (accentColor,   (int) s.accentColorRGB);
    s.warningColorRGB       = (unsigned int) (int) t.getProperty (warningColor,  (int) s.warningColorRGB);
    s.criticalColorRGB      = (unsigned int) (int) t.getProperty (criticalColor, (int) s.criticalColorRGB);

    // Defensive clamp -- a tampered settings file or values left over from a
    // prior version with wider ranges must never produce a ring/pre-fill big
    // enough to crash the engine on the next start().
    s.engineSampleRate      = juce::jlimit (8000.0, 192000.0, s.engineSampleRate);
    s.engineBlockSize       = juce::jlimit (32,    4096,      s.engineBlockSize);
    s.inputRingMultEng      = juce::jlimit (2,     32,        s.inputRingMultEng);
    s.inputRingMultDev      = juce::jlimit (2,     32,        s.inputRingMultDev);
    s.outputRingMultEng     = juce::jlimit (2,     32,        s.outputRingMultEng);
    s.outputRingMultDev     = juce::jlimit (2,     32,        s.outputRingMultDev);
    s.outputPreFillBlocks   = juce::jlimit (0,     64,        s.outputPreFillBlocks);
    s.matrixThreadSleepMicros = juce::jlimit (50, 5000,       s.matrixThreadSleepMicros);
    s.matrixDrainPerWake    = juce::jlimit (1,   256,         s.matrixDrainPerWake);
    s.gainSmoothingMs       = juce::jlimit (0,   500,         s.gainSmoothingMs);
    s.meterTimerHz          = juce::jlimit (1,   120,         s.meterTimerHz);
    s.meterDecayFactor      = juce::jlimit (0.0f, 0.999f,     s.meterDecayFactor);
    s.statusTimerMs         = juce::jlimit (100, 10000,       s.statusTimerMs);
    return s;
}

bool SettingsStore::save (const EngineSettings& s)
{
    juce::ValueTree t (rootId);
    t.setProperty (engineSampleRate,      s.engineSampleRate,      nullptr);
    t.setProperty (engineBlockSize,       s.engineBlockSize,       nullptr);
    t.setProperty (inputRingMultEng,      s.inputRingMultEng,      nullptr);
    t.setProperty (inputRingMultDev,      s.inputRingMultDev,      nullptr);
    t.setProperty (outputRingMultEng,     s.outputRingMultEng,     nullptr);
    t.setProperty (outputRingMultDev,     s.outputRingMultDev,     nullptr);
    t.setProperty (outputPreFillBlocks,   s.outputPreFillBlocks,   nullptr);
    t.setProperty (srcQuality,            (int) s.srcQuality,      nullptr);
    t.setProperty (srcComplexity,         (int) s.srcComplexity,   nullptr);
    t.setProperty (matrixThreadSleepMicros, s.matrixThreadSleepMicros, nullptr);
    t.setProperty (matrixDrainPerWake,    s.matrixDrainPerWake,    nullptr);
    t.setProperty (gainSmoothingMs,       s.gainSmoothingMs,       nullptr);
    t.setProperty (meterTimerHz,          s.meterTimerHz,          nullptr);
    t.setProperty (meterDecayFactor,      (double) s.meterDecayFactor, nullptr);
    t.setProperty (statusTimerMs,         s.statusTimerMs,         nullptr);
    t.setProperty (accentColor,           (int) s.accentColorRGB,  nullptr);
    t.setProperty (warningColor,          (int) s.warningColorRGB, nullptr);
    t.setProperty (criticalColor,         (int) s.criticalColorRGB, nullptr);

    auto xml = t.createXml();
    if (xml == nullptr) return false;
    return xml->writeTo (getFile());
}

} // namespace dcr
