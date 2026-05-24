#include "Persistence/SnapshotStore.h"

namespace dcr {

namespace ids {
    static const juce::Identifier root           ("dcorerouter");
    static const juce::Identifier engine         ("engine");
    static const juce::Identifier devices        ("devices");
    static const juce::Identifier device         ("device");
    static const juce::Identifier matrix         ("matrix");
    static const juce::Identifier inputTrim      ("inputTrim");
    static const juce::Identifier outputTrim     ("outputTrim");
    static const juce::Identifier crosspoint     ("crosspoint");
    static const juce::Identifier inputMute      ("inputMute");
    static const juce::Identifier outputMute     ("outputMute");
    static const juce::Identifier inputSolo      ("inputSolo");
    static const juce::Identifier onProp         ("on");

    static const juce::Identifier sampleRate     ("sampleRate");
    static const juce::Identifier blockSize      ("blockSize");
    static const juce::Identifier name           ("name");
    static const juce::Identifier wantInput      ("wantInput");
    static const juce::Identifier wantOutput     ("wantOutput");
    static const juce::Identifier ch             ("ch");
    static const juce::Identifier outChId        ("out");
    static const juce::Identifier inChId         ("in");
    static const juce::Identifier gain           ("gain");
    static const juce::Identifier version        ("version");

    static const juce::Identifier groups         ("groups");
    static const juce::Identifier inputGroups    ("inputGroups");
    static const juce::Identifier group          ("group");
    static const juce::Identifier layout         ("layout");
    static const juce::Identifier members        ("members");
    static const juce::Identifier member         ("member");
    static const juce::Identifier faderDb        ("faderDb");
    static const juce::Identifier mutedProp      ("muted");
}

static juce::File getAppSupportDir()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                .getChildFile ("dcorerouter");
}

juce::File SnapshotStore::getDirectory()
{
    auto d = getAppSupportDir().getChildFile ("snapshots");
    d.createDirectory();
    return d;
}

juce::File SnapshotStore::getLastUsedFile()
{
    auto d = getAppSupportDir();
    d.createDirectory();
    return d.getChildFile ("last.xml");
}

juce::ValueTree SnapshotStore::toValueTree (const Snapshot& s)
{
    juce::ValueTree root (ids::root);
    root.setProperty (ids::version, 1, nullptr);

    juce::ValueTree eng (ids::engine);
    eng.setProperty (ids::sampleRate, s.engineSampleRate, nullptr);
    eng.setProperty (ids::blockSize,  s.engineBlockSize,  nullptr);
    root.addChild (eng, -1, nullptr);

    juce::ValueTree devs (ids::devices);
    for (const auto& d : s.devices)
    {
        juce::ValueTree dv (ids::device);
        dv.setProperty (ids::name,       d.name,         nullptr);
        dv.setProperty (ids::wantInput,  d.wantInput,    nullptr);
        dv.setProperty (ids::wantOutput, d.wantOutput,   nullptr);
        devs.addChild (dv, -1, nullptr);
    }
    root.addChild (devs, -1, nullptr);

    juce::ValueTree mat (ids::matrix);
    for (size_t i = 0; i < s.inputTrim.size(); ++i)
    {
        juce::ValueTree t (ids::inputTrim);
        t.setProperty (ids::ch,   (int) i,            nullptr);
        t.setProperty (ids::gain, s.inputTrim[i],     nullptr);
        mat.addChild (t, -1, nullptr);
    }
    for (size_t i = 0; i < s.outputTrim.size(); ++i)
    {
        juce::ValueTree t (ids::outputTrim);
        t.setProperty (ids::ch,   (int) i,            nullptr);
        t.setProperty (ids::gain, s.outputTrim[i],    nullptr);
        mat.addChild (t, -1, nullptr);
    }
    for (const auto& xp : s.crosspoints)
    {
        juce::ValueTree t (ids::crosspoint);
        t.setProperty (ids::outChId, xp.outputCh, nullptr);
        t.setProperty (ids::inChId,  xp.inputCh,  nullptr);
        t.setProperty (ids::gain,    xp.gain,     nullptr);
        mat.addChild (t, -1, nullptr);
    }
    auto writeBoolList = [&] (const juce::Identifier& id, const std::vector<unsigned char>& v)
    {
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i])
            {
                juce::ValueTree t (id);
                t.setProperty (ids::ch,    (int) i, nullptr);
                t.setProperty (ids::onProp, true,   nullptr);
                mat.addChild (t, -1, nullptr);
            }
    };
    writeBoolList (ids::inputMute,  s.inputMute);
    writeBoolList (ids::outputMute, s.outputMute);
    writeBoolList (ids::inputSolo,  s.inputSolo);

    root.addChild (mat, -1, nullptr);

    auto writeGroupList = [] (const juce::Identifier& parentId,
                              const std::vector<Snapshot::Group>& src,
                              juce::ValueTree& parent)
    {
        juce::ValueTree gs (parentId);
        for (const auto& g : src)
        {
            juce::ValueTree gv (ids::group);
            gv.setProperty (ids::name,      g.name,       nullptr);
            gv.setProperty (ids::layout,    g.layoutName, nullptr);
            gv.setProperty (ids::faderDb,   (double) g.faderDb, nullptr);
            gv.setProperty (ids::mutedProp, g.muted,      nullptr);
            juce::ValueTree mv (ids::members);
            for (int m : g.memberChannels)
            {
                juce::ValueTree e (ids::member);
                e.setProperty (ids::ch, m, nullptr);
                mv.addChild (e, -1, nullptr);
            }
            gv.addChild (mv, -1, nullptr);
            gs.addChild (gv, -1, nullptr);
        }
        parent.addChild (gs, -1, nullptr);
    };
    writeGroupList (ids::groups,      s.outputGroups, root);
    writeGroupList (ids::inputGroups, s.inputGroups,  root);

    return root;
}

Snapshot SnapshotStore::fromValueTree (const juce::ValueTree& root)
{
    Snapshot s;
    if (! root.hasType (ids::root)) return s;

    auto eng = root.getChildWithName (ids::engine);
    if (eng.isValid())
    {
        s.engineSampleRate = (double) eng.getProperty (ids::sampleRate, 48000.0);
        s.engineBlockSize  = (int)    eng.getProperty (ids::blockSize,  128);
    }

    auto devs = root.getChildWithName (ids::devices);
    for (auto child : devs)
    {
        AudioEngine::DeviceSpec d;
        d.name       = child.getProperty (ids::name).toString();
        d.wantInput  = (bool) child.getProperty (ids::wantInput);
        d.wantOutput = (bool) child.getProperty (ids::wantOutput);
        if (d.name.isNotEmpty() && (d.wantInput || d.wantOutput))
            s.devices.push_back (d);
    }

    auto mat = root.getChildWithName (ids::matrix);
    for (auto child : mat)
    {
        if (child.hasType (ids::inputTrim))
        {
            const int c = (int) child.getProperty (ids::ch);
            if (c >= 0)
            {
                if ((int) s.inputTrim.size() <= c) s.inputTrim.resize ((size_t) c + 1, 1.0f);
                s.inputTrim[(size_t) c] = (float) (double) child.getProperty (ids::gain, 1.0);
            }
        }
        else if (child.hasType (ids::outputTrim))
        {
            const int c = (int) child.getProperty (ids::ch);
            if (c >= 0)
            {
                if ((int) s.outputTrim.size() <= c) s.outputTrim.resize ((size_t) c + 1, 1.0f);
                s.outputTrim[(size_t) c] = (float) (double) child.getProperty (ids::gain, 1.0);
            }
        }
        else if (child.hasType (ids::crosspoint))
        {
            Snapshot::Crosspoint xp;
            xp.outputCh = (int) child.getProperty (ids::outChId);
            xp.inputCh  = (int) child.getProperty (ids::inChId);
            xp.gain     = (float) (double) child.getProperty (ids::gain, 0.0);
            if (xp.outputCh >= 0 && xp.inputCh >= 0) s.crosspoints.push_back (xp);
        }
        else if (child.hasType (ids::inputMute) || child.hasType (ids::outputMute)
              || child.hasType (ids::inputSolo))
        {
            const int c = (int) child.getProperty (ids::ch);
            const bool on = (bool) child.getProperty (ids::onProp);
            auto& target = child.hasType (ids::inputMute)  ? s.inputMute
                         : child.hasType (ids::outputMute) ? s.outputMute
                                                           : s.inputSolo;
            if (c >= 0)
            {
                if ((int) target.size() <= c) target.resize ((size_t) c + 1, 0);
                target[(size_t) c] = on ? 1 : 0;
            }
        }
    }

    auto readGroupList = [&] (const juce::Identifier& parentId,
                              std::vector<Snapshot::Group>& dest)
    {
        auto gs = root.getChildWithName (parentId);
        for (auto child : gs)
        {
            if (! child.hasType (ids::group)) continue;
            Snapshot::Group g;
            g.name       = child.getProperty (ids::name).toString();
            g.layoutName = child.getProperty (ids::layout).toString();
            g.faderDb    = (float) (double) child.getProperty (ids::faderDb, 0.0);
            g.muted      = (bool) child.getProperty (ids::mutedProp);
            auto mv = child.getChildWithName (ids::members);
            for (auto e : mv)
                if (e.hasType (ids::member))
                    g.memberChannels.push_back ((int) e.getProperty (ids::ch, -1));
            dest.push_back (std::move (g));
        }
    };
    readGroupList (ids::groups,      s.outputGroups);
    readGroupList (ids::inputGroups, s.inputGroups);
    return s;
}

bool SnapshotStore::save (const juce::File& file, const Snapshot& s)
{
    auto tree = toValueTree (s);
    auto xml = tree.createXml();
    if (xml == nullptr) return false;
    file.getParentDirectory().createDirectory();
    return xml->writeTo (file);
}

bool SnapshotStore::load (const juce::File& file, Snapshot& outSnap)
{
    if (! file.existsAsFile()) return false;
    auto xml = juce::parseXML (file);
    if (xml == nullptr) return false;
    auto tree = juce::ValueTree::fromXml (*xml);
    if (! tree.isValid()) return false;
    outSnap = fromValueTree (tree);
    return true;
}

} // namespace dcr
