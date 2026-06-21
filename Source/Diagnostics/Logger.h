#pragma once

#include <juce_core/juce_core.h>

namespace dcr
{

    // Central logging facility.  Forwards everything written via juce::Logger
    // (so existing DBG() calls flow through automatically) to BOTH a session
    // log file on disk AND an in-memory ring buffer the LogViewerDialog reads.
    //
    // The disk file is flushed after every line so a hard crash still leaves
    // the lead-up context recoverable.  Logs land in
    // ~/Library/Logs/D-Router/session-YYYY-MM-DD_HH-MM-SS.log
    // with the last 10 sessions kept and older ones auto-pruned.
    class Logger
    {
    public:
        // Call once at app startup BEFORE any DBG() so we capture early init.
        static void init();
        // Call from ~MainComponent after a clean shutdown so the trailing
        // marker is written and the stream is flushed.
        static void shutdown();

        // Convenience -- equivalent to juce::Logger::writeToLog (also routes
        // here automatically once init() has run).
        static void log (const juce::String& msg);

        // Snapshot of the most recent `maxLines` lines in the in-memory ring.
        // Cheap; safe to call from the message thread for the UI viewer.
        static juce::StringArray getRecentLines (int maxLines = 2000);

        // Returns the file currently being written to (or empty if init() not
        // called yet).  Used by the crash handler to append the backtrace.
        static juce::File getCurrentLogFile();

        // ~/Library/Logs/D-Router (auto-created).
        static juce::File getLogDirectory();
    };

} // namespace dcr
