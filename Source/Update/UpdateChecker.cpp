#include "Update/UpdateChecker.h"
#include "Update/Version.h"

namespace dcr::update
{

void UpdateChecker::run()
{
    // Deliver outcomes on the message thread.  std::function must stay copyable,
    // so the "found" path hands the ReleaseInfo over as a released raw pointer and
    // re-takes ownership inside the lambda.
    auto report = [this] (std::unique_ptr<ReleaseInfo> info, bool ok)
    {
        auto* raw = info.release();
        juce::MessageManager::callAsync ([cb = callback, raw, ok]
        {
            std::unique_ptr<ReleaseInfo> owned (raw);
            if (cb) cb (std::move (owned), ok);
        });
    };

    juce::URL api ("https://api.github.com/repos/ZDAudio/D-router/releases");
    api = api.withParameter ("per_page", "20");

    const auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                          .withConnectionTimeoutMs (10000)
                          .withExtraHeaders ("User-Agent: D-Router-Updater\r\n"
                                             "Accept: application/vnd.github+json");

    std::unique_ptr<juce::InputStream> stream (api.createInputStream (opts));
    if (threadShouldExit()) return;
    if (stream == nullptr) { report (nullptr, false); return; }    // network/HTTP failure

    const juce::String body = stream->readEntireStreamAsString();
    if (threadShouldExit()) return;

    const juce::var json = juce::JSON::parse (body);
    const juce::Array<juce::var>* releases = json.getArray();
    if (releases == nullptr) { report (nullptr, false); return; }  // not the expected JSON

    // GitHub returns releases newest-first; take the first non-draft release that
    // ships a D-Router.zip asset (pre-releases included by design).
    for (const auto& rel : *releases)
    {
        if (static_cast<bool> (rel.getProperty ("draft", false))) continue;

        const juce::String tag = rel.getProperty ("tag_name", "").toString();
        if (tag.isEmpty()) continue;

        juce::URL   zipUrl;
        juce::int64 zipSize = 0;
        bool        haveAsset = false;

        if (const auto* assets = rel.getProperty ("assets", juce::var()).getArray())
            for (const auto& a : *assets)
                if (a.getProperty ("name", "").toString() == "D-Router.zip")
                {
                    zipUrl    = juce::URL (a.getProperty ("browser_download_url", "").toString());
                    zipSize   = static_cast<juce::int64> (a.getProperty ("size", juce::var ((juce::int64) 0)));
                    haveAsset = true;
                    break;
                }

        if (! haveAsset) continue;        // release without our zip -> not installable

        if (! isNewer (tag.toStdString(), current.toStdString()))
        {
            report (nullptr, true);       // newest usable release is not newer -> up to date
            return;
        }

        auto info     = std::make_unique<ReleaseInfo>();
        info->tag     = tag;
        info->name    = rel.getProperty ("name", tag).toString();
        info->notes   = rel.getProperty ("body", "").toString();
        info->zipUrl  = zipUrl;
        info->zipSize = zipSize;
        report (std::move (info), true);
        return;
    }

    report (nullptr, true);               // no usable release found -> up to date
}

} // namespace dcr::update
