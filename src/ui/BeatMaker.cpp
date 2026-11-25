#include "ui/BeatMaker.h"

#include "session/Session.h"

#include <algorithm>
#include <cmath>

namespace dojo::ui
{
namespace
{
constexpr int kPollMs = 30;

double nowMs()
{
    return juce::Time::getMillisecondCounterHiRes();
}

/** The per-track speed a clip's content plays at (as the graph compiler gives the engine). */
double rateOf(const juce::ValueTree& clip)
{
    const auto track = clip.getParent();
    return track.hasType(session::ids::track) ? std::max(session::kMinStretch, session::track::getSpeed(track)) : 1.0;
}
} // namespace

BeatMaker::BeatMaker(session::Session& session, Host host)
    : session_(session)
    , host_(std::move(host))
{
}

BeatMaker::~BeatMaker()
{
    stopTimer();
}

int BeatMaker::padForKey(const juce::KeyPress& key) noexcept
{
    const auto modifiers = key.getModifiers();
    if (modifiers.isCtrlDown() || modifiers.isAltDown() || modifiers.isCommandDown())
        return -1;
    auto letter = static_cast<int>(key.getKeyCode());
    if (letter >= 'a' && letter <= 'z')
        letter += 'A' - 'a';
    if (letter < 'A' || letter > 'Z')
    {
        letter = static_cast<int>(juce::CharacterFunctions::toUpperCase(key.getTextCharacter()));
        if (letter < 'A' || letter > 'Z')
            return -1;
    }
    const int pad = letter - 'A';
    return pad < kPads ? pad : -1;
}

juce::String BeatMaker::keyForPad(int pad)
{
    return pad >= 0 && pad < kPads ? juce::String::charToString(static_cast<juce::juce_wchar>('A' + pad)) : juce::String();
}

std::optional<double> BeatMaker::contentBeatAt(const juce::ValueTree& midiClip, const time::TempoMap& map, double seconds, double rate)
{
    namespace clip = session::clip;
    if (!midiClip.hasType(session::ids::midiClip))
        return std::nullopt;
    const double start = clip::getStart(midiClip);
    const double end = clip::getEnd(midiClip);
    if (seconds < start || seconds >= end)
        return std::nullopt;
    // As the engine plays it: a loop iteration is the same number of timeline beats each time.
    const double startBeat = map.beatAt(start);
    double beats = map.beatAt(seconds) - startBeat;
    if (clip::isLooped(midiClip))
    {
        const double loop = clip::getLoopLength(midiClip);
        const double loopBeats = map.beatAt(start + loop) - startBeat;
        if (loop > 0.0 && loop < end - start - 1.0e-9 && loopBeats > 0.0)
            beats -= std::floor(beats / loopBeats + 1.0e-9) * loopBeats;
    }
    return clip::getOffset(midiClip) + std::max(0.0, beats) * rate;
}

bool BeatMaker::isHeld(int pad) const noexcept
{
    return pad >= 0 && pad < kPads && held_[static_cast<std::size_t>(pad)].down;
}

void BeatMaker::padDown(int pad, bool accent, int keyCode)
{
    if (pad < 0 || pad >= kPads || held_[static_cast<std::size_t>(pad)].down)
        return;
    auto& held = held_[static_cast<std::size_t>(pad)];
    held = {};
    held.down = true;
    held.keyCode = keyCode;
    held.velocity = accent ? kAccentVelocity : kVelocity;
    held.trackId = host_.getTargetTrack ? host_.getTargetTrack() : 0u;
    if (held.trackId == 0)
    {
        if (host_.onMessage)
            host_.onMessage("Beat Maker: select the track with the Drum Rack to play it");
    }
    else if (host_.sendMidi)
    {
        host_.sendMidi(held.trackId, juce::MidiMessage::noteOn(1, kFirstNote + pad, static_cast<juce::uint8>(held.velocity)));
    }

    // Recording: where the note starts in the selected clip.
    const auto clip = host_.getRecordingClip ? host_.getRecordingClip() : juce::ValueTree();
    const auto position = clip.isValid() && host_.getPlayheadSeconds ? host_.getPlayheadSeconds() : std::nullopt;
    if (clip.isValid() && position.has_value())
    {
        if (const auto beat = contentBeatAt(clip, session_.getTempoMap(), *position, rateOf(clip)))
        {
            held.clip = clip;
            held.startBeat = *beat;
            held.startSeconds = *position;
            held.startMs = nowMs();
        }
        else if (host_.onMessage)
        {
            host_.onMessage("Beat Maker: the playhead is outside the selected clip, so nothing is recorded");
        }
    }

    if (keyCode != 0 && !isTimerRunning())
        startTimer(kPollMs);
}

void BeatMaker::padUp(int pad)
{
    if (pad < 0 || pad >= kPads || !held_[static_cast<std::size_t>(pad)].down)
        return;
    const auto held = held_[static_cast<std::size_t>(pad)];
    held_[static_cast<std::size_t>(pad)] = {};
    if (held.trackId != 0 && host_.sendMidi)
        host_.sendMidi(held.trackId, juce::MidiMessage::noteOff(1, kFirstNote + pad));
    if (held.clip.isValid())
        record(held, pad);
}

void BeatMaker::releaseAll()
{
    for (int pad = 0; pad < kPads; ++pad)
        padUp(pad);
}

void BeatMaker::record(const Held& held, int pad)
{
    if (!held.clip.getParent().isValid()) // (deleted meanwhile)
        return;
    const auto& map = session_.getTempoMap();
    const double rate = rateOf(held.clip);
    // As long as it was held: by the playhead, or by the clock where the playhead jumped back (a
    // loop) or stopped.
    const auto position = host_.getPlayheadSeconds ? host_.getPlayheadSeconds() : std::nullopt;
    double end = held.startSeconds + (nowMs() - held.startMs) / 1000.0 * session_.getSpeed();
    if (position.has_value() && *position >= held.startSeconds)
        end = *position;
    const double length = std::max(session::kMinNoteLength, (map.beatAt(end) - map.beatAt(held.startSeconds)) * rate);

    // One recording pass is one undo step (a new one if something else was done in between).
    auto& undo = session_.getUndoManager();
    if (newPass_ || undo.getUndoDescription() != kUndoName || !undo.canUndo())
    {
        session_.beginTransaction(kUndoName);
        newPass_ = false;
    }
    session_.addNote(held.clip, held.startBeat, length, kFirstNote + pad, static_cast<float>(held.velocity) / 127.0f, 1);
    ++notesRecorded_;
}

bool BeatMaker::keyPressed(const juce::KeyPress& key, juce::Component*)
{
    if (!host_.isEnabled || !host_.isEnabled())
        return false;
    const int pad = padForKey(key);
    if (pad < 0)
        return false;
    padDown(pad, key.getModifiers().isShiftDown(), key.getKeyCode()); // (ignored while held: key repeat)
    return true;
}

bool BeatMaker::keyStateChanged(bool isKeyDown, juce::Component*)
{
    if (!isKeyDown)
        timerCallback();
    return false;
}

void BeatMaker::timerCallback()
{
    bool polling = false;
    for (int pad = 0; pad < kPads; ++pad)
    {
        const auto& held = held_[static_cast<std::size_t>(pad)];
        if (!held.down || held.keyCode == 0)
            continue;
        if (host_.isKeyDown ? host_.isKeyDown(held.keyCode) : juce::KeyPress::isKeyCurrentlyDown(held.keyCode))
            polling = true;
        else
            padUp(pad);
    }
    if (!polling)
        stopTimer();
}

} // namespace dojo::ui
