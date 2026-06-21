#pragma once

#include <juce_core/juce_core.h>

namespace dcr
{

    // Atomically write `xml` to `target`: serialise into a TemporaryFile in the
    // SAME directory, then rename it over the target.  A crash (or power loss)
    // mid-write then leaves the previous file intact instead of a half-written,
    // unparseable one -- which matters because the bare xml->writeTo() this
    // replaces could corrupt the very snapshot/settings file recovery wants.
    // Returns false if the temp write or the atomic replace fails.
    bool writeXmlAtomically (const juce::XmlElement& xml, const juce::File& target);

} // namespace dcr
