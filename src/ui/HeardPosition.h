#pragma once

#include <cmath>

namespace dojo::ui
{

/**
    Where the sound you are hearing is on the timeline, while playing.

    The engine's position is where it renders; what reaches the speakers left the timeline earlier
    by the delay compensation (the latest path through the devices) plus the audio device's own
    output latency. The playhead and the time display show the heard position, so they match the
    sound (and the video window the picture to it):
      - when playback starts, nothing is heard for that delay: the position stays at the start;
      - when the loop wraps, the end of the loop is still sounding for that delay;
      - stopped, it is the engine's position.
*/
class HeardPosition
{
public:
    /** `delaySeconds`: the delay to the speakers in timeline seconds (latency x speed). */
    double update(double enginePosition, bool playing, double delaySeconds, bool loopEnabled, double loopStart, double loopEnd) noexcept
    {
        if (!playing || delaySeconds <= 0.0)
        {
            playing_ = playing;
            start_ = last_ = enginePosition;
            wrapped_ = false;
            return enginePosition;
        }

        if (!playing_)
        {
            // Just started: from where it stood (the engine may already have moved on a little).
            playing_ = true;
            start_ = last_ <= enginePosition && enginePosition - last_ < delaySeconds + 0.25 ? last_ : enginePosition;
            wrapped_ = false;
        }
        else if (enginePosition < last_ - 1.0e-9)
        {
            // It jumped back: the loop wrapped (its end is still sounding for a moment), or a locate.
            wrapped_ = loopEnabled && loopEnd > loopStart && std::abs(enginePosition - loopStart) < delaySeconds + 0.25;
            start_ = wrapped_ ? loopStart : enginePosition;
        }
        last_ = enginePosition;

        double heard = enginePosition - delaySeconds;
        if (heard < start_)
            heard = wrapped_ ? heard + (loopEnd - loopStart) : start_;
        return heard;
    }

private:
    bool playing_ = false;
    bool wrapped_ = false;
    double start_ = 0.0;
    double last_ = 0.0;
};

} // namespace dojo::ui
