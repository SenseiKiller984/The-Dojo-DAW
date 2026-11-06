#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <optional>
#include <vector>

namespace dojo::devices
{

/**
    Base class of The Dojo's native devices.

    Devices are ordinary juce::AudioProcessors, so the engine runs them exactly like hosted plug-ins
    (through engine::ProcessorNode). This base class removes the boilerplate:

    - DSP runs in 64-bit (processDsp); a 32-bit host buffer is converted through a pre-allocated
      scratch buffer, so both precisions are real-time safe;
    - parameters are plain juce::AudioParameter* objects (lock-free atomics), added with addFloat /
      addBool / addChoice; their values and any extra state (sample paths, ...) are saved as XML;
    - main buses are stereo; extra input buses are sidechains, extra output buses aux outputs.

    Editors are provided by the UI layer (by device type), so hasEditor() is false here.
*/
class BuiltinProcessor : public juce::AudioProcessor
{
public:
    BuiltinProcessor(juce::String typeId, juce::String displayName, BusesProperties buses, bool isInstrument);
    ~BuiltinProcessor() override;

    [[nodiscard]] const juce::String& getTypeId() const noexcept { return typeId_; }

    // --- juce::AudioProcessor ----------------------------------------------------------------------
    const juce::String getName() const override { return name_; }
    void prepareToPlay(double sampleRate, int maximumBlockSize) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    void processBlock(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) override;
    bool supportsDoublePrecisionProcessing() const override { return true; }
    bool isBusesLayoutSupported(const BusesLayout& layout) const override;

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return instrument_; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destination) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    /** Parameter by its ID (null if unknown). */
    [[nodiscard]] juce::RangedAudioParameter* getParameterById(const juce::String& id) const;

protected:
    virtual void prepareDsp(double sampleRate, int maximumBlockSize) = 0;
    /** Processes one block in place. `buffer` holds every bus's channels (main first). */
    virtual void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept = 0;
    virtual void writeExtraState(juce::XmlElement&) const {}
    virtual void readExtraState(const juce::XmlElement&) {}

    juce::AudioParameterFloat* addFloat(const juce::String& id, const juce::String& name,
                                        juce::NormalisableRange<float> range, float defaultValue,
                                        const juce::String& label = {}, bool automatable = true);
    juce::AudioParameterBool* addBool(const juce::String& id, const juce::String& name, bool defaultValue);
    juce::AudioParameterChoice* addChoice(const juce::String& id, const juce::String& name,
                                          const juce::StringArray& choices, int defaultIndex);

    double sampleRate_ = 48000.0;
    int maxBlock_ = 512;

private:
    juce::String typeId_;
    juce::String name_;
    bool instrument_ = false;
    juce::AudioBuffer<double> scratch_;
    std::vector<juce::RangedAudioParameter*> ranged_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BuiltinProcessor)
};

/**
    The number in a typed parameter value: "120", "120 Hz", "-6 dB", "50 %", "2.5k" / "2.5 kHz"
    (thousands, as values are displayed), and "1.5 s" when the unit is "ms". Decimal comma accepted.
    Empty if the text has no number.
*/
[[nodiscard]] std::optional<double> parseParameterNumber(const juce::String& text, const juce::String& unit);
/** parseParameterNumber(), or 0 without a number (juce's value-from-string must return something). */
[[nodiscard]] double parseParameterText(const juce::String& text, const juce::String& unit);

/** Range with a logarithmic-feeling skew centred at `centre` (e.g. frequencies). */
juce::NormalisableRange<float> skewedRange(float minimum, float maximum, float centre, float interval = 0.0f);

} // namespace dojo::devices
