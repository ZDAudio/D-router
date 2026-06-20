#include "Persistence/SettingsStore.h"
#include <juce_core/juce_core.h>

using dcr::EngineSettings;
using dcr::SettingsStore;

struct SettingsStoreTests : juce::UnitTest
{
    SettingsStoreTests() : juce::UnitTest ("SettingsStore") {}

    void runTest() override
    {
        // Protect any real user settings file: back up, restore on scope exit.
        const juce::File file = SettingsStore::getFile();
        const bool had = file.existsAsFile();
        const juce::String backup = had ? file.loadFileAsString() : juce::String();

        struct RestoreGuard
        {
            juce::File f;
            bool had;
            juce::String backup;
            ~RestoreGuard()
            {
                if (had)
                    f.replaceWithText (backup);
                else
                    f.deleteFile();
            }
        } guard { file, had, backup };

        beginTest ("persisted audio-path fields survive save -> load");
        {
            EngineSettings s;
            s.engineSampleRate = 96000.0;
            s.engineBlockSize = 512;
            s.outputPreFillBlocks = 12;
            s.gainSmoothingMs = 40;

            expect (SettingsStore::save (s));
            EngineSettings r = SettingsStore::load();

            expectEquals (r.engineSampleRate, 96000.0);
            expectEquals (r.engineBlockSize, 512);
            expectEquals (r.outputPreFillBlocks, 12);
            expectEquals (r.gainSmoothingMs, 40);
        }

        // KNOWN GAP (fixed in Phase B): warn/crit threshold fields not yet
        // persisted. Documented here; asserted in Phase B.
        logMessage ("NOTE: stalledWarnRatio/cpuWarnRatio persistence is a known "
                    "Phase B gap; not asserted in Phase A.");
    }
};

static SettingsStoreTests settingsStoreTests;
