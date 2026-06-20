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
    static const juce::Identifier blockSelfLoop  ("blockSelfLoop");
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
    static const juce::Identifier faderModeProp  ("faderMode");

    static const juce::Identifier channelChains  ("channelChains");
    static const juce::Identifier groupChains    ("groupChains");
    static const juce::Identifier collapsedIn    ("collapsedInputDevices");
    static const juce::Identifier collapsedOut   ("collapsedOutputDevices");
    static const juce::Identifier deviceNameProp ("deviceName");
    static const juce::Identifier chain          ("chain");
    static const juce::Identifier slot           ("slot");
    static const juce::Identifier globalIdx      ("globalIdx");
    static const juce::Identifier groupIdx       ("groupIdx");
    static const juce::Identifier isInput        ("isInput");
    static const juce::Identifier slotIdx        ("slotIdx");
    static const juce::Identifier descXml        ("descXml");
    static const juce::Identifier stateB64       ("stateB64");
    static const juce::Identifier bypassed       ("bypassed");
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
        dv.setProperty (ids::blockSelfLoop, d.blockSelfLoop, nullptr);
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
            gv.setProperty (ids::faderModeProp, g.faderMode, nullptr);
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

    // ===== Plugin chains =====================================================
    auto writeSlot = [] (juce::ValueTree& parent, int idx, const Snapshot::PluginSlotState& ps)
    {
        if (ps.isEmpty()) return;
        juce::ValueTree sv (ids::slot);
        sv.setProperty (ids::slotIdx,  idx,              nullptr);
        sv.setProperty (ids::descXml,  ps.descriptionXml, nullptr);
        sv.setProperty (ids::stateB64, ps.stateB64,      nullptr);
        sv.setProperty (ids::bypassed, ps.bypassed,      nullptr);
        parent.addChild (sv, -1, nullptr);
    };

    juce::ValueTree cc (ids::channelChains);
    for (const auto& ch : s.channelChains)
    {
        bool anyPlugin = false;
        for (const auto& ps : ch.slots) if (! ps.isEmpty()) { anyPlugin = true; break; }
        if (! anyPlugin) continue;
        juce::ValueTree cv (ids::chain);
        cv.setProperty (ids::globalIdx, ch.globalIdx, nullptr);
        cv.setProperty (ids::isInput,   ch.isInput,   nullptr);
        for (size_t i = 0; i < ch.slots.size(); ++i) writeSlot (cv, (int) i, ch.slots[i]);
        cc.addChild (cv, -1, nullptr);
    }
    root.addChild (cc, -1, nullptr);

    juce::ValueTree gc (ids::groupChains);
    for (const auto& g : s.groupChains)
    {
        bool anyPlugin = false;
        for (const auto& ps : g.slots) if (! ps.isEmpty()) { anyPlugin = true; break; }
        if (! anyPlugin) continue;
        juce::ValueTree gv (ids::chain);
        gv.setProperty (ids::groupIdx, g.groupIdx, nullptr);
        gv.setProperty (ids::isInput,  g.isInput,  nullptr);
        for (size_t i = 0; i < g.slots.size(); ++i) writeSlot (gv, (int) i, g.slots[i]);
        gc.addChild (gv, -1, nullptr);
    }
    root.addChild (gc, -1, nullptr);

    // Collapsed device-name lists (per direction).  Stored as one <device>
    // child per name with a deviceName attribute -- ValueTree doesn't have
    // a native string-array type and this is the established style elsewhere
    // in the file.
    auto writeNameList = [&] (const juce::Identifier& listId,
                              const std::vector<juce::String>& names)
    {
        juce::ValueTree list (listId);
        for (const auto& n : names)
        {
            juce::ValueTree node (ids::device);
            node.setProperty (ids::deviceNameProp, n, nullptr);
            list.addChild (node, -1, nullptr);
        }
        root.addChild (list, -1, nullptr);
    };
    writeNameList (ids::collapsedIn,  s.collapsedInputDevices);
    writeNameList (ids::collapsedOut, s.collapsedOutputDevices);

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
        d.blockSelfLoop = (bool) child.getProperty (ids::blockSelfLoop, false);
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
            g.faderMode  = (int) child.getProperty (ids::faderModeProp, 0);   // default VCA (old snapshots)
            auto mv = child.getChildWithName (ids::members);
            for (auto e : mv)
                if (e.hasType (ids::member))
                    g.memberChannels.push_back ((int) e.getProperty (ids::ch, -1));
            dest.push_back (std::move (g));
        }
    };
    readGroupList (ids::groups,      s.outputGroups);
    readGroupList (ids::inputGroups, s.inputGroups);

    auto readSlots = [] (const juce::ValueTree& parent, std::vector<Snapshot::PluginSlotState>& out)
    {
        // Resize lazily based on slotIdx so future kNumSlots changes don't break old snapshots.
        for (auto sv : parent)
        {
            if (! sv.hasType (ids::slot)) continue;
            const int idx = (int) sv.getProperty (ids::slotIdx, -1);
            if (idx < 0) continue;
            if ((int) out.size() <= idx) out.resize ((size_t) idx + 1);
            auto& ps = out[(size_t) idx];
            ps.descriptionXml = sv.getProperty (ids::descXml).toString();
            ps.stateB64       = sv.getProperty (ids::stateB64).toString();
            ps.bypassed       = (bool) sv.getProperty (ids::bypassed, false);
        }
    };
    auto cc = root.getChildWithName (ids::channelChains);
    for (auto cv : cc)
    {
        if (! cv.hasType (ids::chain)) continue;
        Snapshot::ChannelChain ch;
        ch.globalIdx = (int) cv.getProperty (ids::globalIdx, -1);
        ch.isInput   = (bool) cv.getProperty (ids::isInput, true);
        if (ch.globalIdx < 0) continue;
        readSlots (cv, ch.slots);
        s.channelChains.push_back (std::move (ch));
    }
    auto gc = root.getChildWithName (ids::groupChains);
    for (auto gv : gc)
    {
        if (! gv.hasType (ids::chain)) continue;
        Snapshot::GroupChain g;
        g.groupIdx = (int) gv.getProperty (ids::groupIdx, -1);
        g.isInput  = (bool) gv.getProperty (ids::isInput, false);
        if (g.groupIdx < 0) continue;
        readSlots (gv, g.slots);
        s.groupChains.push_back (std::move (g));
    }

    auto readNameList = [&] (const juce::Identifier& listId,
                             std::vector<juce::String>& out)
    {
        auto list = root.getChildWithName (listId);
        for (auto node : list)
        {
            if (! node.hasType (ids::device)) continue;
            const juce::String n = node.getProperty (ids::deviceNameProp, juce::String());
            if (n.isNotEmpty()) out.push_back (n);
        }
    };
    readNameList (ids::collapsedIn,  s.collapsedInputDevices);
    readNameList (ids::collapsedOut, s.collapsedOutputDevices);

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
