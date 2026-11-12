#include "ui/components/VideoWindow.h"

#include "ui/EditActions.h"
#include "ui/theme/DojoTheme.h"

#include <cmath>

namespace dojo::ui
{

juce::String formatTimecode(double seconds, double frameRate)
{
    const int fps = std::max(1, juce::roundToInt(frameRate));
    const auto frames = static_cast<long long>(std::floor(std::max(0.0, seconds) * frameRate + 1.0e-6));
    const auto totalSeconds = frames / fps;
    return juce::String::formatted("%02lld:%02lld:%02lld:%02lld", totalSeconds / 3600, (totalSeconds / 60) % 60, totalSeconds % 60,
                                   frames % fps);
}

VideoView::VideoView(const session::Session& session, video::FrameServer& frames, std::function<double()> playheadSeconds)
    : session_(session)
    , frames_(frames)
    , playheadSeconds_(std::move(playheadSeconds))
{
    setOpaque(true);
    frames_.addChangeListener(this);
    startTimerHz(30);
}

VideoView::~VideoView()
{
    frames_.removeChangeListener(this);
}

void VideoView::refresh()
{
    const double seconds = playheadSeconds_ ? playheadSeconds_() : 0.0;
    const auto found = actions::findVideoAt(session_, seconds);
    juce::Image image;
    juce::String timecode;
    juce::String name;
    if (found.clip.isValid())
    {
        image = frames_.requestFrame(found.file, found.fileSeconds, 0, 0);
        const auto info = frames_.getInfo(found.file);
        timecode = formatTimecode(seconds, info.has_value() ? info->frameRate : 25.0);
        name = session::clip::getName(found.clip);
    }

    const bool changed = image != frame_ || timecode != timecode_ || name != clipName_ || found.clip.isValid() != hasVideo_;
    frame_ = image;
    timecode_ = timecode;
    clipName_ = name;
    hasVideo_ = found.clip.isValid();
    if (changed)
        repaint();
}

void VideoView::paint(juce::Graphics& g)
{
    using namespace theme;
    g.fillAll(juce::Colours::black);

    if (!hasVideo_)
    {
        g.setColour(colour::textMuted);
        g.setFont(sans(13.0f));
        g.drawText("No video at the playhead", getLocalBounds(), juce::Justification::centred);
        return;
    }

    if (frame_.isValid())
        g.drawImage(frame_, getLocalBounds().toFloat(), juce::RectanglePlacement::centred);

    // Timecode and clip name, on dark tabs so they read on any picture.
    g.setFont(mono(13.0f));
    const auto tc = getLocalBounds().removeFromBottom(26).removeFromLeft(118).reduced(4);
    g.setColour(juce::Colours::black.withAlpha(0.6f));
    g.fillRoundedRectangle(tc.toFloat(), 3.0f);
    g.setColour(juce::Colours::white);
    g.drawText(timecode_, tc, juce::Justification::centred);
    g.setFont(sans(11.0f));
    g.setColour(juce::Colours::white.withAlpha(0.75f));
    g.drawText(clipName_, getLocalBounds().removeFromTop(20).reduced(6, 2), juce::Justification::centredLeft, true);
}

//==================================================================================================

VideoWindow::VideoWindow(const session::Session& session, video::FrameServer& frames, std::function<double()> playheadSeconds)
    : juce::DocumentWindow("Video", juce::Colours::black, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton)
{
    auto view = std::make_unique<VideoView>(session, frames, std::move(playheadSeconds));
    view_ = view.get();
    view->setSize(640, 360);
    setContentOwned(view.release(), true);
    setUsingNativeTitleBar(true);
    setResizable(true, true);
    setResizeLimits(160, 90, 3840, 2160);
    setAlwaysOnTop(false);
}

void VideoWindow::closeButtonPressed()
{
    setVisible(false);
    if (onClosed)
        onClosed();
}

} // namespace dojo::ui
