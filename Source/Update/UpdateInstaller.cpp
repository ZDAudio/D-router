#include "Update/UpdateInstaller.h"

#include <crt_externs.h> // _NSGetEnviron
#include <fcntl.h> // O_RDONLY / O_WRONLY
#include <spawn.h> // posix_spawn, POSIX_SPAWN_SETSID
#include <unistd.h> // getpid, STDIN_FILENO ...

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

        // --- 3. write + launch the detached swap script --------------------------
        const auto oldApp = appBundle();
        const int pid = (int) ::getpid();
        const auto script = dir.getChildFile ("swap.sh");

        juce::String sh;
        sh << "#!/bin/bash\n"
           << "PID=" << pid << "\n"
           << "OLD=" << shQuote (oldApp.getFullPathName()) << "\n"
           << "NEW=" << shQuote (newApp.getFullPathName()) << "\n"
           << "STAGING=" << shQuote (staging.getFullPathName()) << "\n"
           << "ZIP=" << shQuote (zip.getFullPathName()) << "\n"
           << "SELF=\"$0\"\n"
           // Log every step so a half-applied update is diagnosable.  pgid here
           // should equal the helper's own pid -- if it doesn't, we're still in
           // the app's process group (the old bug that got the helper killed).
           << "LOG=\"$HOME/Library/Logs/D-Router/update-swap.log\"\n"
           << "mkdir -p \"$(dirname \"$LOG\")\"\n"
           << "exec >>\"$LOG\" 2>&1\n"
           << "echo \"===== [$(date)] swap start pid=$$ watch=$PID pgid=$(ps -o pgid= -p $$ | tr -d ' ') =====\"\n"
           << "while kill -0 \"$PID\" 2>/dev/null; do sleep 0.2; done\n"
           << "echo \"[$(date)] target app exited; swapping $OLD\"\n"
           << "sleep 0.3\n"
           << "xattr -cr \"$NEW\" 2>/dev/null\n"
           // Copy the new bundle beside the old one first and only delete the old one
           // after that copy succeeds, so a failure can never leave us with no app.
           << "if ditto \"$NEW\" \"$OLD.new\"; then\n"
           << "  rm -rf \"$OLD\" && mv \"$OLD.new\" \"$OLD\" && xattr -cr \"$OLD\" 2>/dev/null && echo \"[$(date)] swap OK\"\n"
           << "else\n"
           << "  echo \"[$(date)] ditto FAILED -- left old app in place\"\n"
           << "fi\n"
           << "open \"$OLD\" && echo \"[$(date)] relaunched\" || echo \"[$(date)] open FAILED\"\n"
           << "rm -rf \"$STAGING\" \"$ZIP\" \"$SELF\"\n";

        script.replaceWithText (sh);
        script.setExecutePermission (true);

        // Launch the swap helper in its OWN session (POSIX_SPAWN_SETSID) so it
        // outlives our exit.  A plain `nohup ... &` leaves it in our session /
        // process group, where macOS kills it the instant we terminate -- before
        // it can swap the bundle or relaunch (the bug: app quit, no swap, no
        // relaunch).  setsid detaches it; stdio goes to /dev/null so it holds
        // none of our descriptors.
        const auto scriptPath = script.getFullPathName();
        const char* argv[] = { "/bin/bash", scriptPath.toRawUTF8(), nullptr };

        posix_spawnattr_t attr;
        posix_spawnattr_init (&attr);
        posix_spawnattr_setflags (&attr, POSIX_SPAWN_SETSID);

        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init (&fa);
        posix_spawn_file_actions_addopen (&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen (&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen (&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

        pid_t child = 0;
        const int rc = posix_spawn (&child, "/bin/bash", &fa, &attr, const_cast<char* const*> (argv), *_NSGetEnviron());

        posix_spawn_file_actions_destroy (&fa);
        posix_spawnattr_destroy (&attr);

        if (rc != 0)
        {
            finish (false, "Couldn't start the update helper.");
            return;
        }

        // Helper is detached and waiting on our PID -- now really quit (clean
        // shutdown releases the audio devices) so it can perform the swap.
        juce::MessageManager::callAsync ([] {
            if (auto* app = juce::JUCEApplicationBase::getInstance())
                app->systemRequestedQuit();
        });
    }

} // namespace dcr::update
