#include "devices/DeviceRegistry.h"

#include "devices/AmpCabinet.h"
#include "devices/AnalogSynth.h"
#include "devices/AutoFilter.h"
#include "devices/ChannelEq.h"
#include "devices/DriftSynth.h"
#include "devices/Drive.h"
#include "devices/Gate.h"
#include "devices/Modulators.h"
#include "devices/MultibandDynamics.h"
#include "devices/Dynamics.h"
#include "devices/Echo.h"
#include "devices/Equaliser.h"
#include "devices/Impulse.h"
#include "devices/Instruments.h"
#include "devices/LfoDevice.h"
#include "devices/Meld.h"
#include "devices/Operator.h"
#include "devices/PlateReverb.h"
#include "devices/Roar.h"
#include "devices/StudioDynamics.h"
#include "devices/TimeEffects.h"
#include "devices/Utility.h"
#include "devices/Click.h"
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
        {Meld::kTypeId, "Meld", "Synth", true},
        {AnalogSynth::kTypeId, "Analog", "Synth", true},
        {DriftSynth::kTypeId, "Drift", "Synth", true},
        {Equaliser::kTypeId, "EQ", "EQ", false},
        {ChannelEq::kTypeId, "Channel EQ", "EQ", false},
        {AutoFilter::kTypeId, "Auto Filter", "Filter", false},
        {Compressor::kTypeId, "Compressor", "Dynamics", false},
        {Limiter::kTypeId, "Limiter", "Dynamics", false},
        {Gate::kTypeId, "Gate", "Dynamics", false},
        {MultibandDynamics::kTypeId, "Multiband Dynamics", "Dynamics", false},
        {TheGlue::kTypeId, "The Glue", "Dynamics", false, "bus compressor glue console"},
        {Expander::kTypeId, "Expander", "Dynamics", false, "noise gate upward expansion"},
        {DeEsser::kTypeId, "De-Esser", "Dynamics", false, "sibilance deesser"},
        {SoftClipper::kTypeId, "Soft Clipper", "Dynamics", false, "clip saturation loudness"},
        {Reverb::kTypeId, "Reverb", "Space", false},
        {PlateReverb::kTypeId, "Plate Reverb", "Space", false, "plate reverb"},
        {Delay::kTypeId, "Delay", "Space", false},
        {PitchShifter::kTypeId, "Pitch Shift", "Pitch", false},
        {Shifter::kTypeId, "Shifter", "Pitch", false},
        {ChorusEnsemble::kTypeId, "Chorus-Ensemble", "Modulation", false},
        {AutoPan::kTypeId, "Auto Pan", "Modulation", false},
        {LfoDevice::kTypeId, "LFO", "Modulation", false},
        {Echo::kTypeId, "Echo", "Space", false},
        {Utility::kTypeId, "Utility", "Utility", false},
        {AmpCabinet::kTypeId, "Amp & Cabinet", "Drive", false},
        {Roar::kTypeId, "Roar", "Drive", false},
        {Pedal::kTypeId, "Pedal", "Drive", false},
        {Overdrive::kTypeId, "Overdrive", "Drive", false},
        {Redux::kTypeId, "Redux", "Drive", false},
        {Click::kTypeId, "Click", "Utility", false},
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
    if (type == ChannelEq::kTypeId)
        return std::make_unique<ChannelEq>();
    if (type == AutoFilter::kTypeId)
        return std::make_unique<AutoFilter>();
    if (type == Gate::kTypeId)
        return std::make_unique<Gate>();
    if (type == MultibandDynamics::kTypeId)
        return std::make_unique<MultibandDynamics>();
    if (type == Compressor::kTypeId)
        return std::make_unique<Compressor>();
    if (type == TheGlue::kTypeId)
        return std::make_unique<TheGlue>();
    if (type == Expander::kTypeId)
        return std::make_unique<Expander>();
    if (type == DeEsser::kTypeId)
        return std::make_unique<DeEsser>();
    if (type == SoftClipper::kTypeId)
        return std::make_unique<SoftClipper>();
    if (type == PlateReverb::kTypeId)
        return std::make_unique<PlateReverb>();
    if (type == Limiter::kTypeId)
        return std::make_unique<Limiter>();
    if (type == Reverb::kTypeId)
        return std::make_unique<Reverb>();
    if (type == Delay::kTypeId)
        return std::make_unique<Delay>();
    if (type == PitchShifter::kTypeId)
        return std::make_unique<PitchShifter>();
    if (type == Shifter::kTypeId)
        return std::make_unique<Shifter>();
    if (type == ChorusEnsemble::kTypeId)
        return std::make_unique<ChorusEnsemble>();
    if (type == AutoPan::kTypeId)
        return std::make_unique<AutoPan>();
    if (type == LfoDevice::kTypeId)
        return std::make_unique<LfoDevice>();
    if (type == Echo::kTypeId)
        return std::make_unique<Echo>();
    if (type == Utility::kTypeId)
        return std::make_unique<Utility>();
    if (type == Click::kTypeId)
        return std::make_unique<Click>();
    if (type == Roar::kTypeId)
        return std::make_unique<Roar>();
    if (type == Pedal::kTypeId)
        return std::make_unique<Pedal>();
    if (type == Overdrive::kTypeId)
        return std::make_unique<Overdrive>();
    if (type == Redux::kTypeId)
        return std::make_unique<Redux>();
    if (type == AmpCabinet::kTypeId)
        return std::make_unique<AmpCabinet>();
    if (type == Sampler::kTypeId)
        return std::make_unique<Sampler>();
    if (type == Impulse::kTypeId)
        return std::make_unique<Impulse>();
    if (type == WavetableSynth::kTypeId)
        return std::make_unique<WavetableSynth>();
    if (type == Meld::kTypeId)
        return std::make_unique<Meld>();
    if (type == AnalogSynth::kTypeId)
        return std::make_unique<AnalogSynth>();
    if (type == DriftSynth::kTypeId)
        return std::make_unique<DriftSynth>();
    if (type == Operator::kTypeId)
        return std::make_unique<Operator>();
    if (type == DrumRack::kTypeId)
        return std::make_unique<DrumRack>();
    return nullptr;
}

} // namespace dojo::devices
