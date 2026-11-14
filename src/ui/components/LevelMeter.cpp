#include "ui/components/LevelMeter.h"

#include <set>

#include "ui/theme/DojoTheme.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>

namespace dojo::ui
{
namespace
{
constexpr float kFallRateDbPerSecond = 24.0f;
constexpr double kHoldSeconds = 1.2;
constexpr float kClipIndicatorHeight = 5.0f;
} // namespace

namespace
{
/** Every meter alive (message thread only), so one command can clear all their clip indicators. */
std::set<LevelMeter*>& allMeters()
{
    static std::set<LevelMeter*> meters;
    return meters;
}
} // namespace

LevelMeter::LevelMeter()
{
    setOpaque(false);
    setTooltip("Master output peak level. Click to reset the clip indicators.");
    allMeters().insert(this);
}

LevelMeter::~LevelMeter()
{
    allMeters().erase(this);
}

bool LevelMeter::isClipLit() const noexcept
{
    for (int i = 0; i < numChannels_; ++i)
        if (channels_[static_cast<std::size_t>(i)].clipped)
            return true;
    return false;
}

void LevelMeter::clearAllClipIndicators()
{
    for (auto* meter : allMeters())
        meter->resetClipIndicators();
}

float LevelMeter::decibelsToProportion(float decibels) noexcept
{
    return juce::jlimit(0.0f, 1.0f, (decibels - kMinDb) / (kMaxDb - kMinDb));
}

void LevelMeter::update(float peakLeft, float peakRight, double elapsedSeconds)
{
    const std::array<float, 2> peaks{peakLeft, peakRight};
    update(peaks.data(), 2, elapsedSeconds);
}

void LevelMeter::update(const float* peaks, int numChannels, double elapsedSeconds)
{
    numChannels = juce::jlimit(1, kMaxChannels, numChannels);
    const auto fall = static_cast<float>(kFallRateDbPerSecond * elapsedSeconds);
    bool changed = false;
    if (numChannels != numChannels_)
    {
        numChannels_ = numChannels;
        changed = true;
    }

    for (std::size_t i = 0; i < static_cast<std::size_t>(numChannels_); ++i)
    {
        auto& ch = channels_[i];
        const float peakDb = juce::Decibels::gainToDecibels(peaks[i], kMinDb);
        const auto previous = ch.displayDb;

        ch.displayDb = juce::jmax(peakDb, ch.displayDb - fall);

        if (peakDb >= ch.holdDb)
        {
            ch.holdDb = peakDb;
            ch.holdAgeSeconds = 0.0;
        }
        else
        {
            ch.holdAgeSeconds += elapsedSeconds;
            if (ch.holdAgeSeconds > kHoldSeconds)
                ch.holdDb = juce::jmax(kMinDb, ch.holdDb - fall);
        }

        if (peaks[i] > 1.0f && !ch.clipped)
        {
            ch.clipped = true;
            changed = true;
        }

        changed = changed || std::abs(previous - ch.displayDb) > 0.01f || ch.holdDb > kMinDb;
    }

    if (changed)
        repaint();
}

void LevelMeter::resetClipIndicators()
{
    for (auto& ch : channels_)
        ch.clipped = false;

    repaint();
}

void LevelMeter::mouseDown(const juce::MouseEvent&)
{
    resetClipIndicators();
}

void LevelMeter::paint(juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();
    const float gap = numChannels_ > 2 ? 1.0f : 2.0f;
    const auto n = static_cast<float>(numChannels_);
    const float channelWidth = (area.getWidth() - gap * (n - 1.0f)) / n;

    for (int i = 0; i < numChannels_; ++i)
    {
        paintChannel(g, area.removeFromLeft(channelWidth), channels_[static_cast<std::size_t>(i)]);
        area.removeFromLeft(gap);
    }
}

void LevelMeter::paintChannel(juce::Graphics& g, juce::Rectangle<float> area, const Channel& channel) const
{
    using namespace theme;

    auto clipArea = area.removeFromTop(kClipIndicatorHeight);
    area.removeFromTop(2.0f);

    g.setColour(channel.clipped ? colour::meterClip : colour::meterBackground);
    g.fillRoundedRectangle(clipArea, 1.5f);

    g.setColour(colour::meterBackground);
    g.fillRoundedRectangle(area, 2.0f);

    // Gradient spans the full scale so that colour encodes absolute level, not bar length.
    const auto yFor = [&area](float db)
    {
        return area.getBottom() - area.getHeight() * decibelsToProportion(db);
    };

    // Gradient positions run bottom (0) -> top (1), i.e. exactly decibelsToProportion().
    juce::ColourGradient gradient(colour::meterLow, 0.0f, area.getBottom(), colour::meterClip, 0.0f,
                                  area.getY(), false);
    gradient.addColour(decibelsToProportion(-18.0f), colour::meterLow);
    gradient.addColour(decibelsToProportion(-9.0f), colour::meterMid);
    gradient.addColour(decibelsToProportion(-3.0f), colour::meterHigh);
    gradient.addColour(decibelsToProportion(0.0f), colour::meterClip);

    const float levelY = yFor(channel.displayDb);
    if (channel.displayDb > kMinDb)
    {
        g.setGradientFill(gradient);
        g.fillRect(area.withTop(levelY));
    }

    // 0 dBFS reference line.
    g.setColour(colour::outlineStrong);
    g.fillRect(area.getX(), yFor(0.0f), area.getWidth(), 1.0f);

    if (channel.holdDb > kMinDb)
    {
        g.setColour(gradient.getColourAtPosition(decibelsToProportion(channel.holdDb)));
        g.fillRect(area.getX(), yFor(channel.holdDb) - 1.0f, area.getWidth(), 2.0f);
    }
}

} // namespace dojo::ui
