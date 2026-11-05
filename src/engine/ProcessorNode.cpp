#include "engine/ProcessorNode.h"

#include <algorithm>

namespace dojo::engine
{

ProcessorNode::ProcessorNode(std::unique_ptr<juce::AudioProcessor> processor)
    : processor_(std::move(processor))
{
    jassert(processor_ != nullptr);
    negotiateLayout();
}

ProcessorNode::~ProcessorNode()
{
    if (prepared_)
        processor_->releaseResources();
}

void ProcessorNode::negotiateLayout()
{
    // Ask for stereo main buses; keep whatever the processor insists on otherwise.
    auto layout = processor_->getBusesLayout();
    if (!layout.inputBuses.isEmpty() && layout.inputBuses.getReference(0) != juce::AudioChannelSet::stereo())
        layout.inputBuses.getReference(0) = juce::AudioChannelSet::stereo();
    if (!layout.outputBuses.isEmpty() && layout.outputBuses.getReference(0) != juce::AudioChannelSet::stereo())
        layout.outputBuses.getReference(0) = juce::AudioChannelSet::stereo();

    if (layout != processor_->getBusesLayout())
        processor_->setBusesLayout(layout); // may fail; then the processor's own layout is used

    const auto* mainIn = processor_->getBus(true, 0);
    const auto* mainOut = processor_->getBus(false, 0);
    const auto* sidechain = processor_->getBus(true, 1);

    mainInputs_ = mainIn != nullptr && mainIn->isEnabled() ? mainIn->getNumberOfChannels() : 0;
    mainOutputs_ = mainOut != nullptr && mainOut->isEnabled() ? mainOut->getNumberOfChannels() : 0;
    sidechainInputs_ = sidechain != nullptr && sidechain->isEnabled() ? sidechain->getNumberOfChannels() : 0;
    sidechainFirstChannel_ = sidechainInputs_ > 0 ? processor_->getChannelIndexInProcessBlockBuffer(true, 1, 0) : 0;
    auxOutputs_ = std::max(0, processor_->getTotalNumOutputChannels() - mainOutputs_);
    totalChannels_ = std::max({processor_->getTotalNumInputChannels(), processor_->getTotalNumOutputChannels(), 2});
}

void ProcessorNode::prepare(double sampleRate, int maximumBlockSize, juce::AudioPlayHead* playHead)
{
    if (prepared_)
        processor_->releaseResources();

    negotiateLayout();
    doublePrecision_ = processor_->supportsDoublePrecisionProcessing();
    processor_->setProcessingPrecision(doublePrecision_ ? juce::AudioProcessor::doublePrecision
                                                        : juce::AudioProcessor::singlePrecision);
    processor_->setPlayHead(playHead);
    processor_->setRateAndBufferSizeDetails(sampleRate, maximumBlockSize);
    processor_->prepareToPlay(sampleRate, maximumBlockSize);

    if (doublePrecision_)
    {
        doubleScratch_.setSize(totalChannels_, maximumBlockSize, false, true, false);
        floatScratch_.setSize(0, 0);
    }
    else
    {
        floatScratch_.setSize(totalChannels_, maximumBlockSize, false, true, false);
        doubleScratch_.setSize(0, 0);
    }

    aux_.setSize(std::max(1, auxOutputs_), maximumBlockSize, false, true, false);
    emptyMidi_.ensureSize(1024);
    prepared_ = true;
}

void ProcessorNode::setNonRealtime(bool nonRealtime)
{
    processor_->setNonRealtime(nonRealtime);
}

const Sample* ProcessorNode::getAuxOutput(int channel) const noexcept
{
    if (channel < 0 || channel >= auxOutputs_)
        return nullptr;
    return aux_.getReadPointer(channel);
}

void ProcessorNode::process(juce::AudioBuffer<Sample>& stereo, int numSamples, juce::MidiBuffer& midi,
                            const juce::AudioBuffer<Sample>* sidechain) noexcept
{
    if (!prepared_ || numSamples <= 0)
        return;

    if (isBypassed())
    {
        if (mainInputs_ == 0) // a bypassed instrument makes no sound
            stereo.clear(0, numSamples);
        for (int c = 0; c < auxOutputs_; ++c)
            aux_.clear(c, 0, numSamples);
        return;
    }

    if (doublePrecision_)
        runProcessor(doubleScratch_, stereo, numSamples, midi, sidechain);
    else
        runProcessor(floatScratch_, stereo, numSamples, midi, sidechain);
}

template <typename FloatType>
void ProcessorNode::runProcessor(juce::AudioBuffer<FloatType>& scratch, juce::AudioBuffer<Sample>& stereo,
                                 int numSamples, juce::MidiBuffer& midi,
                                 const juce::AudioBuffer<Sample>* sidechain) noexcept
{
    // Shrinks the visible size only: no reallocation (avoidReallocating).
    scratch.setSize(totalChannels_, numSamples, true, false, true);
    scratch.clear();

    const Sample* left = stereo.getReadPointer(0);
    const Sample* right = stereo.getReadPointer(1);

    if (mainInputs_ >= 2)
    {
        auto* l = scratch.getWritePointer(0);
        auto* r = scratch.getWritePointer(1);
        for (int i = 0; i < numSamples; ++i)
        {
            l[i] = static_cast<FloatType>(left[i]);
            r[i] = static_cast<FloatType>(right[i]);
        }
    }
    else if (mainInputs_ == 1)
    {
        auto* m = scratch.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i)
            m[i] = static_cast<FloatType>(0.5 * (left[i] + right[i]));
    }

    if (sidechainInputs_ > 0 && sidechain != nullptr)
    {
        for (int c = 0; c < sidechainInputs_ && sidechainFirstChannel_ + c < totalChannels_; ++c)
        {
            const Sample* src = sidechain->getReadPointer(std::min(c, sidechain->getNumChannels() - 1));
            auto* dst = scratch.getWritePointer(sidechainFirstChannel_ + c);
            for (int i = 0; i < numSamples; ++i)
                dst[i] = static_cast<FloatType>(src[i]);
        }
    }

    juce::MidiBuffer& midiForProcessor = processor_->acceptsMidi() ? midi : emptyMidi_;
    emptyMidi_.clear();

    processor_->processBlock(scratch, midiForProcessor);

    Sample* outLeft = stereo.getWritePointer(0);
    Sample* outRight = stereo.getWritePointer(1);

    if (mainOutputs_ >= 2)
    {
        const auto* l = scratch.getReadPointer(0);
        const auto* r = scratch.getReadPointer(1);
        for (int i = 0; i < numSamples; ++i)
        {
            outLeft[i] = static_cast<Sample>(l[i]);
            outRight[i] = static_cast<Sample>(r[i]);
        }
    }
    else if (mainOutputs_ == 1)
    {
        const auto* m = scratch.getReadPointer(0);
        for (int i = 0; i < numSamples; ++i)
            outLeft[i] = outRight[i] = static_cast<Sample>(m[i]);
    }

    for (int c = 0; c < auxOutputs_; ++c)
    {
        const auto* src = scratch.getReadPointer(mainOutputs_ + c);
        auto* dst = aux_.getWritePointer(c);
        for (int i = 0; i < numSamples; ++i)
            dst[i] = static_cast<Sample>(src[i]);
    }
}

template void ProcessorNode::runProcessor<float>(juce::AudioBuffer<float>&, juce::AudioBuffer<Sample>&, int,
                                                 juce::MidiBuffer&, const juce::AudioBuffer<Sample>*) noexcept;
template void ProcessorNode::runProcessor<double>(juce::AudioBuffer<double>&, juce::AudioBuffer<Sample>&, int,
                                                  juce::MidiBuffer&, const juce::AudioBuffer<Sample>*) noexcept;

} // namespace dojo::engine
