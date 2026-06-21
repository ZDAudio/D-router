#include "Update/UpdateInstaller.h"

#include <cstdlib> // std::system
#include <unistd.h> // getpid

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
           << "while kill -0 \"$PID\" 2>/dev/null; do sleep 0.2; done\n"
           << "sleep 0.3\n"
           << "xattr -cr \"$NEW\" 2>/dev/null\n"
           // Copy the new bundle beside the old one first and only delete the old one
           // after that copy succeeds, so a failure can never leave us with no app.
           << "if ditto \"$NEW\" \"$OLD.new\"; then\n"
           << "  rm -rf \"$OLD\" && mv \"$OLD.new\" \"$OLD\" && xattr -cr \"$OLD\" 2>/dev/null\n"
           << "fi\n"
           << "open \"$OLD\"\n"
           << "rm -rf \"$STAGING\" \"$ZIP\" \"$SELF\"\n";

        script.replaceWithText (sh);
        script.setExecutePermission (true);

        // Detach so it outlives our exit; then quit (real quit -> shutdown releases
        // the audio devices, and the script waits for the PID before swapping).
        const juce::String cmd = "nohup /bin/bash " + shQuote (script.getFullPathName())
                                 + " >/dev/null 2>&1 &";
        std::system (cmd.toRawUTF8());

        juce::MessageManager::callAsync ([] {
            if (auto* app = juce::JUCEApplicationBase::getInstance())
                app->systemRequestedQuit();
        });
    }

} // namespace dcr::update
