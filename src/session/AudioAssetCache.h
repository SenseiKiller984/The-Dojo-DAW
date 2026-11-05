#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <array>
#include <functional>
#include <map>
#include <memory>
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

/** A decoded audio file. Immutable once published, shared by clips, the UI and render graphs. */
struct AudioAsset
{
    juce::File file;
    double sampleRate = 0.0;
    juce::AudioBuffer<float> audio; ///< at most 2 channels
    WaveformPeaks peaks;

    [[nodiscard]] double getLengthSeconds() const noexcept
    {
        return sampleRate > 0.0 ? static_cast<double>(audio.getNumSamples()) / sampleRate : 0.0;
    }
};

using AudioAssetPtr = std::shared_ptr<const AudioAsset>;

/**
    Decodes audio files (WAV, AIFF, FLAC, Ogg Vorbis, MP3, Opus) on background threads and keeps one
    shared, immutable copy of each in memory.

    All public functions are message-thread only; completion callbacks also arrive on the message
    thread. (Clips are fully memory-resident for now; disk streaming for very long files is a later
    optimisation that will not change this interface.)
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

private:
    struct DecodeResult
    {
        std::shared_ptr<AudioAsset> asset;
        juce::String error;
    };

    static DecodeResult decode(juce::AudioFormatManager& formats, const juce::File& file);
    void finish(const juce::File& file, const DecodeResult& result);

    juce::AudioFormatManager formats_;
    std::map<juce::String, AudioAssetPtr> assets_;
    std::map<juce::String, std::vector<Completion>> pending_;
    std::map<juce::String, juce::String> failures_; ///< last decode error per file, to avoid retry storms
    juce::ListenerList<Listener> listeners_;
    juce::ThreadPool pool_{2}; // declared last: destroyed (and joined) before everything it uses

    JUCE_DECLARE_WEAK_REFERENCEABLE(AudioAssetCache)
};

} // namespace dojo::session
