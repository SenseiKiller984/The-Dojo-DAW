#include "devices/DeviceRegistry.h"

#include "devices/AmpCabinet.h"
#include "devices/Dynamics.h"
#include "devices/Echo.h"
#include "devices/Equaliser.h"
#include "devices/Impulse.h"
#include "devices/Instruments.h"
#include "devices/Operator.h"
#include "devices/TimeEffects.h"
#include "devices/Utility.h"
#include "devices/WavetableSynth.h"

namespace dojo::devices
{

const std::vector<BuiltinDeviceInfo>& getBuiltinDevices()
{
    static const std::vector<BuiltinDeviceInfo> devices{
        {DrumRack::kTypeId, "Drum Rack", "Instrument", true},
        {Sampler::kTypeId, "Sampler", "Instrument", true},
        {Impulse::kTypeId, "Impulse", "Drums", true},
        {WavetableSynth::kTypeId, "Wavetable", "Synth", true},
        {Operator::kTypeId, "Operator", "Synth", true},
        {Equaliser::kTypeId, "EQ", "EQ", false},
        {Compressor::kTypeId, "Compressor", "Dynamics", false},
        {Limiter::kTypeId, "Limiter", "Dynamics", false},
        {Reverb::kTypeId, "Reverb", "Space", false},
        {Delay::kTypeId, "Delay", "Space", false},
        {PitchShifter::kTypeId, "Pitch Shift", "Pitch", false},
        {Echo::kTypeId, "Echo", "Space", false},
        {Utility::kTypeId, "Utility", "Utility", false},
        {AmpCabinet::kTypeId, "Amp & Cabinet", "Drive", false},
    };
    return devices;
}

const BuiltinDeviceInfo* findBuiltinDevice(const juce::String& type)
{
    for (const auto& info : getBuiltinDevices())
        if (info.type == type)
            return &info;
    return nullptr;
}

bool isBuiltinType(const juce::String& type)
{
    return type.startsWith("builtin:");
}

std::unique_ptr<juce::AudioProcessor> createBuiltinDevice(const juce::String& type)
{
    if (type == Equaliser::kTypeId)
        return std::make_unique<Equaliser>();
    if (type == Compressor::kTypeId)
        return std::make_unique<Compressor>();
    if (type == Limiter::kTypeId)
        return std::make_unique<Limiter>();
    if (type == Reverb::kTypeId)
        return std::make_unique<Reverb>();
    if (type == Delay::kTypeId)
        return std::make_unique<Delay>();
    if (type == PitchShifter::kTypeId)
        return std::make_unique<PitchShifter>();
    if (type == Echo::kTypeId)
        return std::make_unique<Echo>();
    if (type == Utility::kTypeId)
        return std::make_unique<Utility>();
    if (type == AmpCabinet::kTypeId)
        return std::make_unique<AmpCabinet>();
    if (type == Sampler::kTypeId)
        return std::make_unique<Sampler>();
    if (type == Impulse::kTypeId)
        return std::make_unique<Impulse>();
    if (type == WavetableSynth::kTypeId)
        return std::make_unique<WavetableSynth>();
    if (type == Operator::kTypeId)
        return std::make_unique<Operator>();
    if (type == DrumRack::kTypeId)
        return std::make_unique<DrumRack>();
    return nullptr;
}

} // namespace dojo::devices
