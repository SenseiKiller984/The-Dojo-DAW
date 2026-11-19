#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>

namespace dojo::devices
{
class TapeA800;
}

namespace dojo::session
{
class Session;
}

namespace dojo::ui
{

namespace tape
{
/** NAB reels with 14-inch flanges (inches). */
inline constexpr double kHubRadius = 2.25;
inline constexpr double kFullRadius = 6.75; ///< the pack of a full reel
inline constexpr double kFlangeRadius = 7.0;
/** A standard reel: 2,500 feet. */
inline constexpr double kStandardReelFeet = 2500.0;

/** Both reels at one point of the tape. */
struct Reels
{
    double wound = 0.0;                                      ///< the share of the tape on the take-up (right) reel, 0 .. 1
    double leftRadius = kFullRadius, rightRadius = kHubRadius; ///< the packs (inches)
    double leftAngle = 0.0, rightAngle = 0.0;                ///< how far each reel has turned since the head of the tape (radians, clockwise)
};

/**
    The reels `seconds` into a tape of `lengthSeconds` running at `ips` inches per second. The tape
    starts wound on the left (supply) reel and ends on the right (take-up) one; each pack holds its
    share of the tape's area, so the packs' radii follow the square root, and each reel turns at the
    tape speed over its own radius (a small pack spins faster), both clockwise.
*/
[[nodiscard]] Reels reelsAt(double seconds, double lengthSeconds, double ips) noexcept;

/** How long a standard reel plays at `ips`. */
[[nodiscard]] double standardReelSeconds(double ips) noexcept;

/**
    The tape the machine carries, in seconds: exactly as long as what it plays. On the master: the
    whole arrangement; on a track: up to the end of its material (or of what feeds it, see
    Session::getContentEnd). With nothing there yet: the arrangement, and in an empty session a
    standard reel.
*/
[[nodiscard]] double lengthFor(const session::Session& session, const juce::ValueTree& owner, double ips);
} // namespace tape

/**
    The A800's deck: its two reels, the tape threaded past the guides, the heads and the capstan,
    the tape timer, the transport lamps and the meter bridge.

    The tape follows the transport: at the start it is all on the left reel, at the end of what the
    machine plays (see tape::lengthFor) all on the right. Playing, the reels turn at the tape speed
    (the emptier reel faster); a locate winds them there (fast forward or rewind, the flanges a blur)
    and the timer shows where the tape is. Past the end, the END lamp lights.
*/
class TapeDeck final : public juce::Component, private juce::Timer
{
public:
    enum class Motion
    {
        stopped,
        playing,
        recording,
        rewinding,
        fastForward
    };

    /** `length` gives the tape's length in seconds (asked four times a second). */
    TapeDeck(devices::TapeA800& tape, std::function<double()> length);

    /** Moves the deck on by `elapsedSeconds` (the timer does this 30 times a second; tests call it). */
    void update(double elapsedSeconds);

    [[nodiscard]] Motion getMotion() const noexcept { return motion_; }
    /** Where the tape shows (seconds): the transport's time, or on the way there while winding. */
    [[nodiscard]] double getShownSeconds() const noexcept { return shown_; }
    [[nodiscard]] double getTapeLength() const noexcept { return length_; }
    [[nodiscard]] const tape::Reels& getReels() const noexcept { return reels_; }
    /** The flanges' motion blur (0: sharp .. 1: a disc), per reel. */
    [[nodiscard]] float getBlur(int reel) const noexcept { return blur_[reel == 0 ? 0 : 1]; }
    [[nodiscard]] bool isAtEnd() const noexcept;
    /** The tape timer's text ("01:23.4"). */
    [[nodiscard]] static juce::String timerText(double seconds);

    void paint(juce::Graphics& g) override;

private:
    void timerCallback() override { update(1.0 / 30.0); }
    [[nodiscard]] double ips() const noexcept;
    [[nodiscard]] int speed() const noexcept;
    void paintDeck(juce::Graphics& g, juce::Rectangle<float> area) const;
    void paintReel(juce::Graphics& g, juce::Point<float> centre, float flange, double packInches, double angle, float blur) const;
    void paintMeterBridge(juce::Graphics& g, juce::Rectangle<float> area) const;

    devices::TapeA800& tape_;
    std::function<double()> length_fn_;
    double length_ = 1.0;
    double lengthCountdown_ = 0.0;
    double shown_ = 0.0;
    bool started_ = false;
    Motion motion_ = Motion::stopped;
    tape::Reels reels_;
    std::array<float, 2> blur_{0.0f, 0.0f};
    std::array<double, 2> vu_{-60.0, -60.0};
};

} // namespace dojo::ui
