#include "DSP/SafePluginOps.h"

#if JUCE_MAC
 #import <Foundation/Foundation.h>
#endif

namespace dcr::safe {

namespace
{
    bool runGuarded (const char* op, juce::AudioPluginInstance* p, void (^block)()) noexcept
    {
       #if JUCE_MAC
        @try {
            @try { block(); return true; }
            @catch (NSException* ex) {
                DBG ("[safe::" << op << " for " << (p ? p->getName() : juce::String ("?"))
                     << "] NSException: " << [[ex reason] UTF8String]);
                return false;
            }
        }
        @catch (...) {
            DBG ("[safe::" << op << "] unknown exception");
            return false;
        }
       #else
        try { block(); return true; }
        catch (...) { DBG ("[safe::" << op << "] exception"); return false; }
       #endif
    }
}

bool getStateInformation (juce::AudioPluginInstance& p, juce::MemoryBlock& dest) noexcept
{
    dest.reset();
    return runGuarded ("getStateInformation", &p, ^{ p.getStateInformation (dest); });
}

bool setStateInformation (juce::AudioPluginInstance& p, const void* data, int sizeInBytes) noexcept
{
    return runGuarded ("setStateInformation", &p,
                       ^{ p.setStateInformation (data, sizeInBytes); });
}

bool getPluginDescriptionXml (juce::AudioPluginInstance& p, juce::String& destXml) noexcept
{
    destXml.clear();
    return runGuarded ("getPluginDescriptionXml", &p, ^{
        if (auto xml = p.getPluginDescription().createXml())
            destXml = xml->toString (juce::XmlElement::TextFormat().singleLine());
    });
}

bool loadPluginDescriptionFromXml (const juce::String& xml,
                                   juce::PluginDescription& destDesc) noexcept
{
    // __block so the ObjC block can write through to the local from inside.
    __block bool result = false;
    runGuarded ("loadPluginDescriptionFromXml", nullptr, ^{
        if (auto x = juce::parseXML (xml))
            result = destDesc.loadFromXml (*x);
    });
    return result;
}

} // namespace dcr::safe
