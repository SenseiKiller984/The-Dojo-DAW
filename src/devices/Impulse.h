#pragma once

#include "core/rt/SpscQueue.h"
#include "devices/BuiltinProcessor.h"
#include "devices/Instruments.h"
#include "devices/SampleSlot.h"
#include "devices/dsp/SynthCore.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace dojo::devices
{

/**
    Impulse: an 8-slot drum sampler for quick kits. Slots play on MIDI notes 36 (C1) .. 43 (G1).

    Each slot has its own sample (drag one onto the slot, or load it) and:
    - Start (where in the sample playback begins) and Transpose (-48 .. +48 semitones);
    - Drive (saturation) and Transient (sharpen the attack, or soften it);
    - Decay (fade-out time; at its maximum the sample plays to its end);
    - a filter (low-pass, high-pass or band-pass with resonance);
    - Pan, Volume and Velocity sensitivity.
    Slots 7 and 8 can be linked so they choke each other (open and closed hi-hat). Global Volume
    and Transpose apply to every slot.
*/
class Impulse final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:impulse";
    static constexpr int kSlots = 8;
    static constexpr int kVoicesPerSlot = 4;
    static constexpr int kFirstNote = 36;
    static constexpr float kMaxDecayMs = 10'000.0f; ///< at this value the sample plays to its end

    Impulse();

    struct SlotParameters
    {
        juce::AudioParameterFloat* start = nullptr;
        juce::AudioParameterFloat* transpose = nullptr;
        juce::AudioParameterFloat* drive = nullptr;
        juce::AudioParameterFloat* transient = nullptr;
        juce::AudioParameterFloat* decay = nullptr;
        juce::AudioParameterChoice* filter = nullptr; ///< Off, Low Pass, High Pass, Band Pass
        juce::AudioParameterFloat* cutoff = nullptr;
        juce::AudioParameterFloat* resonance = nullptr;
        juce::AudioParameterFloat* pan = nullptr;
        juce::AudioParameterFloat* volume = nullptr;
        juce::AudioParameterFloat* velocity = nullptr;
    };

    [[nodiscard]] const SlotParameters& getSlot(int slot) const { return slots_[static_cast<std::size_t>(slot)]; }

    /** Message thread: decodes `file` onto `slot`. */
    bool loadSlotSample(int slot, const juce::File& file, juce::String& error);
    void setSlotSample(int slot, std::shared_ptr<const SampleData> sample);
    [[nodiscard]] std::shared_ptr<const SampleData> getSlotSample(int slot) const;

    /** Message thread: plays a slot now (audition). */
    void triggerSlot(int slot, float velocity);
    /** Message thread: peak level of a slot since the last call. */
    [[nodiscard]] float takeSlotActivity(int slot) noexcept;
    [[nodiscard]] static juce::String getSlotNoteName(int slot);

    juce::AudioParameterBool* link_ = nullptr;
    juce::AudioParameterFloat* volume_ = nullptr;
    juce::AudioParameterFloat* transpose_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;
    void writeExtraState(juce::XmlElement& xml) const override;
    void readExtraState(const juce::XmlElement& xml) override;

private:
    struct Voice
    {
        bool active = false;
        int slot = 0;
        double position = 0.0;
        double velocity = 1.0;
        double elapsed = 0.0;   ///< samples since the hit
        double decay = 1.0;     ///< decay envelope
        double fade = 1.0;
        double fadeStep = 0.0;  ///< > 0 while choked
        std::uint64_t age = 0;
        std::array<dsp::StateVariableFilter, 2> filters;
    };

    void trigger(int slot, float velocity) noexcept;
    void render(juce::AudioBuffer<double>& buffer, int start, int end) noexcept;

    std::array<SlotParameters, kSlots> slots_;
    std::array<SampleSlot, kSlots> samples_;
    std::array<const SampleData*, kSlots> blockSamples_{};
    std::array<Voice, kSlots * kVoicesPerSlot> voices_;
    std::uint64_t ageCounter_ = 0;
    rt::SpscQueue<DrumPadTrigger> auditions_{64};
    std::array<std::atomic<float>, kSlots> activity_{};
};

} // namespace dojo::devices
