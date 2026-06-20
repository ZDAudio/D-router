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

        beginTest ("load reports NoFile for a missing path");
        {
            juce::TemporaryFile scratch;
            juce::File missing = scratch.getFile();
            missing.deleteFile();
            Snapshot s;
            expect (SnapshotStore::load (missing, s) == SnapshotStore::LoadResult::NoFile);
        }

        beginTest ("round-trips through disk as LoadResult::Ok");
        {
            juce::TemporaryFile scratch;
            const juce::File file = scratch.getFile();
            Snapshot s;
            s.engineSampleRate = 88200.0;
            s.engineBlockSize = 64;
            expect (SnapshotStore::save (file, s));

            Snapshot r;
            expect (SnapshotStore::load (file, r) == SnapshotStore::LoadResult::Ok);
            expectEquals (r.engineSampleRate, 88200.0);
            expectEquals (r.engineBlockSize, 64);
        }

        beginTest ("load reports ParseError on non-XML garbage");
        {
            juce::TemporaryFile scratch;
            const juce::File file = scratch.getFile();
            file.replaceWithText ("this is definitely not xml");
            Snapshot s;
            expect (SnapshotStore::load (file, s) == SnapshotStore::LoadResult::ParseError);
        }

        beginTest ("load reports Corrupt for valid XML of the wrong shape");
        {
            juce::TemporaryFile scratch;
            const juce::File file = scratch.getFile();
            file.replaceWithText ("<somethingElse foo='1'/>");
            Snapshot s;
            expect (SnapshotStore::load (file, s) == SnapshotStore::LoadResult::Corrupt);
        }

        beginTest ("load reports UnsupportedVersion for a future schema");
        {
            juce::TemporaryFile scratch;
            const juce::File file = scratch.getFile();
            Snapshot s;
            auto tree = SnapshotStore::toValueTree (s);
            tree.setProperty ("version", SnapshotStore::kVersion + 1, nullptr);
            auto xml = tree.createXml();
            expect (xml != nullptr);
            expect (xml->writeTo (file));
            Snapshot out;
            expect (SnapshotStore::load (file, out) == SnapshotStore::LoadResult::UnsupportedVersion);
        }

        beginTest ("save then overwrite leaves a loadable, current file (atomic)");
        {
            juce::TemporaryFile scratch;
            const juce::File file = scratch.getFile();

            Snapshot a;
            a.engineBlockSize = 128;
            expect (SnapshotStore::save (file, a));

            Snapshot b;
            b.engineBlockSize = 512;
            expect (SnapshotStore::save (file, b)); // overwrite

            Snapshot r;
            expect (SnapshotStore::load (file, r) == SnapshotStore::LoadResult::Ok);
            expectEquals (r.engineBlockSize, 512);

            auto leftovers = file.getParentDirectory()
                                 .findChildFiles (juce::File::findFiles, false, "*.temp");
            for (auto& f : leftovers)
                expect (!f.getFileName().startsWith (file.getFileName()));
        }
    }
};

static SnapshotStoreTests snapshotStoreTests;
