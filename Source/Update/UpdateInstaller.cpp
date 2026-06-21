#include "Update/UpdateInstaller.h"

#include <unistd.h> // getpid, getuid

namespace dcr::update
{

    juce::File UpdateInstaller::appBundle()
    {
        // On macOS currentApplicationFile is the .app bundle itself.
        return juce::File::getSpecialLocation (juce::File::currentApplicationFile);
    }

    juce::File UpdateInstaller::cacheDir()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
            .getChildFile ("Caches")
            .getChildFile ("D-Router")
            .getChildFile ("update");
    }

    juce::URL UpdateInstaller::releasesPageUrl()
    {
        return juce::URL ("https://github.com/ZDAudio/D-router/releases");
    }

    bool UpdateInstaller::canInstallInPlace (juce::String& reason)
    {
        const auto app = appBundle();

        // Gatekeeper App Translocation runs unsigned/quarantined apps from a random
        // read-only path; replacing in place there is impossible and pointless.
        if (app.getFullPathName().contains ("/AppTranslocation/"))
        {
            reason = "D-Router is running from a temporary read-only location. "
                     "Move D-Router to your Applications folder, then check for updates again.";
            return false;
        }

        if (!app.getParentDirectory().hasWriteAccess())
        {
            reason = "Can't write to " + app.getParentDirectory().getFullPathName() + ". "
                                                                                      "Move D-Router to a writable folder (e.g. ~/Applications) and try again.";
            return false;
        }

        return true;
    }

    namespace
    {
        // Single-quote a path for safe embedding in the bash swap script.
        juce::String shQuote (const juce::String& s)
        {
            return "'" + s.replace ("'", "'\\''") + "'";
        }
    }

    void UpdateInstaller::run()
    {
        auto finish = [this] (bool ok, juce::String err) {
            juce::MessageManager::callAsync ([cb = done, ok, err] { if (cb) cb (ok, err); });
        };

        // --- 1. download to the cache, with progress + size verification ---------
        auto dir = cacheDir();
        dir.deleteRecursively();
        if (!dir.createDirectory())
        {
            finish (false, "Can't create the update folder.");
            return;
        }

        const auto zip = dir.getChildFile ("D-Router.zip");
        {
            const auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                  .withConnectionTimeoutMs (15000)
                                  .withExtraHeaders ("User-Agent: D-Router-Updater");
            std::unique_ptr<juce::InputStream> in (release.zipUrl.createInputStream (opts));
            if (in == nullptr)
            {
                finish (false, "Couldn't start the download.");
                return;
            }

            juce::FileOutputStream out (zip);
            if (out.failedToOpen())
            {
                finish (false, "Couldn't write the download.");
                return;
            }

            const juce::int64 total = release.zipSize > 0 ? release.zipSize : in->getTotalLength();
            juce::int64 got = 0;
            juce::HeapBlock<char> buffer (1 << 16);

            while (!in->isExhausted())
            {
                if (cancelled || threadShouldExit())
                {
                    zip.deleteFile();
                    finish (false, {});
                    return;
                }

                const int n = in->read (buffer, 1 << 16);
                if (n <= 0)
                    break;
                out.write (buffer, (size_t) n);
                got += n;

                if (total > 0)
                    juce::MessageManager::callAsync (
                        [cb = progress, frac = (double) got / (double) total] { if (cb) cb (frac); });
            }
            out.flush();

            if (release.zipSize > 0 && got != release.zipSize)
            {
                zip.deleteFile();
                finish (false, "The download was incomplete. Please try again.");
                return;
            }
        }

        // --- 2. unpack with ditto (preserves the bundle layout) ------------------
        auto staging = dir.getChildFile ("staged");
        staging.deleteRecursively();
        staging.createDirectory();
        {
            juce::ChildProcess unzip;
            unzip.start (juce::StringArray { "/usr/bin/ditto", "-x", "-k", zip.getFullPathName(), staging.getFullPathName() });
            unzip.waitForProcessToFinish (60000);
            if (unzip.getExitCode() != 0)
            {
                finish (false, "Couldn't unpack the update.");
                return;
            }
        }

        const auto newApp = staging.getChildFile ("D-Router.app");
        if (!newApp.isDirectory())
        {
            finish (false, "The update package was malformed.");
            return;
        }

        // --- 3. hand the swap to LAUNCHD ----------------------------------------
        // A child WE spawn is killed by macOS the instant we quit -- verified to
        // happen even with POSIX_SPAWN_SETSID (own session) AND responsibility-
        // disclaim.  A launchd job is not our child and not our "responsibility",
        // so it is the reliable way to run code that outlives us.  Write a swap
        // script + a LaunchAgent that runs it, load it with `launchctl bootstrap`,
        // then quit; launchd performs the swap + relaunch.
        const auto oldApp = appBundle();
        const int pid = (int) ::getpid();
        const int uid = (int) ::getuid();
        const juce::String label = "com.zdaudio.drouter.updateswap";
        const auto script = dir.getChildFile ("swap.sh");
        const auto plist = dir.getChildFile ("updateswap.plist");

        // launchd gives the job a MINIMAL PATH, so call tools by absolute path.
        // $HOME *is* set by launchd (verified), so the log path resolves.  The
        // /tmp marker is a belt-and-suspenders liveness check.
        juce::String sh;
        sh << "#!/bin/bash\n"
           << "PID=" << pid << "\n"
           << "USERID=" << uid << "\n"
           << "LABEL=" << label << "\n"
           << "OLD=" << shQuote (oldApp.getFullPathName()) << "\n"
           << "NEW=" << shQuote (newApp.getFullPathName()) << "\n"
           << "STAGING=" << shQuote (staging.getFullPathName()) << "\n"
           << "ZIP=" << shQuote (zip.getFullPathName()) << "\n"
           << "PLIST=" << shQuote (plist.getFullPathName()) << "\n"
           << "echo \"[$(/bin/date)] launchd swap helper alive pid=$$\" > /tmp/drouter-swap.marker 2>&1\n"
           // NOTE: /bin/bash is Apple's bash 3.2, which mis-parses nested double
           // quotes inside $(...)  -- e.g. "$(dirname "$LOG")" aborts the whole
           // script (exit 2).  Keep the dir literal; no command substitution.
           << "LOGDIR=\"$HOME/Library/Logs/D-Router\"\n"
           << "/bin/mkdir -p \"$LOGDIR\"\n"
           << "LOG=\"$LOGDIR/update-swap.log\"\n"
           << "exec >>\"$LOG\" 2>&1\n"
           << "echo \"===== [$(/bin/date)] launchd swap start pid=$$ watch=$PID =====\"\n"
           << "while /bin/kill -0 \"$PID\" 2>/dev/null; do /bin/sleep 0.2; done\n"
           << "echo \"[$(/bin/date)] app exited; swapping $OLD\"\n"
           << "/bin/sleep 0.3\n"
           << "/usr/bin/xattr -cr \"$NEW\" 2>/dev/null\n"
           // Copy the new bundle beside the old one first and only delete the old
           // one after that copy succeeds, so a failure can't leave us with no app.
           << "if /usr/bin/ditto \"$NEW\" \"$OLD.new\"; then\n"
           << "  /bin/rm -rf \"$OLD\" && /bin/mv \"$OLD.new\" \"$OLD\" && /usr/bin/xattr -cr \"$OLD\" 2>/dev/null && echo \"[$(/bin/date)] swap OK\"\n"
           << "else\n"
           << "  echo \"[$(/bin/date)] ditto FAILED -- left old app in place\"\n"
           << "fi\n"
           << "/usr/bin/open \"$OLD\" && echo \"[$(/bin/date)] relaunched\" || echo \"[$(/bin/date)] open FAILED\"\n"
           // Tidy up the big bits + the plist (NOT swap.sh -- we're running it),
           // then unload this launchd job (that terminates us last; done by then).
           << "/bin/rm -rf \"$STAGING\" \"$ZIP\" \"$PLIST\"\n"
           << "/bin/launchctl bootout gui/$USERID/$LABEL 2>/dev/null\n";

        script.replaceWithText (sh);
        script.setExecutePermission (true);

        // LaunchAgent that runs the script once, the moment launchd loads it.
        auto xmlEsc = [] (const juce::String& s) {
            return s.replace ("&", "&amp;").replace ("<", "&lt;").replace (">", "&gt;");
        };
        juce::String pl;
        pl << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
              "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           << "<plist version=\"1.0\"><dict>\n"
           << "  <key>Label</key><string>" << label << "</string>\n"
           << "  <key>ProgramArguments</key><array>\n"
           << "    <string>/bin/bash</string><string>" << xmlEsc (script.getFullPathName()) << "</string>\n"
           << "  </array>\n"
           << "  <key>RunAtLoad</key><true/>\n"
           << "</dict></plist>\n";
        plist.replaceWithText (pl);

        // Clear any stale job from a previous attempt (ignore failure), then load
        // ours -- launchd runs it immediately (RunAtLoad).
        const juce::String domain = "gui/" + juce::String (uid);

        juce::ChildProcess clear;
        clear.start (juce::StringArray { "/bin/launchctl", "bootout", domain + "/" + label });
        clear.waitForProcessToFinish (5000);

        juce::ChildProcess load;
        load.start (juce::StringArray { "/bin/launchctl", "bootstrap", domain, plist.getFullPathName() });
        load.waitForProcessToFinish (10000);
        if (load.getExitCode() != 0)
        {
            finish (false, "Couldn't schedule the update helper.");
            return;
        }

        // launchd owns the swap now -- quit (clean shutdown releases the audio
        // devices) so the job can replace us and relaunch.
        juce::MessageManager::callAsync ([] {
            if (auto* app = juce::JUCEApplicationBase::getInstance())
                app->systemRequestedQuit();
        });
    }

} // namespace dcr::update
