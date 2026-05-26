#include "Diagnostics/Logger.h"

#include <juce_core/juce_core.h>

#include <deque>
#include <memory>
#include <mutex>

namespace dcr {

namespace
{
    constexpr size_t kRingCapacity = 5000;
    constexpr int    kKeepSessions = 10;

    class FileLogger : public juce::Logger
    {
    public:
        FileLogger()
        {
            auto dir = dcr::Logger::getLogDirectory();
            dir.createDirectory();
            pruneOldSessions (dir);

            const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S");
            logFile = dir.getChildFile ("session-" + stamp + ".log");
            stream  = logFile.createOutputStream();
            if (stream != nullptr)
                writeHeader();
        }

        ~FileLogger() override
        {
            std::lock_guard<std::mutex> lk (mutex);
            writeLineUnlocked ("=== Logger shutting down cleanly ===");
            if (stream != nullptr) stream->flush();
        }

        void logMessage (const juce::String& msg) override
        {
            const auto now  = juce::Time::getCurrentTime();
            const auto stamp = now.formatted ("%H:%M:%S.")
                             + juce::String (now.getMilliseconds()).paddedLeft ('0', 3);
            const auto line  = stamp + "  " + msg;

            std::lock_guard<std::mutex> lk (mutex);
            writeLineUnlocked (line);
        }

        juce::StringArray snapshotRecent (int maxLines) const
        {
            std::lock_guard<std::mutex> lk (mutex);
            juce::StringArray out;
            const int take = (int) std::min ((size_t) maxLines, ring.size());
            const auto begin = ring.end() - take;
            for (auto it = begin; it != ring.end(); ++it)
                out.add (*it);
            return out;
        }

        juce::File getFile() const { return logFile; }

    private:
        void writeHeader()
        {
            writeLineUnlocked ("================================================================");
            writeLineUnlocked ("=== D-Router session started "
                               + juce::Time::getCurrentTime().toString (true, true));
            writeLineUnlocked ("=== OS:    " + juce::SystemStats::getOperatingSystemName());
            writeLineUnlocked ("=== Model: " + juce::SystemStats::getDeviceDescription());
            writeLineUnlocked ("=== CPU:   " + juce::SystemStats::getCpuVendor()
                               + " / " + juce::String (juce::SystemStats::getNumCpus()) + " cores");
            writeLineUnlocked ("=== Build: " __DATE__ " " __TIME__);
            writeLineUnlocked ("================================================================");
        }

        // Must be called with mutex held.
        void writeLineUnlocked (const juce::String& line)
        {
            if (stream != nullptr)
            {
                stream->writeText (line, false, false, "\n");
                stream->writeText ("\n", false, false, nullptr);
                // Flush every line -- if the process dies, we want the last
                // line that was actually written to be on disk.  Cost is
                // small because the OS buffers the writes anyway.
                stream->flush();
            }
            ring.push_back (line);
            while (ring.size() > kRingCapacity) ring.pop_front();
        }

        static void pruneOldSessions (const juce::File& dir)
        {
            auto files = dir.findChildFiles (juce::File::findFiles, false, "session-*.log");
            files.sort();   // filenames are timestamped, so sort == oldest first
            while (files.size() > kKeepSessions)
            {
                files.getReference (0).deleteFile();
                files.remove (0);
            }
        }

        mutable std::mutex                       mutex;
        std::unique_ptr<juce::FileOutputStream>  stream;
        juce::File                               logFile;
        std::deque<juce::String>                 ring;
    };

    // Owned by us; juce::Logger::setCurrentLogger holds a raw pointer.
    std::unique_ptr<FileLogger> gLogger;
}

void Logger::init()
{
    if (gLogger != nullptr) return;
    gLogger = std::make_unique<FileLogger>();
    juce::Logger::setCurrentLogger (gLogger.get());
    DBG ("dcr::Logger initialised; file: " << gLogger->getFile().getFullPathName());
}

void Logger::shutdown()
{
    if (gLogger == nullptr) return;
    juce::Logger::setCurrentLogger (nullptr);
    gLogger.reset();
}

void Logger::log (const juce::String& msg)
{
    juce::Logger::writeToLog (msg);
}

juce::StringArray Logger::getRecentLines (int maxLines)
{
    if (gLogger == nullptr) return {};
    return gLogger->snapshotRecent (maxLines);
}

juce::File Logger::getCurrentLogFile()
{
    return gLogger != nullptr ? gLogger->getFile() : juce::File{};
}

juce::File Logger::getLogDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                .getChildFile ("Library/Logs/D-Router");
}

} // namespace dcr
