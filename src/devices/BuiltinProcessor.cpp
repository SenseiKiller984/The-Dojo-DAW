#include "devices/BuiltinProcessor.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices
{

std::optional<double> parseParameterNumber(const juce::String& text, const juce::String& unit)
{
    const auto t = text.trim().toLowerCase();
    int end = 0;
    if (end < t.length() && (t[end] == '-' || t[end] == '+'))
        ++end;
    bool digits = false;
    while (end < t.length() && (juce::CharacterFunctions::isDigit(t[end]) || t[end] == '.' || t[end] == ','))
    {
        digits = digits || juce::CharacterFunctions::isDigit(t[end]);
        ++end;
    }
    if (!digits)
        return std::nullopt;

    double value = t.substring(0, end).replaceCharacter(',', '.').getDoubleValue();
    const auto suffix = t.substring(end).trim();
    if (suffix.startsWith("k"))
        value *= 1000.0; // "2.5k", "2.5 kHz"
    else if (unit.trim().toLowerCase() == "ms" && suffix.startsWith("s"))
        value *= 1000.0; // "1.5 s" for a time in milliseconds
    return value;
}

double parseParameterText(const juce::String& text, const juce::String& unit)
{
    return parseParameterNumber(text, unit).value_or(0.0);
}

juce::NormalisableRange<float> skewedRange(float minimum, float maximum, float centre, float interval)
{
    juce::NormalisableRange<float> range(minimum, maximum, interval);
    range.setSkewForCentre(centre);
    return range;
}

BuiltinProcessor::BuiltinProcessor(juce::String typeId, juce::String displayName, BusesProperties buses,
                                   bool isInstrument)
    : juce::AudioProcessor(std::move(buses))
    , typeId_(std::move(typeId))
    , name_(std::move(displayName))
    , instrument_(isInstrument)
{
}

BuiltinProcessor::~BuiltinProcessor() = default;

juce::AudioParameterFloat* BuiltinProcessor::addFloat(const juce::String& id, const juce::String& name,
                                                      juce::NormalisableRange<float> range, float defaultValue,
                                                      const juce::String& label, bool automatable, int decimals)
{
    // Readable values: 3 significant digits, "k" above 1000 (e.g. 2.50k Hz), or fixed decimals.
    const auto format = [decimals](float value, int maximumLength)
    {
        const float magnitude = std::abs(value);
        juce::String text;
        if (decimals >= 0)
            text = juce::String(value, decimals);
        else if (magnitude >= 1000.0f)
            text = juce::String(value / 1000.0f, magnitude >= 10000.0f ? 1 : 2) + "k";
        else
            text = juce::String(value, magnitude >= 100.0f ? 0 : magnitude >= 10.0f ? 1 : 2);
        return maximumLength > 0 ? text.substring(0, maximumLength) : text;
    };
    // Typed values (the editors let you type a number): what the display shows parses back, with
    // or without the unit ("2.50k", "2500 Hz", "-6 dB", "50 %"), and "1.5 s" works for ms values.
    const auto parse = [label](const juce::String& text) { return static_cast<float>(parseParameterText(text, label)); };
    auto parameter = std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{id, 1}, name, range, defaultValue,
        juce::AudioParameterFloatAttributes()
            .withLabel(label)
            .withAutomatable(automatable)
            .withStringFromValueFunction(format)
            .withValueFromStringFunction(parse));
    auto* raw = parameter.get();
    addParameter(parameter.release());
    ranged_.push_back(raw);
    return raw;
}

juce::AudioParameterBool* BuiltinProcessor::addBool(const juce::String& id, const juce::String& name, bool defaultValue)
{
    auto parameter = std::make_unique<juce::AudioParameterBool>(juce::ParameterID{id, 1}, name, defaultValue);
    auto* raw = parameter.get();
    addParameter(parameter.release());
    ranged_.push_back(raw);
    return raw;
}

juce::AudioParameterChoice* BuiltinProcessor::addChoice(const juce::String& id, const juce::String& name,
                                                        const juce::StringArray& choices, int defaultIndex, bool automatable)
{
    auto parameter = std::make_unique<juce::AudioParameterChoice>(juce::ParameterID{id, 1}, name, choices, defaultIndex,
                                                                  juce::AudioParameterChoiceAttributes().withAutomatable(automatable));
    auto* raw = parameter.get();
    addParameter(parameter.release());
    ranged_.push_back(raw);
    return raw;
}

juce::RangedAudioParameter* BuiltinProcessor::getParameterById(const juce::String& id) const
{
    for (auto* parameter : ranged_)
        if (parameter->getParameterID() == id)
            return parameter;
    return nullptr;
}

bool BuiltinProcessor::isBusesLayoutSupported(const BusesLayout& layout) const
{
    for (const auto& bus : layout.inputBuses)
        if (!bus.isDisabled() && bus != juce::AudioChannelSet::stereo())
            return false;
    for (const auto& bus : layout.outputBuses)
        if (!bus.isDisabled() && bus != juce::AudioChannelSet::stereo())
            return false;
    return true;
}

void BuiltinProcessor::prepareToPlay(double sampleRate, int maximumBlockSize)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxBlock_ = std::max(1, maximumBlockSize);
    const int channels = std::max({getTotalNumInputChannels(), getTotalNumOutputChannels(), 2});
    scratch_.setSize(channels, maxBlock_, false, true, false);
    prepareDsp(sampleRate_, maxBlock_);
}

void BuiltinProcessor::processBlock(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi)
{
    const juce::ScopedNoDenormals noDenormals;
    processDsp(buffer, midi);
}

void BuiltinProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    const juce::ScopedNoDenormals noDenormals;
    const int numChannels = std::min(buffer.getNumChannels(), std::max({getTotalNumInputChannels(), getTotalNumOutputChannels(), 2}));
    const int total = buffer.getNumSamples();

    // Hosts may exceed the announced block size; process in announced-size pieces (MIDI goes with
    // the first piece — sample positions beyond it are clamped by the device).
    for (int offset = 0; offset < total; offset += maxBlock_)
    {
        const int n = std::min(maxBlock_, total - offset);
        scratch_.setSize(numChannels, n, false, false, true); // within the allocation: no reallocation
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* src = buffer.getReadPointer(ch, offset);
            double* dst = scratch_.getWritePointer(ch);
            for (int i = 0; i < n; ++i)
                dst[i] = static_cast<double>(src[i]);
        }

        processDsp(scratch_, midi);
        if (offset == 0)
            midi.clear();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const double* src = scratch_.getReadPointer(ch);
            float* dst = buffer.getWritePointer(ch, offset);
            for (int i = 0; i < n; ++i)
                dst[i] = static_cast<float>(src[i]);
        }
    }
}

void BuiltinProcessor::getStateInformation(juce::MemoryBlock& destination)
{
    juce::XmlElement xml("DOJO_DEVICE");
    xml.setAttribute("type", typeId_);
    xml.setAttribute("version", 1);

    for (auto* parameter : ranged_)
    {
        auto* element = xml.createNewChildElement("PARAM");
        element->setAttribute("id", parameter->getParameterID());
        element->setAttribute("value", static_cast<double>(parameter->convertFrom0to1(parameter->getValue())));
    }

    auto* extra = xml.createNewChildElement("EXTRA");
    writeExtraState(*extra);
    copyXmlToBinary(xml, destination);
}

void BuiltinProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr || !xml->hasTagName("DOJO_DEVICE"))
        return;

    for (const auto* element : xml->getChildWithTagNameIterator("PARAM"))
    {
        if (auto* parameter = getParameterById(element->getStringAttribute("id")))
        {
            const auto value = static_cast<float>(element->getDoubleAttribute("value"));
            parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
        }
    }

    if (const auto* extra = xml->getChildByName("EXTRA"))
        readExtraState(*extra);
}

} // namespace dojo::devices
