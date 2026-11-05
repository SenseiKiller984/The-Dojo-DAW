#pragma once

#include "engine/dsp/ClipStretcher.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace dojo::session
{

/** Min/max overview of an audio file, for drawing waveforms at any zoom level cheaply. */
struct WaveformPeaks
{
    static constexpr int kFramesPerBucket = 256;

    int numChannels = 0;
    std::array<std::vector<float>, 2> minimum;
    std::array<std::vector<float>, 2> maximum;

    [[nodiscard]] int getNumBuckets() const noexcept { return static_cast<int>(minimum[0].size()); }
};

/**
    An audio file ready for playback: either decoded into memory, or (for long files) streamed from
    disk. Immutable once published, shared by clips, the UI and render graphs.
*/
struct AudioAsset
{
    juce::File file;
    double sampleRate = 0.0;
    int numChannels = 0;        ///< 1 or 2
    juce::int64 numFrames = 0;
    juce::AudioBuffer<float> audio;                  ///< decoded audio (empty when streamed)
    std::shared_ptr<engine::StreamedAudio> streamed; ///< set for disk-streamed files
    WaveformPeaks peaks;

    [[nodiscard]] bool isStreamed() const noexcept { return streamed != nullptr; }
    [[nodiscard]] double getLengthSeconds() const noexcept
    {
        return sampleRate > 0.0 ? static_cast<double>(numFrames) / sampleRate : 0.0;
    }
};

using AudioAssetPtr = std::shared_ptr<const AudioAsset>;

/**
    Loads audio files (WAV, AIFF, FLAC, Ogg Vorbis, MP3, Opus) on background threads and keeps one
    shared, immutable copy of each.

    Files up to the streaming threshold (default: 10 minutes) are decoded into memory. Longer ones are
    streamed from disk: only the regions around what is being played are resident, kept loaded by a
    background service thread (see engine::StreamedAudio).

    All public functions are message-thread only; completion callbacks also arrive on the message
    thread.
*/
class AudioAssetCache final
{
public:
    using Completion = std::function<void(AudioAssetPtr asset, const juce::String& error)>;

    struct Listener
    {
        virtual ~Listener() = default;
        virtual void audioAssetLoaded(const juce::File& file) = 0;
    };

    AudioAssetCache();
    ~AudioAssetCache();

    AudioAssetCache(const AudioAssetCache&) = delete;
    AudioAssetCache& operator=(const AudioAssetCache&) = delete;

    /** The decoded asset for `file`, or null if it is not loaded (yet). */
    [[nodiscard]] AudioAssetPtr find(const juce::File& file) const;

    [[nodiscard]] bool isLoading(const juce::File& file) const;

    /** Loads `file` in the background (or completes at once if already loaded). `onDone` may be empty. */
    void request(const juce::File& file, Completion onDone);

    /** Decodes `file` on the calling thread and caches it. Returns null and fills `error` on failure. */
    AudioAssetPtr loadNow(const juce::File& file, juce::String& error);

    void addListener(Listener* listener) { listeners_.add(listener); }
    void removeListener(Listener* listener) { listeners_.remove(listener); }

    /** File-chooser / browser pattern for every readable format, e.g. "*.wav;*.flac;...". */
    [[nodiscard]] juce::String getWildcard() const { return formats_.getWildcardForAllFormats(); }

    [[nodiscard]] bool canRead(const juce::File& file) const;

    /** Files longer than this are streamed from disk instead of decoded into memory. */
    void setStreamingThreshold(double seconds) noexcept { streamingThreshold_ = seconds; }
    [[nodiscard]] double getStreamingThreshold() const noexcept { return streamingThreshold_; }

    /** Source of the engine's process epoch (see AudioEngine::getProcessEpoch), used to free evicted
        stream chunks only once no audio block can still be reading them. */
    void setEpochSource(std::function<std::uint64_t()> source);

    [[nodiscard]] juce::AudioFormatManager& getFormatManager() noexcept { return formats_; }

private:
    class StreamService;
    struct DecodeResult
    {
        std::shared_ptr<AudioAsset> asset;
        juce::String error;
    };

    static DecodeResult decode(juce::AudioFormatManager& formats, const juce::File& file, double streamingThreshold);
    void finish(const juce::File& file, const DecodeResult& result);

    juce::AudioFormatManager formats_;
    std::map<juce::String, AudioAssetPtr> assets_;
    std::map<juce::String, std::vector<Completion>> pending_;
    std::map<juce::String, juce::String> failures_; ///< last decode error per file, to avoid retry storms
    juce::ListenerList<Listener> listeners_;
    double streamingThreshold_ = 10.0 * 60.0;
    std::unique_ptr<StreamService> streamService_;
    juce::ThreadPool pool_{2}; // declared last: destroyed (and joined) before everything it uses

    JUCE_DECLARE_WEAK_REFERENCEABLE(AudioAssetCache)
};

} // namespace dojo::session
