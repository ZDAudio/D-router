#include "Diagnostics/Logger.h"

#include <juce_core/juce_core.h>

#include <deque>
#include <memory>
#include <mutex>

namespace dcr
{

    namespace
    {
        constexpr size_t kRingCapacity = 5000;
        // Hard cap on the TOTAL size of every session-*.log on disk.  Once
        // this is exceeded, oldest session files are evicted until the total
        // fits.  The currently-open session is never deleted -- if it alone
        // grows past the budget the cap is best-effort only (we won't truncate
        // an open log file mid-stream; everything else gets pruned first).
        constexpr juce::int64 kTotalBudgetBytes = 1024 * 1024; // 1 MB
        // Re-check the budget every N log lines so a long session that's
        // outgrowing the budget still evicts older files on the fly, not
        // only at startup.
        constexpr int kBudgetRecheckInterval = 200;

        class FileLogger : public juce::Logger
        {
        public:
            FileLogger()
            {
                auto dir = dcr::Logger::getLogDirectory();
                dir.createDirectory();

                const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S");
                logFile = dir.getChildFile ("session-" + stamp + ".log");
                stream = logFile.createOutputStream();
                // Prune AFTER opening the new file so getFullPathName() match
                // works when we exclude the current session from eviction.
                enforceTotalBudget (dir);
                if (stream != nullptr)
                    writeHeader();
            }

            ~FileLogger() override
            {
                std::lock_guard<std::mutex> lk (mutex);
                writeLineUnlocked ("=== Logger shutting down cleanly ===");
                if (stream != nullptr)
                    stream->flush();
            }

            void logMessage (const juce::String& msg) override
            {
                const auto now = juce::Time::getCurrentTime();
                const auto stamp = now.formatted ("%H:%M:%S.")
                                   + juce::String (now.getMilliseconds()).paddedLeft ('0', 3);
                const auto line = stamp + "  " + msg;

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
                // The mtime of the executable -- always reflects the latest
                // link, unlike __DATE__/__TIME__ which only updates when this
                // particular .cpp recompiles.  That was the source of a really
                // confusing crash bisection earlier where the banner read an
                // old timestamp while the actual binary already had the fix.
                const auto execFile = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
                const auto mtime = execFile.getLastModificationTime();
                writeLineUnlocked ("=== Built: " + mtime.toString (true, true)
                                   + "   (binary mtime, always current)");
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
                while (ring.size() > kRingCapacity)
                    ring.pop_front();

                // Periodically re-enforce the on-disk byte budget.  A long
                // session that grows past the cap evicts older sessions until
                // the total fits.  Cheap (one directory scan + a couple of
                // size queries every ~200 lines).
                if (++linesSinceBudgetCheck >= kBudgetRecheckInterval)
                {
                    linesSinceBudgetCheck = 0;
                    enforceTotalBudget (logFile.getParentDirectory());
                }
            }

            // Delete oldest session-*.log files until the total on-disk size
            // is <= kTotalBudgetBytes.  Never deletes the file the current
            // FileLogger is writing to.
            void enforceTotalBudget (const juce::File& dir)
            {
                auto files = dir.findChildFiles (juce::File::findFiles, false, "session-*.log");
                files.sort(); // filenames are timestamped -> sort == oldest first

                juce::int64 total = 0;
                for (auto& f : files)
                    total += f.getSize();
                if (total <= kTotalBudgetBytes)
                    return;

                const auto currentPath = logFile.getFullPathName();
                for (auto& f : files)
                {
                    if (total <= kTotalBudgetBytes)
                        break;
                    if (f.getFullPathName() == currentPath)
                        continue; // never the active log
                    const auto sz = f.getSize();
                    if (f.deleteFile())
                        total -= sz;
                }
            }

            mutable std::mutex mutex;
            std::unique_ptr<juce::FileOutputStream> stream;
            juce::File logFile;
            std::deque<juce::String> ring;
            int linesSinceBudgetCheck = 0;
        };

        // Owned by us; juce::Logger::setCurrentLogger holds a raw pointer.
        std::unique_ptr<FileLogger> gLogger;
    }

    void Logger::init()
    {
        if (gLogger != nullptr)
            return;
        gLogger = std::make_unique<FileLogger>();
        juce::Logger::setCurrentLogger (gLogger.get());
        DBG ("dcr::Logger initialised; file: " << gLogger->getFile().getFullPathName());
    }

    void Logger::shutdown()
    {
        if (gLogger == nullptr)
            return;
        juce::Logger::setCurrentLogger (nullptr);
        gLogger.reset();
    }

    void Logger::log (const juce::String& msg)
    {
        juce::Logger::writeToLog (msg);
    }

    juce::StringArray Logger::getRecentLines (int maxLines)
    {
        if (gLogger == nullptr)
            return {};
        return gLogger->snapshotRecent (maxLines);
    }

    juce::File Logger::getCurrentLogFile()
    {
        return gLogger != nullptr ? gLogger->getFile() : juce::File {};
    }

    juce::File Logger::getLogDirectory()
    {
        return juce::File::getSpecialLocation (juce::File::userHomeDirectory)
            .getChildFile ("Library/Logs/D-Router");
    }

} // namespace dcr
