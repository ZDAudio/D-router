#include "Persistence/SnapshotStore.h"
#include <juce_core/juce_core.h>

using dcr::Snapshot;
using dcr::SnapshotStore;

struct SnapshotStoreTests : juce::UnitTest
{
    SnapshotStoreTests() : juce::UnitTest ("SnapshotStore") {}

    void runTest() override
    {
        beginTest ("toValueTree -> fromValueTree preserves scalar + vector state");
        {
            Snapshot s;
            s.engineSampleRate = 44100.0;
            s.engineBlockSize = 256;
            s.inputTrim = { 1.0f, 0.5f };
            s.outputTrim = { 0.25f };
            s.inputMute = { 0, 1 };
            s.crosspoints.push_back ({ /*out*/ 0, /*in*/ 1, /*gain*/ 0.75f });

            auto r = SnapshotStore::fromValueTree (SnapshotStore::toValueTree (s));

            expectEquals (r.engineSampleRate, 44100.0);
            expectEquals (r.engineBlockSize, 256);
            expectEquals ((int) r.inputTrim.size(), 2);
            expectEquals (r.inputTrim[1], 0.5f);
            expectEquals ((int) r.outputTrim.size(), 1);
            expectEquals (r.outputTrim[0], 0.25f);
            expectEquals ((int) r.inputMute.size(), 2);
            expect (r.inputMute[1] != 0);
            expectEquals ((int) r.crosspoints.size(), 1);
            expectEquals (r.crosspoints[0].outputCh, 0);
            expectEquals (r.crosspoints[0].inputCh, 1);
            expectEquals (r.crosspoints[0].gain, 0.75f);
        }

        beginTest ("group state round-trips (name, layout, members, fader, mute)");
        {
            Snapshot s;
            Snapshot::Group g;
            g.name = "Mains";
            g.layoutName = "Stereo";
            g.memberChannels = { 0, 1 };
            g.faderDb = -6.0f;
            g.muted = true;
            s.outputGroups.push_back (g);

            auto r = SnapshotStore::fromValueTree (SnapshotStore::toValueTree (s));
            expectEquals ((int) r.outputGroups.size(), 1);
            expectEquals (r.outputGroups[0].name, juce::String ("Mains"));
            expectEquals (r.outputGroups[0].layoutName, juce::String ("Stereo"));
            expectEquals ((int) r.outputGroups[0].memberChannels.size(), 2);
            expectEquals (r.outputGroups[0].faderDb, -6.0f);
            expect (r.outputGroups[0].muted);
        }

        beginTest ("empty snapshot round-trips without error");
        {
            Snapshot s;
            auto r = SnapshotStore::fromValueTree (SnapshotStore::toValueTree (s));
            expectEquals ((int) r.inputTrim.size(), 0);
            expectEquals ((int) r.crosspoints.size(), 0);
        }
    }
};

static SnapshotStoreTests snapshotStoreTests;
