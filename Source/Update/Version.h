#pragma once

// JUCE-free semantic-version parsing + comparison for the GitHub auto-updater.
// Kept dependency-free so it unit-tests headless in the ctest target and is the
// single source of truth for "is release X newer than what we're running?".

#include <cctype>
#include <string>

namespace dcr::update
{

    struct Version
    {
        int major = 0, minor = 0, patch = 0;
        std::string prerelease; // e.g. "beta", "beta.2"; empty == stable
        bool valid = false; // false if the tag had no leading number
    };

    // Accepts "v1.2.3", "1.2.3-beta", "V0.2.0-beta.1", " 1.2 " (missing minor/patch
    // default to 0).  A leading 'v'/'V'/space is tolerated.  Anything after the first
    // '-' becomes the prerelease string.  valid==false if there's no leading integer.
    inline Version parseVersion (const std::string& s)
    {
        Version v;
        size_t i = 0;
        while (i < s.size() && (s[i] == 'v' || s[i] == 'V' || s[i] == ' '))
            ++i;

        auto readInt = [&] (int& out) -> bool {
            long val = 0;
            bool any = false;
            while (i < s.size() && std::isdigit (static_cast<unsigned char> (s[i])))
            {
                val = val * 10 + (s[i] - '0');
                ++i;
                any = true;
            }
            if (any)
                out = static_cast<int> (val);
            return any;
        };

        if (!readInt (v.major))
            return v; // need at least a major
        if (i < s.size() && s[i] == '.')
        {
            ++i;
            readInt (v.minor);
        }
        if (i < s.size() && s[i] == '.')
        {
            ++i;
            readInt (v.patch);
        }
        if (i < s.size() && s[i] == '-')
            v.prerelease = s.substr (i + 1);
        v.valid = true;
        return v;
    }

    // -1 if a < b, 0 if equal, +1 if a > b.  At an equal major.minor.patch a build
    // WITH a prerelease tag is older than one without (0.2.0-beta < 0.2.0); two
    // prereleases compare by their string.
    inline int compareVersions (const Version& a, const Version& b)
    {
        if (a.major != b.major)
            return a.major < b.major ? -1 : 1;
        if (a.minor != b.minor)
            return a.minor < b.minor ? -1 : 1;
        if (a.patch != b.patch)
            return a.patch < b.patch ? -1 : 1;

        const bool ap = !a.prerelease.empty();
        const bool bp = !b.prerelease.empty();
        if (ap != bp)
            return ap ? -1 : 1; // has-prerelease is older
        if (a.prerelease != b.prerelease)
            return a.prerelease < b.prerelease ? -1 : 1;
        return 0;
    }

    // True only if `candidate` is a strictly newer release than `current`.
    // Conservative: if either tag fails to parse, return false so a garbage tag can
    // never trigger a spurious update prompt.
    inline bool isNewer (const std::string& candidate, const std::string& current)
    {
        const Version c = parseVersion (candidate);
        const Version r = parseVersion (current);
        if (!c.valid || !r.valid)
            return false;
        return compareVersions (c, r) > 0;
    }

} // namespace dcr::update
