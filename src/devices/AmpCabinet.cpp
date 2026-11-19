#include "devices/AmpCabinet.h"

#include <juce_dsp/juce_dsp.h>

#include <cmath>

namespace dojo::devices
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
using Shape = dsp::BiquadCoefficients::Shape;

struct AmpModel
{
    double driveMin, driveMax; ///< total preamp gain at Gain 0 and 10
    double inputHighPass;      ///< tightness: how much low end reaches the preamp
    double bias;               ///< asymmetry of the gain stages (even harmonics)
    int stages;
    double bassFrequency, midFrequency, trebleFrequency;
    double trim; ///< output level
};

constexpr std::array<AmpModel, 7> kModels{{
    {0.6, 4.0, 60.0, 0.0, 1, 100.0, 600.0, 2800.0, 1.0},     // Clean
    {1.0, 14.0, 90.0, 0.08, 1, 110.0, 800.0, 3000.0, 0.9},   // Boost
    {1.5, 40.0, 80.0, 0.12, 2, 100.0, 700.0, 2600.0, 0.8},   // Blues
    {3.0, 90.0, 100.0, 0.15, 2, 110.0, 650.0, 3000.0, 0.7},  // Rock
    {6.0, 250.0, 120.0, 0.2, 3, 120.0, 900.0, 3200.0, 0.6},  // Lead
    {8.0, 400.0, 150.0, 0.1, 3, 90.0, 500.0, 3500.0, 0.6},   // Heavy
    {1.0, 30.0, 30.0, 0.05, 2, 60.0, 500.0, 2000.0, 0.8},    // Bass
}};

struct CabinetFilter
{
    Shape shape;
    double frequency, q, gainDb;
};

/** Speaker models: low resonance, cone break-up and a steep (24 dB/oct) high roll-off. */
const std::array<std::vector<CabinetFilter>, 5>& cabinetModels()
{
    static const std::array<std::vector<CabinetFilter>, 5> models{{
        // 1x12
        {{Shape::lowCut, 80.0, 0.7, 0.0}, {Shape::peak, 110.0, 1.2, 3.0}, {Shape::peak, 500.0, 1.0, -2.0},
         {Shape::peak, 2200.0, 1.0, 3.0}, {Shape::highCut, 5500.0, 0.7, 0.0}, {Shape::highCut, 5500.0, 0.7, 0.0}},
        // 2x12
        {{Shape::lowCut, 75.0, 0.7, 0.0}, {Shape::peak, 100.0, 1.2, 4.0}, {Shape::peak, 600.0, 1.0, -3.0},
         {Shape::peak, 1800.0, 1.0, 3.0}, {Shape::highCut, 5000.0, 0.7, 0.0}, {Shape::highCut, 5000.0, 0.7, 0.0}},
        // 4x12
        {{Shape::lowCut, 70.0, 0.7, 0.0}, {Shape::peak, 95.0, 1.4, 5.0}, {Shape::peak, 400.0, 1.0, -2.0},
         {Shape::peak, 1500.0, 1.0, 2.5}, {Shape::peak, 2800.0, 2.0, 3.0}, {Shape::highCut, 4600.0, 0.7, 0.0},
         {Shape::highCut, 4600.0, 0.7, 0.0}},
        // 4x10 Bass
        {{Shape::lowCut, 45.0, 0.7, 0.0}, {Shape::peak, 80.0, 1.2, 4.0}, {Shape::peak, 1200.0, 1.0, 2.0},
         {Shape::highCut, 4000.0, 0.7, 0.0}, {Shape::highCut, 4000.0, 0.7, 0.0}},
        // 1x15 Bass
        {{Shape::lowCut, 35.0, 0.7, 0.0}, {Shape::peak, 70.0, 1.2, 5.0}, {Shape::peak, 800.0, 1.0, 2.0},
         {Shape::highCut, 3000.0, 0.7, 0.0}, {Shape::highCut, 3000.0, 0.7, 0.0}},
    }};
    return models;
}
} // namespace

const juce::StringArray& AmpCabinet::ampNames()
{
    static const juce::StringArray names{"Clean", "Boost", "Blues", "Rock", "Lead", "Heavy", "Bass"};
    return names;
}

const juce::StringArray& AmpCabinet::cabinetNames()
{
    static const juce::StringArray names{"Off", "1x12", "2x12", "4x12", "4x10 Bass", "1x15 Bass"};
    return names;
}

AmpCabinet::AmpCabinet()
    : BuiltinProcessor(kTypeId, "Amp & Cabinet",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
    amp_ = addChoice("amp", "Amp", ampNames(), 3);
    gain_ = addFloat("gain", "Gain", {0.0f, 10.0f, 0.01f}, 5.0f);
    bass_ = addFloat("bass", "Bass", {0.0f, 10.0f, 0.01f}, 5.0f);
    middle_ = addFloat("middle", "Middle", {0.0f, 10.0f, 0.01f}, 5.0f);
    treble_ = addFloat("treble", "Treble", {0.0f, 10.0f, 0.01f}, 5.0f);
    presence_ = addFloat("presence", "Presence", {0.0f, 10.0f, 0.01f}, 5.0f);
    volume_ = addFloat("volume", "Volume", {-36.0f, 12.0f, 0.1f}, 0.0f, "dB");
    cabinet_ = addChoice("cabinet", "Cabinet", cabinetNames(), 3);
    microphone_ = addChoice("mic", "Microphone", {"Near On-Axis", "Near Off-Axis", "Far"}, 0);
    mix_ = addFloat("mix", "Dry/Wet", {0.0f, 100.0f, 0.1f}, 100.0f, "%");
}

AmpCabinet::~AmpCabinet() = default;

void AmpCabinet::prepareDsp(double sampleRate, int maximumBlockSize)
{
    oversampling_ = std::make_unique<juce::dsp::Oversampling<double>>(
        2, 2, juce::dsp::Oversampling<double>::filterHalfBandFIREquiripple, true, true);
    oversampling_->initProcessing(static_cast<std::size_t>(maximumBlockSize));
    oversampling_->reset();
    latency_ = static_cast<int>(std::lround(oversampling_->getLatencyInSamples()));
    setLatencySamples(latency_);

    interstageCoefficient_ = 1.0 - std::exp(-2.0 * kPi * 7000.0 / (sampleRate * 4.0));
    dcCoefficient_ = std::exp(-2.0 * kPi * 15.0 / sampleRate);
    const auto roomSize = static_cast<std::size_t>(0.004 * sampleRate) + 2;
    for (auto& r : room_)
        r.assign(roomSize, 0.0);
    roomWrite_ = 0;
    for (auto& d : dryDelay_)
        d.assign(static_cast<std::size_t>(latency_ + maximumBlockSize + 1), 0.0); // a block is written before it is read
    dryWrite_ = 0;

    for (auto& f : inputFilter_)
        f.reset();
    for (auto& channel : toneFilters_)
        for (auto& f : channel)
            f.reset();
    for (auto& channel : cabinetFilters_)
        for (auto& f : channel)
            f.reset();
    interstage_ = {};
    interstage2_ = {};
    dcIn_ = {};
    dcOut_ = {};
    shownAmp_ = shownCabinet_ = shownMicrophone_ = -1;
    shownTone_.fill(-1.0f);
    updateFilters();
}

void AmpCabinet::updateFilters() noexcept
{
    const int amp = amp_->getIndex();
    const int cabinet = cabinet_->getIndex();
    const int microphone = microphone_->getIndex();
    const std::array<float, 5> tone{gain_->get(), bass_->get(), middle_->get(), treble_->get(), presence_->get()};

    bool toneChanged = amp != shownAmp_;
    for (std::size_t i = 0; i < tone.size(); ++i)
        toneChanged = toneChanged || std::abs(tone[i] - shownTone_[i]) > 1.0e-4f;

    const auto& model = kModels[static_cast<std::size_t>(std::clamp(amp, 0, static_cast<int>(kModels.size()) - 1))];
    if (toneChanged)
    {
        shownTone_ = tone;
        inputHighPass_ = dsp::BiquadCoefficients::make(Shape::lowCut, sampleRate_, model.inputHighPass, 0.7, 0.0);
        driveGain_ = model.driveMin * std::pow(model.driveMax / model.driveMin, tone[0] / 10.0);
        stageGain_ = std::pow(driveGain_, 1.0 / model.stages);
        bias_ = model.bias;
        stages_ = model.stages;
        outputTrim_ = model.trim;
        tone_[0] = dsp::BiquadCoefficients::make(Shape::lowShelf, sampleRate_, model.bassFrequency, 0.7, (tone[1] - 5.0) * 2.4);
        tone_[1] = dsp::BiquadCoefficients::make(Shape::peak, sampleRate_, model.midFrequency, 0.8, (tone[2] - 5.0) * 2.4);
        tone_[2] = dsp::BiquadCoefficients::make(Shape::highShelf, sampleRate_, model.trebleFrequency, 0.7, (tone[3] - 5.0) * 2.4);
        tone_[3] = dsp::BiquadCoefficients::make(Shape::highShelf, sampleRate_, 5000.0, 0.7, (tone[4] - 5.0) * 1.8);
    }
    shownAmp_ = amp;

    if (cabinet == shownCabinet_ && microphone == shownMicrophone_)
        return;
    shownCabinet_ = cabinet;
    shownMicrophone_ = microphone;

    numCabinetFilters_ = 0;
    roomDelay_ = 0;
    if (cabinet <= 0)
        return;

    constexpr std::array<double, 3> lowPassScale{1.15, 0.8, 0.9};
    constexpr std::array<double, 3> highPassScale{1.0, 1.0, 1.3};
    const auto mic = static_cast<std::size_t>(std::clamp(microphone, 0, 2));
    const auto& filters = cabinetModels()[static_cast<std::size_t>(std::min(cabinet, 5) - 1)];
    for (const auto& f : filters)
    {
        double frequency = f.frequency;
        if (f.shape == Shape::highCut)
            frequency *= lowPassScale[mic];
        else if (f.shape == Shape::lowCut)
            frequency *= highPassScale[mic];
        if (numCabinetFilters_ < kMaxCabinetFilters)
            cabinetCoefficients_[static_cast<std::size_t>(numCabinetFilters_++)] =
                dsp::BiquadCoefficients::make(f.shape, sampleRate_, frequency, f.q, f.gainDb);
    }
    if (mic != 2 && numCabinetFilters_ < kMaxCabinetFilters)
        cabinetCoefficients_[static_cast<std::size_t>(numCabinetFilters_++)] =
            dsp::BiquadCoefficients::make(Shape::peak, sampleRate_, 3500.0, mic == 0 ? 1.5 : 1.0, mic == 0 ? 2.5 : -3.0);
    if (mic == 2)
        roomDelay_ = std::max(1, static_cast<int>(0.0013 * sampleRate_));
}

void AmpCabinet::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    if (oversampling_ == nullptr || numSamples <= 0)
        return;
    updateFilters();

    double* channels[2] = {buffer.getWritePointer(0), buffer.getWritePointer(1)};
    const int drySize = static_cast<int>(dryDelay_[0].size());

    // Dry copy (delayed later by the latency) and the input high-pass.
    for (int i = 0; i < numSamples; ++i)
    {
        const int slot = (dryWrite_ + i) % drySize;
        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            dryDelay_[ch][static_cast<std::size_t>(slot)] = channels[ch][i];
            channels[ch][i] = inputFilter_[ch].process(channels[ch][i], inputHighPass_);
        }
    }

    // Preamp, 4x oversampled.
    juce::dsp::AudioBlock<double> block(channels, 2, 0, static_cast<std::size_t>(numSamples));
    auto up = oversampling_->processSamplesUp(block);
    const double restingOutput = std::tanh(bias_);
    for (std::size_t ch = 0; ch < 2; ++ch)
    {
        double* data = up.getChannelPointer(ch);
        for (std::size_t j = 0; j < up.getNumSamples(); ++j)
        {
            double x = data[j];
            for (int stage = 0; stage < stages_; ++stage)
            {
                x = std::tanh(x * stageGain_ + bias_) - restingOutput;
                if (stage == 0 && stages_ > 1)
                {
                    interstage_[ch] += (x - interstage_[ch]) * interstageCoefficient_;
                    x = interstage_[ch];
                }
                else if (stage == 1 && stages_ > 2)
                {
                    interstage2_[ch] += (x - interstage2_[ch]) * interstageCoefficient_;
                    x = interstage2_[ch];
                }
            }
            data[j] = x;
        }
    }
    oversampling_->processSamplesDown(block);

    // Tone stack, power stage, cabinet, output.
    const double volume = juce::Decibels::decibelsToGain(static_cast<double>(volume_->get()), -100.0);
    const double mix = mix_->get() / 100.0;
    const int roomSize = static_cast<int>(room_[0].size());
    for (int i = 0; i < numSamples; ++i)
    {
        const int drySlot = ((dryWrite_ + i - latency_) % drySize + drySize) % drySize;
        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            double x = channels[ch][i];
            for (std::size_t t = 0; t < tone_.size(); ++t)
                x = toneFilters_[ch][t].process(x, tone_[t]);
            x = std::tanh(x * 0.9) / 0.9 * outputTrim_;
            for (int f = 0; f < numCabinetFilters_; ++f)
                x = cabinetFilters_[ch][static_cast<std::size_t>(f)].process(x, cabinetCoefficients_[static_cast<std::size_t>(f)]);
            if (numCabinetFilters_ > 0)
                x *= 0.7; // the speaker resonances add up to ~+3 dB
            if (roomDelay_ > 0)
            {
                auto& room = room_[ch];
                const double reflection = room[static_cast<std::size_t>((roomWrite_ - roomDelay_ + roomSize) % roomSize)];
                room[static_cast<std::size_t>(roomWrite_)] = x;
                x += 0.35 * reflection;
            }
            const double y = x - dcIn_[ch] + dcCoefficient_ * dcOut_[ch];
            dcIn_[ch] = x;
            dcOut_[ch] = y;
            channels[ch][i] = dryDelay_[ch][static_cast<std::size_t>(drySlot)] * (1.0 - mix) + y * volume * mix;
        }
        roomWrite_ = (roomWrite_ + 1) % roomSize;
    }
    dryWrite_ = (dryWrite_ + numSamples) % drySize;
}

} // namespace dojo::devices
