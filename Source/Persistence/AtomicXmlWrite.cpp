#include "Persistence/AtomicXmlWrite.h"

namespace dcr
{

    bool writeXmlAtomically (const juce::XmlElement& xml, const juce::File& target)
    {
        target.getParentDirectory().createDirectory();

        // TemporaryFile (target) places the temp file in the SAME directory, so
        // overwriteTargetFileWithTemporary() is a same-filesystem rename --
        // atomic on macOS (no cross-device copy).
        juce::TemporaryFile temp (target);
        if (!xml.writeTo (temp.getFile()))
            return false;
        return temp.overwriteTargetFileWithTemporary();
    }

} // namespace dcr
