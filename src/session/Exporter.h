#pragma once

#include "engine/Capture.h"
#include "engine/EngineController.h"
#include "engine/RealtimeHandoff.h"
#include "session/Project.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace dojo::session
{

struct ExportSettings
{
    enum class Format
    {
        wav,
        flac,
        mp3,
    };

    juce::File destination; ///< the mix; stems are written next to it as "<name> - <track>.<ext>"
    Format format = Format::wav;
    int bitDepth = 24;          ///< WAV: 16, 24 or 32 (float); FLAC: 16 or 24
    int mp3QualityIndex = 4;    ///< index into LameMp3Format::getQualityOptions() (256 kbps)
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    double tailSeconds = 2.0;   ///< rendered after the end so reverbs and delays ring out
    bool realtime = false;      ///< play through the audio device and capture, instead of offline
    bool includeMix = true;
    bool includeStems = false;  ///< one file per track (its own post-fader output)
};

/**
    Bounces the project to audio files.

    Offline: live processing is paused (the device outputs silence) and an export thread renders the
    graph as fast as the CPU allows, through the very same engine code as playback (devices are put
    into non-real-time mode). Real-time: the project plays through the audio device while the mix and
    stems are captured — for hardware inserts or plug-ins that must run in real time.

    Either way the output is aligned to the timeline (the graph's plug-in latency is skipped), and
    stems are each track's own post-fader output. Message thread API; onFinished arrives on the
    message thread.
*/
class Exporter final : private juce::Thread, private juce::Timer
{
public:
    Exporter(Project& project, engine::EngineController& controller);
    ~Exporter() override;

    /** Starts an export. Returns an error message, or an empty string if it started. */
    juce::String start(const ExportSettings& settings);
    void cancel();

    [[nodiscard]] bool isRunning() const noexcept { return running_; }
    [[nodiscard]] double getProgress() const noexcept { return progress_.load(std::memory_order_relaxed); }

    /** Called on the message thread when an export ends (ok, failed or cancelled). */
    std::function<void(const juce::Result&, const juce::Array<juce::File>& filesWritten)> onFinished;

    [[nodiscard]] static juce::String getFileExtension(ExportSettings::Format format);
    /** Creates a writer for `file` (replacing it). Null with `error` set on failure. */
    static std::unique_ptr<juce::AudioFormatWriter> createWriter(const juce::File& file, const ExportSettings& settings,
                                                                 double sampleRate, int numChannels, juce::String& error);
    /** The stem file for a track. */
    [[nodiscard]] static juce::File getStemFile(const ExportSettings& settings, const juce::String& trackName);

private:
    struct Target
    {
        int slot = -1; ///< -1 = the mix
        juce::File file;
        int channels = 2;                ///< channels written (a surround mix or stem has one per speaker)
        engine::ChannelLayout layout;    ///< the signal's channels (a stem: its track's destination's)
        bool foldDown = false;           ///< a surround stem into a stereo-only format (MP3)
        /** How late its signal is (delay compensation): this much of the start is skipped, so the
            file lines up with the timeline. The mix: the whole graph's latency; a stem: its own
            track's output (after its compensation delay). */
        int latency = 0;
        std::unique_ptr<juce::AudioFormatWriter> writer;          // offline
        std::shared_ptr<engine::CaptureStream> stream;             // real-time
    };

    void run() override;          // offline render thread
    void timerCallback() override; // real-time progress / completion
    juce::String prepareTargets(double sampleRate, bool forStreams);
    void finishOffline();
    void finishRealtime();
    void complete(const juce::Result& result);

    Project& project_;
    engine::EngineController& controller_;
    ExportSettings settings_;
    std::vector<Target> targets_;
    engine::DiskWriterThread diskWriter_;
    std::unique_ptr<engine::CaptureSession> capture_;

    bool running_ = false;
    bool preparedForExport_ = false;
    bool realtimeStopping_ = false;
    std::uint32_t stopCountBefore_ = 0;
    std::uint64_t graphSerial_ = 0;
    int latency_ = 0;       ///< at the master output
    int stemLatency_ = 0;   ///< at the master input (stems whose own latency is unknown)
    int longestLatency_ = 0; ///< of all targets: how much longer than the range is rendered
    double sampleRate_ = 48000.0;
    double speed_ = 1.0;
    double previousPosition_ = 0.0;
    std::atomic<double> progress_{0.0};
    std::atomic<bool> failed_{false};
    juce::String error_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0); ///< guards async completion after destruction
};

} // namespace dojo::session
