#include "app/SmokeTest.h"

#include "core/rt/RealtimeGuard.h"

#include <cstdio>

namespace dojo::app
{

SmokeTest::SmokeTest(int durationSeconds, const ui::MainComponent& mainComponent,
                     engine::EngineController& controller, std::function<void(int exitCode)> onFinished)
    : mainComponent_(mainComponent)
    , controller_(controller)
    , onFinished_(std::move(onFinished))
{
    startTimer(durationSeconds * 1000);
}

SmokeTest::~SmokeTest()
{
    stopTimer();
}

void SmokeTest::timerCallback()
{
    stopTimer();

    const auto device = controller_.getDeviceStatus();
    const auto telemetry = controller_.getLastTelemetry();
    const auto frames = mainComponent_.getFrameCount();
    const auto violations = rt::getRealtimeViolationCount();

    juce::StringArray failures;

    if (frames == 0)
        failures.add("the UI rendered no frames");

    if (device.isOpen && telemetry.processedBlocks == 0)
        failures.add("an audio device is open but the audio callback never ran");

    if (violations > 0)
        failures.add(juce::String(static_cast<juce::int64>(violations)) +
                     " real-time violation(s) on the audio thread");

    juce::String report;
    report << "[smoke-test] UI frames: " << static_cast<juce::int64>(frames) << "\n"
           << "[smoke-test] Audio device: "
           << (device.isOpen ? device.deviceType + " / " + device.deviceName + " @ " +
                                   juce::String(device.sampleRate) + " Hz"
                             : "none (" + device.lastError + ")")
           << "\n"
           << "[smoke-test] Audio blocks processed: " << static_cast<juce::int64>(telemetry.processedBlocks)
           << "\n"
           << "[smoke-test] RT allocation trap: "
           << (rt::isAllocationTrapInstalled() ? "installed" : "not installed")
           << ", violations: " << static_cast<juce::int64>(violations) << "\n"
           << "[smoke-test] Result: "
           << (failures.isEmpty() ? "PASS" : "FAIL - " + failures.joinIntoString("; "));

    // The log file is what CI reads on Windows (a GUI-subsystem app has no stdout there). Debug
    // builds already echo log lines to the console, so only optimised builds also print to stdout.
    juce::Logger::writeToLog(report);
#if !JUCE_DEBUG
    std::fprintf(stdout, "%s\n", report.toRawUTF8());
    std::fflush(stdout);
#endif

    onFinished_(failures.isEmpty() ? 0 : 1);
}

} // namespace dojo::app
