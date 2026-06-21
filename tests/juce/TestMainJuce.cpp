// Console runner for JUCE-dependent D-Router unit tests. Runs every
// statically-registered juce::UnitTest; non-zero exit on any failure (ctest).
#include <juce_core/juce_core.h>

int main()
{
    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);
    runner.runAllTests();

    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
        if (auto* r = runner.getResult (i))
            failures += r->failures;

    juce::Logger::writeToLog (failures == 0 ? "ALL JUCE TESTS PASSED"
                                            : "JUCE TESTS FAILED: " + juce::String (failures));
    return failures == 0 ? 0 : 1;
}
