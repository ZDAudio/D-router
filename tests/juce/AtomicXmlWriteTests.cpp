#include "Persistence/AtomicXmlWrite.h"
#include <juce_core/juce_core.h>

struct AtomicXmlWriteTests : juce::UnitTest
{
    AtomicXmlWriteTests() : juce::UnitTest ("AtomicXmlWrite") {}

    void runTest() override
    {
        beginTest ("writes parseable content to a fresh path");
        {
            juce::TemporaryFile scratch; // owns + deletes its path on scope exit
            const juce::File target = scratch.getFile();

            juce::XmlElement xml ("root");
            xml.setAttribute ("k", "v1");
            expect (dcr::writeXmlAtomically (xml, target));
            expect (target.existsAsFile());

            auto back = juce::parseXML (target);
            expect (back != nullptr);
            expectEquals (back->getStringAttribute ("k"), juce::String ("v1"));
        }

        beginTest ("atomically replaces existing content");
        {
            juce::TemporaryFile scratch;
            const juce::File target = scratch.getFile();

            juce::XmlElement a ("root");
            a.setAttribute ("k", "v1");
            expect (dcr::writeXmlAtomically (a, target));

            juce::XmlElement b ("root");
            b.setAttribute ("k", "v2");
            expect (dcr::writeXmlAtomically (b, target));

            auto back = juce::parseXML (target);
            expect (back != nullptr);
            expectEquals (back->getStringAttribute ("k"), juce::String ("v2"));
        }

        beginTest ("leaves no temp sibling for the target");
        {
            juce::TemporaryFile scratch;
            const juce::File target = scratch.getFile();

            juce::XmlElement xml ("root");
            expect (dcr::writeXmlAtomically (xml, target));

            // JUCE TemporaryFile names its sibling "<stem>_temp<hex><ext>", so
            // scan for OUR target's stem specifically (not "*.temp", which never
            // matches and would make this vacuous; and not a bare "*_temp*",
            // which would catch unrelated files in the shared temp dir and flake).
            // A successful atomic write renames the temp onto the target, none left.
            auto leftovers = target.getParentDirectory().findChildFiles (
                juce::File::findFiles, false, target.getFileNameWithoutExtension() + "_temp*");
            expectEquals (leftovers.size(), 0);
        }
    }
};

static AtomicXmlWriteTests atomicXmlWriteTests;
