#pragma once

#include <string>

// JUCE-free recorder helpers: output-file naming + record channel count.  Kept
// dependency-free so the pure-logic test target (dcorerouter_tests) covers them
// without linking JUCE.  The processor supplies the wall-clock fields (from
// juce::Time, message thread) and the format index; collision avoidance is the
// caller's job (juce::File::getNonexistentSibling).
namespace dcr::recorder
{
    // Format index -> lower-case extension (no dot).  0 = WAV, 1 = FLAC,
    // 2 = AAC (.m4a).  Out-of-range clamps to wav.
    inline std::string extensionForFormat (int formatIndex)
    {
        switch (formatIndex)
        {
            case 1:
                return "flac";
            case 2:
                return "m4a";
            default:
                return "wav";
        }
    }

    // Replace anything outside [A-Za-z0-9 _-] with '_' (so '.', '/', etc. all
    // become '_'), then trim surrounding spaces and underscores; fall back to the
    // default when nothing usable is left.  A leading/trailing dot in the input is
    // thus dropped too -- via the '_' it was turned into.
    inline std::string sanitizePrefix (const std::string& raw,
        const std::string& fallback = "Recording")
    {
        std::string out;
        out.reserve (raw.size());
        for (char c : raw)
        {
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                            || (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '-';
            out += ok ? c : '_';
        }
        // By here 'out' contains only the allow-list, so trimming spaces and
        // underscores covers every surrounding char a replacement could produce.
        const auto isTrim = [] (char c) { return c == ' ' || c == '_'; };
        std::size_t b = 0, e = out.size();
        while (b < e && isTrim (out[b]))
            ++b;
        while (e > b && isTrim (out[e - 1]))
            --e;
        out = out.substr (b, e - b);
        return out.empty() ? fallback : out;
    }

    // "<sanitized prefix>_YYYY-MM-DD_HH-MM-SS.<ext>".  month/day are 1-based; all
    // time fields must be non-negative (juce::Time's accessors always are).
    inline std::string makeFileName (const std::string& prefix,
        int year,
        int month,
        int day,
        int hour,
        int minute,
        int second,
        int formatIndex)
    {
        const auto p2 = [] (int v) {
            std::string s = std::to_string (v);
            return v < 10 ? "0" + s : s;
        };
        return sanitizePrefix (prefix) + "_"
               + std::to_string (year) + "-" + p2 (month) + "-" + p2 (day) + "_"
               + p2 (hour) + "-" + p2 (minute) + "-" + p2 (second) + "."
               + extensionForFormat (formatIndex);
    }

    // How many channels a take is written with.  A per-channel insert runs in a
    // "mono host" (PluginHost) that duplicates the mono signal across the stereo
    // scratch (L == R); collapse that to a single true mono channel.  A group
    // host presents its true N-channel buffer -> record it as-is.  Floors at 1 so
    // a take is never opened with zero channels.
    inline int recordChannelCount (bool monoHost, int presentedChannels)
    {
        if (monoHost)
            return 1;
        return presentedChannels > 0 ? presentedChannels : 1;
    }
} // namespace dcr::recorder
