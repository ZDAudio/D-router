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

        beginTest ("warn/crit threshold fields survive save -> load");
        {
            EngineSettings s;
            s.stalledWarnRatio = 0.02f;
            s.stalledCritRatio = 0.10f;
            s.cpuWarnRatio = 0.60f;
            s.cpuCritRatio = 0.85f;

            expect (SettingsStore::save (s));
            EngineSettings r = SettingsStore::load();

            expectEquals (r.stalledWarnRatio, 0.02f);
            expectEquals (r.stalledCritRatio, 0.10f);
            expectEquals (r.cpuWarnRatio, 0.60f);
            expectEquals (r.cpuCritRatio, 0.85f);
        }

        beginTest ("settings file carries a version attribute");
        {
            EngineSettings s;
            expect (SettingsStore::save (s));
            auto xml = juce::parseXML (SettingsStore::getFile());
            expect (xml != nullptr);
            expect (xml->hasAttribute ("version"));
            expectEquals (xml->getIntAttribute ("version"), 1);
        }

        beginTest ("out-of-range threshold ratios clamp to [0,1] on load");
        {
            EngineSettings s;
            s.cpuWarnRatio = 5.0f; // tampered / out of range
            s.stalledWarnRatio = -1.0f;
            expect (SettingsStore::save (s));
            EngineSettings r = SettingsStore::load();
            expectEquals (r.cpuWarnRatio, 1.0f);
            expectEquals (r.stalledWarnRatio, 0.0f);
        }
    }
};

static SettingsStoreTests settingsStoreTests;
