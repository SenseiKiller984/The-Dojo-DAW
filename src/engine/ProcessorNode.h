#pragma once

#include "core/Types.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>

namespace dojo::engine
{

/**
    Runs one juce::AudioProcessor — a built-in device or a hosted VST3/CLAP plug-in — inside the
    engine's 64-bit signal path.

    - The track signal is stereo and 64-bit. Processors that support double precision process it
      directly; others go through a pre-allocated float scratch buffer.
    - A processor's main input/output buses are negotiated to stereo where possible (mono is mixed
      down / duplicated). An optional second input bus is the sidechain. Output buses beyond the main
      one are "aux outputs" (e.g. a Drum Rack's per-pad outputs) that other tracks can pull from.
    - Bypass is an atomic flag (pass-through; instruments go silent).

    Lifetime: created and prepared on the message thread; referenced by RenderGraphs and kept alive by
    their keepAlive lists, so it is destroyed on the message thread after the audio thread has
    stopped using it. process() is real-time safe and is only ever called from one thread at a time.
*/
class ProcessorNode final
{
public:
    explicit ProcessorNode(std::unique_ptr<juce::AudioProcessor> processor);
    ~ProcessorNode();

    ProcessorNode(const ProcessorNode&) = delete;
    ProcessorNode& operator=(const ProcessorNode&) = delete;

    /** Prepares the processor and allocates scratch buffers. Never concurrently with process(). */
    void prepare(double sampleRate, int maximumBlockSize, juce::AudioPlayHead* playHead);

    /** Switches offline (faster-than-real-time) rendering mode. Never concurrently with process(). */
    void setNonRealtime(bool nonRealtime);

    /**
        Processes `numSamples` of `stereo` in place. `midi` is passed to processors that accept MIDI
        (and is cleared for those that don't). `sidechain` (stereo, may be null) feeds the sidechain
        bus if there is one.
    */
    void process(juce::AudioBuffer<Sample>& stereo, int numSamples, juce::MidiBuffer& midi,
                 const juce::AudioBuffer<Sample>* sidechain) noexcept;

    [[nodiscard]] juce::AudioProcessor& getProcessor() noexcept { return *processor_; }
    [[nodiscard]] const juce::AudioProcessor& getProcessor() const noexcept { return *processor_; }

    void setBypassed(bool bypassed) noexcept { bypassed_.store(bypassed, std::memory_order_relaxed); }
    [[nodiscard]] bool isBypassed() const noexcept { return bypassed_.load(std::memory_order_relaxed); }

    [[nodiscard]] int getLatencySamples() const noexcept { return processor_->getLatencySamples(); }
    [[nodiscard]] bool isInstrument() const noexcept { return mainInputs_ == 0 && processor_->acceptsMidi(); }
    [[nodiscard]] bool hasSidechainInput() const noexcept { return sidechainInputs_ > 0; }
    [[nodiscard]] int getNumAuxOutputChannels() const noexcept { return auxOutputs_; }

    /** Aux output channel `channel` after the last process() call (silence if out of range). */
    [[nodiscard]] const Sample* getAuxOutput(int channel) const noexcept;

    [[nodiscard]] bool isPrepared() const noexcept { return prepared_; }

private:
    void negotiateLayout();

    template <typename FloatType>
    void runProcessor(juce::AudioBuffer<FloatType>& scratch, juce::AudioBuffer<Sample>& stereo, int numSamples,
                      juce::MidiBuffer& midi, const juce::AudioBuffer<Sample>* sidechain) noexcept;

    std::unique_ptr<juce::AudioProcessor> processor_;
    bool doublePrecision_ = false;
    bool prepared_ = false;

    int mainInputs_ = 0;
    int mainOutputs_ = 0;
    int sidechainInputs_ = 0;
    int sidechainFirstChannel_ = 0;
    int auxOutputs_ = 0;
    int totalChannels_ = 0;

    juce::AudioBuffer<double> doubleScratch_;
    juce::AudioBuffer<float> floatScratch_;
    juce::AudioBuffer<Sample> aux_;
    juce::MidiBuffer emptyMidi_;

    std::atomic<bool> bypassed_{false};
};

} // namespace dojo::engine
