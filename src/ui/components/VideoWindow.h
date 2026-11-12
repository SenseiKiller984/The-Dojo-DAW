#pragma once

#include "session/Session.h"
#include "video/FrameServer.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>

namespace dojo::ui
{

/**
    The picture: the frame of the video track at the playhead (the topmost active, unmuted video
    track with a clip there), letterboxed on black, with the timecode of that frame (hh:mm:ss:ff at
    the video's frame rate) and the clip's name. Follows the transport while playing, locating and
    scrubbing; frames are decoded in the background, so it never holds up the interface.
*/
class VideoView final : public juce::Component, private juce::Timer, private juce::ChangeListener
{
public:
    VideoView(const session::Session& session, video::FrameServer& frames, std::function<double()> playheadSeconds);
    ~VideoView() override;

    /** Looks up the frame for the playhead now (also done 30 times a second). */
    void refresh();

    /** What is on screen (for tests). */
    [[nodiscard]] const juce::Image& getCurrentFrame() const noexcept { return frame_; }
    [[nodiscard]] juce::String getTimecode() const { return timecode_; }
    [[nodiscard]] juce::String getClipName() const { return clipName_; }

    void paint(juce::Graphics& g) override;

private:
    void timerCallback() override { refresh(); }
    void changeListenerCallback(juce::ChangeBroadcaster*) override { refresh(); }

    const session::Session& session_;
    video::FrameServer& frames_;
    std::function<double()> playheadSeconds_;
    juce::Image frame_;
    juce::String timecode_;
    juce::String clipName_;
    bool hasVideo_ = false;
};

/** A timecode for `seconds` at `frameRate`: hh:mm:ss:ff. */
[[nodiscard]] juce::String formatTimecode(double seconds, double frameRate);
/** Seconds from a timecode "hh:mm:ss:ff" (also "mm:ss:ff", "ss:ff" or "hh:mm:ss.ff"; any of
    : ; . as separators) at `frameRate`; nullopt if it is not one. */
[[nodiscard]] std::optional<double> parseTimecode(const juce::String& text, double frameRate);

/** The floating Video window (View > Video, Ctrl+Shift+V). Closing it only hides it. */
class VideoWindow final : public juce::DocumentWindow
{
public:
    VideoWindow(const session::Session& session, video::FrameServer& frames, std::function<double()> playheadSeconds);

    [[nodiscard]] VideoView& getView() noexcept { return *view_; }
    void closeButtonPressed() override;

    /** Called when the window is hidden with its close button. */
    std::function<void()> onClosed;

private:
    VideoView* view_ = nullptr;
};

} // namespace dojo::ui
