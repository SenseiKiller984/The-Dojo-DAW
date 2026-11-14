#include "app/SmokeTest.h"

#include "core/rt/RealtimeGuard.h"

#include <cstdio>

namespace dojo::app
{

namespace
{
constexpr int kStepMs = 400; // (time for the window system to configure the window between steps)

juce::String describe(juce::Rectangle<int> r)
{
    return r.toString().replace(" ", ",");
}
} // namespace

SmokeTest::SmokeTest(int durationSeconds, MainWindow& window, juce::ApplicationCommandManager& commands,
                     engine::EngineController& controller, std::function<void(int exitCode)> onFinished)
    : window_(window)
    , commands_(commands)
    , controller_(controller)
    , onFinished_(std::move(onFinished))
    , finishAt_(juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(durationSeconds) * 1000u)
{
    startTimer(kStepMs);
}

SmokeTest::~SmokeTest()
{
    stopTimer();
}

void SmokeTest::expectWindow(bool condition, const juce::String& what)
{
    if (!condition)
        windowFailures_.add(what + " (window " + describe(window_.getBounds()) + ", work area " +
                            describe(window_.getScreenArea(false)) + ", screen " +
                            describe(window_.getScreenArea(true)) + ")");
}

void SmokeTest::pressF11()
{
    // Through the key mappings, as the key itself would.
    if (!commands_.getKeyMappings()->keyPressed(juce::KeyPress(juce::KeyPress::F11Key), &window_))
        windowFailures_.add("F11 is not mapped to a command");
}

bool SmokeTest::stepWindowCheck()
{
    auto& w = window_;
    const auto work = w.getScreenArea(false);
    const auto whole = w.getScreenArea(true);
    switch (step_++)
    {
    case 0: // An ordinary window, smaller than the screen.
        w.setBounds(work.reduced(work.getWidth() / 10, work.getHeight() / 10));
        return true;
    case 1:
        windowed_ = w.getBounds();
        expectWindow(!w.isMaximised() && !w.isFullScreenMode(), "the window started maximised");
        w.maximiseButtonPressed();
        return true;
    case 2:
        expectWindow(w.isMaximised() && w.getBounds() == work, "the square button did not maximise the window");
        w.maximiseButtonPressed();
        return true;
    case 3:
        expectWindow(!w.isMaximised() && w.getBounds() == windowed_,
                     "the square button did not restore the window to " + describe(windowed_));
        pressF11();
        return true;
    case 4:
        expectWindow(w.isFullScreenMode() && w.getBounds() == whole && w.getTitleBarHeight() == 0,
                     "F11 did not make the window full screen");
        pressF11();
        return true;
    case 5:
        expectWindow(!w.isFullScreenMode() && !w.isMaximised() && w.getBounds() == windowed_ &&
                         w.getTitleBarHeight() > 0,
                     "F11 did not bring the window back from full screen to " + describe(windowed_));
        w.maximiseButtonPressed();
        return true;
    case 6:
        pressF11(); // full screen from maximised ...
        return true;
    case 7:
        expectWindow(w.isFullScreenMode() && w.getBounds() == whole, "F11 from maximised did not go full screen");
        pressF11(); // ... and back to maximised
        return true;
    case 8:
        expectWindow(w.isMaximised() && !w.isFullScreenMode() && w.getBounds() == work,
                     "leaving full screen did not go back to maximised");
        w.maximiseButtonPressed();
        return true;
    case 9:
        expectWindow(!w.isMaximised() && w.getBounds() == windowed_,
                     "the window did not restore after full screen and maximise");
        return false;
    default:
        return false;
    }
}

void SmokeTest::timerCallback()
{
    if (stepWindowCheck() || juce::Time::getMillisecondCounter() < finishAt_)
        return;
    stopTimer();

    const auto device = controller_.getDeviceStatus();
    const auto telemetry = controller_.getLastTelemetry();
    const auto frames = window_.getMainComponent().getFrameCount();
    const auto violations = rt::getRealtimeViolationCount();

    juce::StringArray failures;

    if (frames == 0)
        failures.add("the UI rendered no frames");

    if (device.isOpen && telemetry.processedBlocks == 0)
        failures.add("an audio device is open but the audio callback never ran");

    if (violations > 0)
        failures.add(juce::String(static_cast<juce::int64>(violations)) +
                     " real-time violation(s) on the audio thread");

    for (const auto& failure : windowFailures_)
        failures.add("window: " + failure);

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
           << "[smoke-test] Window maximise / restore / full screen: "
           << (windowFailures_.isEmpty() ? "OK" : juce::String(windowFailures_.size()) + " problem(s)") << "\n"
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
