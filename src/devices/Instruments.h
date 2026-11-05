#pragma once

#include "core/rt/SpscQueue.h"
#include "devices/BuiltinProcessor.h"
#include "devices/SampleSlot.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace dojo::devices
{

/**
    Polyphonic sampler: one sample played chromatically from a root note, with an ADSR amplitude
    envelope, tuning, velocity sensitivity, start/end points and optional looping. Responds to note
    on/off, sustain pedal (CC 64), pitch bend (±2 semitones) and all-notes-off.
*/
class Sampler final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:sampler";
    static constexpr int kVoices = 32;

    Sampler();

    /** Message thread: decodes `file` and makes it the sample. */
    bool loadSample(const juce::File& file, juce::String& error);
    void setSample(std::shared_ptr<const SampleData> sample);
    [[nodiscard]] std::shared_ptr<const SampleData> getSample() const { return sample_.get(); }

    /** Number of voices sounding after the last block (UI). */
    [[nodiscard]] int getActiveVoiceCount() const noexcept { return activeVoices_.load(std::memory_order_relaxed); }

    double getTailLengthSeconds() const override { return release_->get() / 1000.0; }

    // Parameters (for the editor).
    juce::AudioParameterFloat* root_ = nullptr;
    juce::AudioParameterFloat* tune_ = nullptr;
    juce::AudioParameterFloat* fine_ = nullptr;
    juce::AudioParameterFloat* attack_ = nullptr;
    juce::AudioParameterFloat* decay_ = nullptr;
    juce::AudioParameterFloat* sustain_ = nullptr;
    juce::AudioParameterFloat* release_ = nullptr;
    juce::AudioParameterFloat* gain_ = nullptr;
    juce::AudioParameterFloat* velocity_ = nullptr;
    juce::AudioParameterFloat* start_ = nullptr;
    juce::AudioParameterFloat* end_ = nullptr;
    juce::AudioParameterBool* loop_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;
    void writeExtraState(juce::XmlElement& xml) const override;
    void readExtraState(const juce::XmlElement& xml) override;

private:
    struct Voice
    {
        bool active = false;
        int note = 0;
        double position = 0.0;
        double velocityGain = 1.0;
        juce::ADSR envelope;
        std::uint64_t age = 0;
        bool keyDown = false;
        bool sustained = false;
    };

    void handleMidi(const std::uint8_t* data, int size) noexcept;
    void noteOn(int note, float velocity) noexcept;
    void noteOff(int note) noexcept;
    void render(double* left, double* right, int start, int end, const SampleData& sample) noexcept;

    SampleSlot sample_;
    const SampleData* lastSample_ = nullptr;
    std::array<Voice, kVoices> voices_;
    std::uint64_t ageCounter_ = 0;
    bool sustainPedal_ = false;
    double pitchBend_ = 0.0;
    std::atomic<int> activeVoices_{0};
};

/** A pad hit queued by the UI (audition). Namespace scope: a nested type with default member
    initialisers is not default-constructible until its enclosing class is complete. */
struct DrumPadTrigger
{
    int pad = 0;
    float velocity = 1.0f;
};

/**
    Drum Rack: 16 sample pads on MIDI notes 36 (C1) to 51, each with volume, pan, pitch, a choke
    group and an output: the main stereo output or one of 16 stereo aux outputs ("multi-out"), which
    The Dojo returns on their own mixer tracks. Pads can be auditioned from the UI.
*/
class DrumRack final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:drumrack";
    static constexpr int kPads = 16;
    static constexpr int kVoicesPerPad = 4;
    static constexpr int kFirstNote = 36;

    DrumRack();

    struct PadParameters
    {
        juce::AudioParameterFloat* volume = nullptr;
        juce::AudioParameterFloat* pan = nullptr;
        juce::AudioParameterFloat* pitch = nullptr;
        juce::AudioParameterChoice* output = nullptr; ///< 0 = main, k = aux output k
        juce::AudioParameterChoice* choke = nullptr;  ///< 0 = none, 1..4 = group
    };

    [[nodiscard]] const PadParameters& getPad(int pad) const { return pads_[static_cast<std::size_t>(pad)]; }

    /** Message thread: decodes `file` onto `pad`. */
    bool loadPadSample(int pad, const juce::File& file, juce::String& error);
    void setPadSample(int pad, std::shared_ptr<const SampleData> sample);
    [[nodiscard]] std::shared_ptr<const SampleData> getPadSample(int pad) const;

    /** Message thread: plays a pad now (audition). */
    void triggerPad(int pad, float velocity);
    /** Message thread: peak level of a pad since the last call (for the pad's light). */
    [[nodiscard]] float takePadActivity(int pad) noexcept;

    [[nodiscard]] static juce::String getPadNoteName(int pad);

    double getTailLengthSeconds() const override { return 0.0; }

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;
    void writeExtraState(juce::XmlElement& xml) const override;
    void readExtraState(const juce::XmlElement& xml) override;

private:
    struct Voice
    {
        bool active = false;
        int pad = 0;
        double position = 0.0;
        double velocity = 1.0;
        double fade = 1.0;
        double fadeStep = 0.0; ///< > 0 while choked
        std::uint64_t age = 0;
    };

    static BusesProperties makeBuses();
    void trigger(int pad, float velocity) noexcept;
    void render(juce::AudioBuffer<double>& buffer, int start, int end) noexcept;

    std::array<PadParameters, kPads> pads_;
    juce::AudioParameterFloat* master_ = nullptr;
    std::array<SampleSlot, kPads> samples_;
    std::array<const SampleData*, kPads> blockSamples_{};
    std::array<Voice, kPads * kVoicesPerPad> voices_;
    std::uint64_t ageCounter_ = 0;
    rt::SpscQueue<DrumPadTrigger> auditions_{64};
    std::array<std::atomic<float>, kPads> activity_{};
};

} // namespace dojo::devices
