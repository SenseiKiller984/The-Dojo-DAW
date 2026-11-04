#include "ui/MainComponent.h"

#include "core/rt/RealtimeGuard.h"
#include "ui/theme/DojoTheme.h"

namespace dojo::ui
{
namespace
{
constexpr double kStatusRefreshIntervalSeconds = 0.25;
constexpr double kMaxFrameDeltaSeconds = 0.1; // clamp after stalls so meters don't jump
} // namespace

MainComponent::MainComponent(engine::EngineController& controller)
    : controller_(controller)
    , transport_(controller)
    , arranger_(controller)
    , mixer_(controller)
    , vblank_(this, [this](double timestampSeconds) { onDisplayRefresh(timestampSeconds); })
{
    setOpaque(true);
    setWantsKeyboardFocus(true);

    addAndMakeVisible(transport_);
    addAndMakeVisible(browser_);
    addAndMakeVisible(arranger_);
    addAndMakeVisible(mixer_);
    addAndMakeVisible(statusBar_);

    refreshStatusBar();
}

MainComponent::~MainComponent() = default;

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(theme::colour::windowBackground);
}

void MainComponent::resized()
{
    using namespace theme::metrics;

    auto area = getLocalBounds();

    transport_.setBounds(area.removeFromTop(transportHeight));
    statusBar_.setBounds(area.removeFromBottom(statusBarHeight));
    browser_.setBounds(area.removeFromLeft(browserWidth));

    const int mixerHeightClamped = juce::jlimit(160, juce::jmax(160, area.getHeight() / 2), mixerHeight);
    mixer_.setBounds(area.removeFromBottom(mixerHeightClamped));
    arranger_.setBounds(area);
}

bool MainComponent::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey)
    {
        controller_.togglePlayback();
        return true;
    }

    if (key == juce::KeyPress::homeKey || key == juce::KeyPress::returnKey)
    {
        controller_.returnToStart();
        return true;
    }

    return false;
}

void MainComponent::onDisplayRefresh(double timestampSeconds)
{
    const double elapsed = lastFrameTimestamp_ > 0.0 ? juce::jlimit(0.0, kMaxFrameDeltaSeconds,
                                                                    timestampSeconds - lastFrameTimestamp_)
                                                     : 1.0 / 60.0;
    lastFrameTimestamp_ = timestampSeconds;
    ++frameCount_;

    const auto& telemetry = controller_.pollTelemetry();
    const double sampleRate = controller_.getSampleRateOr(48000.0);

    transport_.update(telemetry, sampleRate);
    arranger_.setPlayhead(telemetry.playheadSamples, sampleRate);
    mixer_.pushMasterLevels(telemetry.masterPeakLeft, telemetry.masterPeakRight, elapsed);
    loadPeakSinceStatusRefresh_ = juce::jmax(loadPeakSinceStatusRefresh_, telemetry.dspLoadPeak);

    if (timestampSeconds - lastStatusRefresh_ >= kStatusRefreshIntervalSeconds)
    {
        lastStatusRefresh_ = timestampSeconds;
        refreshStatusBar();
    }
}

void MainComponent::refreshStatusBar()
{
    StatusBar::Info info;
    info.device = controller_.getDeviceStatus();
    info.dspLoadPeak = loadPeakSinceStatusRefresh_;
    info.realtimeViolations = rt::getRealtimeViolationCount();
    info.realtimeTrapInstalled = rt::isAllocationTrapInstalled();
    info.clippedSamples = controller_.getLastTelemetry().clippedSamples;
    info.parkedCommands = controller_.getParkedCommandCount();
    statusBar_.setInfo(info);
    loadPeakSinceStatusRefresh_ = 0.0f;
}

} // namespace dojo::ui
