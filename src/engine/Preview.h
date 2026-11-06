#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace dojo::engine
{

class AudioEngine;

/**
    The audio of a previewed file at the engine's sample rate, stereo.

    The buffer is allocated once, up front, for the whole (possibly truncated) length; the decoder
    then fills it from the start and publishes how far it got through `framesReady`, so playback can
    start while the rest is still being decoded. The audio thread only ever reads frames below
    `framesReady`, the decoder only writes frames at or above it.
*/
struct PreviewSound
{
    juce::File file;
    juce::AudioBuffer<float> audio; ///< 2 channels x totalFrames
    int totalFrames = 0;
    double sampleRate = 0.0;
    bool truncated = false;              ///< longer than PreviewService::kMaxSeconds
    std::atomic<int> framesReady{0};     ///< decoder -> audio thread (release / acquire)
    std::atomic<bool> cancelled{false};  ///< stop decoding (a newer preview replaced this one)
    std::atomic<bool> failed{false};
};

/**
    The audio-thread side of previewing: plays one PreviewSound on top of the device output.

    Previews bypass the mix entirely (they are added to the device output after the master), so they
    are never metered, recorded or exported. Real-time safe hand-over, like devices::SampleSlot:
    the audio thread acknowledges the sound it uses each block, and a replaced sound is released only
    after the audio thread has acknowledged a newer one.
*/
class PreviewPlayer final
{
public:
    PreviewPlayer() = default;
    PreviewPlayer(const PreviewPlayer&) = delete;
    PreviewPlayer& operator=(const PreviewPlayer&) = delete;

    // --- Any non-real-time thread ----------------------------------------------------------------
    /** Plays `sound` from its start (replacing what is playing). */
    void play(std::shared_ptr<PreviewSound> sound);
    /** Fades out what is playing. */
    void stop();
    void setGain(float linear) noexcept { gain_.store(linear, std::memory_order_relaxed); }
    [[nodiscard]] float getGain() const noexcept { return gain_.load(std::memory_order_relaxed); }
    /** True while the audio thread is playing a preview. */
    [[nodiscard]] bool isPlaying() const noexcept { return active_.load(std::memory_order_acquire); }
    /** 0..1 through the current sound. */
    [[nodiscard]] float getProgress() const noexcept { return progress_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::shared_ptr<PreviewSound> getCurrent() const;
    /** Frees replaced sounds the audio thread no longer uses (called by play(); call it periodically). */
    void collectGarbage();

    // --- Audio thread ----------------------------------------------------------------------------
    /** Adds the preview to the device outputs (one output: mono sum). */
    void addTo(float* const* outputs, int numOutputs, int numSamples) noexcept;

private:
    static constexpr float kFadeStep = 1.0f / 256.0f; // ~5 ms fades at 48 kHz

    std::atomic<PreviewSound*> pending_{nullptr};
    std::atomic<PreviewSound*> acknowledged_{nullptr};
    std::atomic<std::uint32_t> request_{0}; ///< (serial << 1) | isStop
    std::atomic<float> gain_{0.7f};
    std::atomic<bool> active_{false};
    std::atomic<float> progress_{0.0f};

    // Audio thread only.
    std::uint32_t seenRequest_ = 0;
    int position_ = 0;
    bool running_ = false;
    bool stopping_ = false;
    float fade_ = 0.0f;

    // Non-real-time side (guarded; the audio thread never takes this lock).
    mutable std::mutex mutex_;
    std::uint32_t serial_ = 0;
    std::shared_ptr<PreviewSound> current_;
    std::vector<std::shared_ptr<PreviewSound>> retired_;
};

/**
    Previews audio files (the browser's preview): decodes and resamples on a background thread and
    plays through the engine's PreviewPlayer as soon as the first part is decoded.

    Message thread API. Files longer than kMaxSeconds are previewed up to that point.
*/
class PreviewService final : private juce::Thread
{
public:
    static constexpr double kMaxSeconds = 120.0;

    PreviewService(AudioEngine& engine, juce::AudioFormatManager& formats);
    ~PreviewService() override;

    /** Starts previewing `file`. False if no audio device is running or the file is not audio. */
    bool preview(const juce::File& file);
    void stop();
    [[nodiscard]] bool isPlaying() const noexcept;
    /** The file being previewed (or last previewed). */
    [[nodiscard]] juce::File getCurrentFile() const;
    [[nodiscard]] float getProgress() const noexcept;
    void setVolume(float linearGain) noexcept;
    [[nodiscard]] float getVolume() const noexcept;

    /** Frees sounds the audio thread has finished with (call periodically, e.g. from a UI timer). */
    void collectGarbage();

    /** Test helper: true once the current sound is fully decoded (or failed). */
    [[nodiscard]] bool isDecodeFinished() const;

private:
    void run() override;
    void decode(PreviewSound& sound, juce::AudioFormatReader& reader, double targetRate);

    AudioEngine& engine_;
    juce::AudioFormatManager& formats_;
    mutable std::mutex jobMutex_;
    juce::File jobFile_;
    double jobRate_ = 0.0;
    bool jobPending_ = false;
    std::uint64_t generation_ = 0; ///< bumped by preview() and stop()
    std::uint64_t jobGeneration_ = 0;
    bool decodeDone_ = true; ///< the latest job was decoded completely (or could not be)
    std::shared_ptr<PreviewSound> decoding_;
    juce::File currentFile_;
};

} // namespace dojo::engine
