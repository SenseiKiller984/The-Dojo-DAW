#include "devices/DeviceRegistry.h"

#include "devices/Dynamics.h"
#include "devices/Equaliser.h"
#include "devices/Instruments.h"
#include "devices/TimeEffects.h"

namespace dojo::devices
{

const std::vector<BuiltinDeviceInfo>& getBuiltinDevices()
{
    static const std::vector<BuiltinDeviceInfo> devices{
        {DrumRack::kTypeId, "Drum Rack", "Instrument", true},
        {Sampler::kTypeId, "Sampler", "Instrument", true},
        {Equaliser::kTypeId, "EQ", "EQ", false},
        {Compressor::kTypeId, "Compressor", "Dynamics", false},
        {Limiter::kTypeId, "Limiter", "Dynamics", false},
        {Reverb::kTypeId, "Reverb", "Space", false},
        {Delay::kTypeId, "Delay", "Space", false},
        {PitchShifter::kTypeId, "Pitch Shift", "Pitch", false},
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
    if (type == Sampler::kTypeId)
        return std::make_unique<Sampler>();
    if (type == DrumRack::kTypeId)
        return std::make_unique<DrumRack>();
    return nullptr;
}

} // namespace dojo::devices
