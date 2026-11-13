#include "ui/components/StatusBar.h"

#include "core/Version.h"
#include "ui/Latency.h"
#include "ui/theme/DojoTheme.h"

namespace dojo::ui
{
namespace
{
const char* const kSeparator = "   |   ";
} // namespace

StatusBar::StatusBar()
{
    setOpaque(true);
}

void StatusBar::setInfo(const Info& info)
{
    info_ = info;

    auto device = describeDevice();
    auto engine = describeEngine();

    if (device != deviceText_ || engine != engineText_)
    {
        deviceText_ = std::move(device);
        engineText_ = std::move(engine);
        repaint();
    }
}

juce::String StatusBar::describeDevice() const
{
    const auto& d = info_.device;

    if (!d.isOpen)
        return "No audio device" + (d.lastError.isNotEmpty() ? " - " + d.lastError : juce::String());

    const double latencyMs = d.sampleRate > 0.0 ? 1000.0 * d.outputLatencySamples / d.sampleRate : 0.0;

    juce::String text;
    text << d.deviceType << kSeparator << d.deviceName << kSeparator << juce::String(d.sampleRate / 1000.0, 1)
         << " kHz" << kSeparator << d.bufferSizeSamples << " samples" << kSeparator
         << juce::String(latencyMs, 1) << " ms out" << kSeparator << d.numInputChannels << " in" << kSeparator
         << info_.midiInputs << " MIDI in";

    if (d.driverReportedError)
        text << kSeparator << "DRIVER ERROR";

    return text;
}

juce::String StatusBar::describeEngine() const
{
    juce::String text;

    if (info_.recording)
        text << "RECORDING" << kSeparator;
    if (info_.scanningPlugins)
        text << "Scanning plug-ins" << kSeparator;
    if (info_.latencySamples > 0)
        text << "PDC " << latency::formatMilliseconds(info_.latencySamples, info_.device.sampleRate) << " (" << info_.latencySamples
             << " smp)" << kSeparator;

    if (info_.device.xrunCount >= 0)
        text << "XRuns " << info_.device.xrunCount << kSeparator;

    text << "Peak load " << juce::roundToInt(info_.dspLoadPeak * 100.0f) << "%" << kSeparator;

    if (info_.realtimeTrapInstalled)
        text << "RT violations " << static_cast<juce::int64>(info_.realtimeViolations) << kSeparator;

    if (info_.pendingImports > 0)
        text << "Loading " << info_.pendingImports << (info_.pendingImports == 1 ? " file" : " files")
             << kSeparator;

    if (info_.parkedCommands > 0)
        text << "Commands parked " << static_cast<int>(info_.parkedCommands) << kSeparator;

    text << "v" << version::kVersionString;
    return text;
}

void StatusBar::paint(juce::Graphics& g)
{
    using namespace theme;

    g.fillAll(colour::panel);
    g.setColour(colour::outline);
    g.fillRect(getLocalBounds().removeFromTop(1));

    auto area = getLocalBounds().reduced(12, 0);

    // Device health LED.
    const bool deviceOk = info_.device.isOpen && !info_.device.driverReportedError;
    const auto led = area.removeFromLeft(10).toFloat().withSizeKeepingCentre(7.0f, 7.0f);
    g.setColour(deviceOk ? colour::play : colour::gold);
    g.fillEllipse(led);
    area.removeFromLeft(8);

    g.setFont(sans(11.5f));

    const bool rtProblem = info_.realtimeViolations > 0;
    g.setColour(rtProblem ? colour::meterClip : colour::textMuted);
    const int engineWidth = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), engineText_) + 4;
    g.drawText(engineText_, area.removeFromRight(engineWidth), juce::Justification::centredRight, false);

    g.setColour(deviceOk ? colour::textSecondary : colour::gold);
    g.drawText(deviceText_, area, juce::Justification::centredLeft, true);
}

} // namespace dojo::ui
