#pragma once

#include "engine/Capture.h"
#include "engine/EngineController.h"
#include "engine/RealtimeHandoff.h"
#include "session/Project.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <memory>
#include <vector>

namespace dojo::session
{

/**
    Records audio and MIDI onto the armed tracks.

    start() opens one WAV file per armed audio track (its input channel or stereo pair) and captures
    live MIDI for armed instrument tracks, then starts the transport. The audio thread writes into
    lock-free capture streams that a disk thread drains, so recording never blocks the audio thread.

    stop() stops the transport and the capture; once the audio thread has acknowledged, the files are
    closed and become clips (audio) or MIDI clips — one undo step. Clips are placed with latency
    compensation: everything the performer heard was late by the output latency plus the graph's
    plug-in latency, and audio input arrives a further input latency later. The loop range is
    ignored while recording, except as the punch range: with punch-in / punch-out on, takes keep
    only what falls between the loop start / end. A take replaces what was on its track where it
    lies (one undo step with the recording). Message thread only.
*/
class Recorder final : private juce::Timer
{
public:
    Recorder(Project& project, engine::EngineController& controller);
    ~Recorder() override;

    /** Starts recording on every armed track. Returns an error message, or an empty string. */
    juce::String start();
    /** Stops recording; clips appear once the audio thread has let go of the capture. */
    void stop();

    [[nodiscard]] bool isRecording() const noexcept { return state_ == State::recording; }
    [[nodiscard]] bool isBusy() const noexcept { return state_ != State::idle; }

    /** Called when recording starts, stops or its clips have been created. */
    std::function<void()> onStateChanged;

    /** Where recordings of the current project go (created on demand). */
    [[nodiscard]] juce::File getRecordingsFolder() const;

    /** Latency (seconds of timeline) applied to recorded audio and MIDI. */
    [[nodiscard]] double getAudioLatencySeconds() const;
    [[nodiscard]] double getMidiLatencySeconds() const;

private:
    enum class State
    {
        idle,
        recording,
        finishing,
    };

    struct AudioTake
    {
        std::uint32_t trackId = 0;
        juce::File file;
        std::shared_ptr<engine::CaptureStream> stream;
    };

    void timerCallback() override;
    void finish();
    void createMidiClips(double captureStart, double stopPosition);

    Project& project_;
    engine::EngineController& controller_;
    engine::DiskWriterThread diskWriter_;

    State state_ = State::idle;
    std::unique_ptr<engine::CaptureSession> capture_;
    std::vector<std::unique_ptr<engine::CaptureSession>> abandoned_; ///< never acknowledged; kept alive
    std::vector<AudioTake> takes_;
    std::vector<std::uint32_t> midiTracks_;
    std::uint32_t stopCountBefore_ = 0;
    juce::uint32 stopRequestedAt_ = 0;
    double stopPosition_ = 0.0;
    double punchStart_ = 0.0; ///< the part of the timeline takes keep (punch in / out), from start()
    double punchEnd_ = 0.0;
    double audioLatency_ = 0.0;
    double midiLatency_ = 0.0;
};

} // namespace dojo::session
