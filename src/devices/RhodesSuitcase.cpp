#include "devices/RhodesSuitcase.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices
{
namespace
{
using Shape = dsp::BiquadCoefficients::Shape;
constexpr double kPi = 3.14159265358979323846;
constexpr double kPreampGain = 0.6;           ///< a single mezzo-forte note at Volume 7: about 0 dBu
constexpr double kOpAmpClip = 0.1 * 11.220184543019634; ///< +21 dBu on +-15 V
constexpr double kPhotoBrighten = 0.004;      ///< seconds: the photoresistor follows more light quickly ...
constexpr double kPhotoDarken = 0.035;        ///< ... and less light slowly

inline double saturate(double x, double hardness) noexcept
{
    const double magnitude = std::abs(x);
    if (magnitude < 1.0e-9)
        return x;
    return x / std::pow(1.0 + std::pow(magnitude, hardness), 1.0 / hardness);
}

/** A steady, repeatable variation per key (-1 .. 1). */
inline double variation(int key, double salt) noexcept
{
    const double v = std::sin(key * 12.9898 + salt * 78.233) * 43758.5453;
    return (v - std::floor(v)) * 2.0 - 1.0;
}

inline double onePole(double seconds, double rate) noexcept
{
    return 1.0 - std::exp(-1.0 / (std::max(1.0e-5, seconds) * rate));
}
} // namespace

//==================================================================================================
// The suitcase: vibrato, power amps, speakers

juce::String SuitcaseStage::speedText(float position)
{
    return juce::String(rateHz(position), 2) + " Hz";
}

float SuitcaseStage::speedFromText(const juce::String& text)
{
    const double number = parseParameterText(text, {});
    if (text.containsIgnoreCase("hz"))
        return static_cast<float>(std::clamp(10.0 * std::log10(std::max(number, 1.0)), 0.0, 10.0));
    return static_cast<float>(std::clamp(number, 0.0, 10.0)); // (a slider position)
}

void SuitcaseStage::prepare(double sampleRate)
{
    sampleRate_ = sampleRate;
    cabinet_ = -1;
    reset();
}

void SuitcaseStage::reset() noexcept
{
    phase_ = 0.0;
    light_ = {0.5, 0.5};
    gain_ = {1.0, 1.0};
    sag_ = {0.0, 0.0};
    for (auto& side : speaker_)
        for (auto& f : side)
            f.reset();
}

void SuitcaseStage::process(const Controls& controls, const double* mono, double* left, double* right, int n) noexcept
{
    const double increment = rateHz(controls.speed->get()) / sampleRate_;
    const double depth = std::clamp(static_cast<double>(controls.intensity->get()) / 10.0, 0.0, 1.0);
    const bool opto = controls.modulator->getIndex() == 0;
    const bool lineOut = controls.output->getIndex() == 1;
    const double master = dsp::dbToGain(controls.master->get()) * kUnitsToDbfs;
    const double compensation = 1.0 / (1.0 - 0.29 * depth); // (about equal loudness as the swing widens)
    const double brighten = onePole(kPhotoBrighten, sampleRate_), darken = onePole(kPhotoDarken, sampleRate_);
    const double fet = onePole(0.001, sampleRate_);
    const double ampClip = kNominal * dsp::dbToGain(kAmpHeadroomDb);
    const double sagAttack = onePole(0.05, sampleRate_), sagRelease = onePole(0.4, sampleRate_);

    const int cabinet = controls.cabinet->getIndex();
    if (cabinet != cabinet_)
    {
        cabinet_ = cabinet;
        const double top = 0.45 * sampleRate_;
        if (cabinet == 0) // four 10-inch speakers, a parallel pair per side
            speakerCoefficients_ = {dsp::BiquadCoefficients::make(Shape::lowCut, sampleRate_, 32.0, 0.85, 0.0),
                                    dsp::BiquadCoefficients::make(Shape::peak, sampleRate_, 110.0, 1.0, 1.5),
                                    dsp::BiquadCoefficients::make(Shape::peak, sampleRate_, 2800.0, 1.2, 2.5),
                                    dsp::BiquadCoefficients::make(Shape::highCut, sampleRate_, std::min(13000.0, top), 0.6, 0.0)};
        else // two 12-inch speakers
            speakerCoefficients_ = {dsp::BiquadCoefficients::make(Shape::lowCut, sampleRate_, 30.0, 1.0, 0.0),
                                    dsp::BiquadCoefficients::make(Shape::peak, sampleRate_, 90.0, 1.0, 2.5),
                                    dsp::BiquadCoefficients::make(Shape::peak, sampleRate_, 2000.0, 1.0, 1.5),
                                    dsp::BiquadCoefficients::make(Shape::highCut, sampleRate_, std::min(9000.0, top), 0.65, 0.0)};
    }

    double peak = 0.0, lfo = 0.0;
    for (int i = 0; i < n; ++i)
    {
        // The LFO: a smoothed triangle.
        phase_ += increment;
        if (phase_ >= 1.0)
            phase_ -= 1.0;
        const double triangle = 4.0 * std::abs(phase_ - 0.5) - 1.0;
        lfo = std::sin(0.5 * kPi * triangle);

        double* out[2] = {left, right};
        for (std::size_t side = 0; side < 2; ++side)
        {
            const double drive = side == 0 ? lfo : -lfo; // (180 degrees apart)
            double gain = 1.0;
            if (opto)
            {
                // The LED lights the photoresistor: it follows quickly as the light rises and slowly as
                // it falls, so the swing is rounded and never quite goes dark at fast speeds.
                const double light = 0.5 * (1.0 + drive);
                light_[side] += (light - light_[side]) * (light > light_[side] ? brighten : darken);
                gain = (1.0 - depth) + depth * std::pow(std::max(0.0, light_[side]), 0.7);
            }
            else
            {
                gain_[side] += ((1.0 - depth) + depth * 0.5 * (1.0 + drive) - gain_[side]) * fet;
                gain = gain_[side];
            }
            double y = mono[i] * gain * compensation;
            if (!lineOut)
            {
                // The power amp: clean to its rated 40 W, then into the (sagging) rails.
                const double clip = ampClip * (1.0 - 0.1 * std::min(1.0, sag_[side]));
                y = clip * saturate(y / clip, kAmpHardness);
                const double load = std::abs(y) / ampClip;
                sag_[side] += (load - sag_[side]) * (load > sag_[side] ? sagAttack : sagRelease);
                peak = std::max(peak, std::abs(y));
                auto& filters = speaker_[side];
                for (std::size_t k = 0; k < filters.size(); ++k)
                    y = filters[k].process(y, speakerCoefficients_[k]);
            }
            out[side][i] = y * master;
        }
    }
    lfoOut_.store(static_cast<float>(lfo), std::memory_order_relaxed);
    if (!lineOut && peak > 0.0)
    {
        const float db = static_cast<float>(dsp::gainToDb(peak / kNominal)); // (re the rated 40 W)
        float previous = ampPeak_.load(std::memory_order_relaxed);
        while (db > previous && !ampPeak_.compare_exchange_weak(previous, db, std::memory_order_relaxed))
        {
        }
    }
}

//==================================================================================================
// Rhodes Suitcase

double RhodesSuitcase::frequencyOf(int note, double cents) noexcept
{
    return 440.0 * std::pow(2.0, (note - 69 + cents / 100.0) / 12.0);
}

RhodesSuitcase::RhodesSuitcase()
    : BuiltinProcessor(kTypeId, "1980s Rhodes Suitcase 73 Mk II",
                       BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true), true)
{
    voicing_ = addFloat("voicing", "Voicing", {0.0f, 100.0f, 0.1f}, 50.0f, "%");
    hammer_ = addFloat("hammer", "Hammer", {0.0f, 100.0f, 0.1f}, 50.0f, "%");
    tine_ = addFloat("tine", "Tine", {0.0f, 100.0f, 0.1f}, 50.0f, "%");
    decay_ = addFloat("decay", "Decay", {25.0f, 400.0f, 1.0f}, 100.0f, "%");
    release_ = addFloat("release", "Release", skewedRange(30.0f, 800.0f, 150.0f), 140.0f, "ms");
    velocityCurve_ = addFloat("velcurve", "Velocity Curve", {0.6f, 2.4f, 0.01f}, 1.4f);
    age_ = addFloat("age", "Age", {0.0f, 100.0f, 0.1f}, 25.0f, "%");
    tuning_ = addFloat("tuning", "Tuning", {-50.0f, 50.0f, 0.1f}, 0.0f, "ct");
    load_ = addChoice("load", "Harp Load", {"Janus (1 MOhm)", "Amp (100 kOhm)", "Mixer (10 kOhm)"}, 0);
    volume_ = addFloat("volume", "Volume", {0.0f, 10.0f, 0.01f}, 7.0f);
    treble_ = addFloat("treble", "Treble", {-12.0f, 12.0f, 0.1f}, 0.0f, "dB");
    bass_ = addFloat("bass", "Bass", {-15.0f, 15.0f, 0.1f}, 0.0f, "dB");
    loop_ = addBool("loop", "Accessory Loop", false);
    stage.speed = addFloatWithText("speed", "Speed", {0.0f, 10.0f, 0.01f}, 4.0f, {}, SuitcaseStage::speedText, SuitcaseStage::speedFromText);
    stage.intensity = addFloat("intensity", "Intensity", {0.0f, 10.0f, 0.01f}, 3.0f);
    stage.modulator = addChoice("modulator", "Modulator", {"Opto", "JFET"}, 0);
    stage.output = addChoice("out", "Output", {"Speakers", "Line Out"}, 0);
    stage.cabinet = addChoice("cabinet", "Speakers", {"4x10\"", "2x12\""}, 0);
    stage.master = addFloat("master", "Master", {-24.0f, 6.0f, 0.1f}, 0.0f, "dB");
}

void RhodesSuitcase::prepareDsp(double sampleRate, int maximumBlockSize)
{
    harp_.assign(static_cast<std::size_t>(maximumBlockSize), 0.0);
    for (auto& key : keys_)
        key = Key{};
    sustainPedal_ = false;
    loadFilter_.reset();
    trebleFilter_.reset();
    bassFilter_.reset();
    stage_.prepare(sampleRate);
    signature_.fill(-1.0e9f);
    updateKeys();
}

double RhodesSuitcase::flux(const Key& key, double x) const noexcept
{
    // The pickup's flux against the tine's position (in pickup gaps): strongest with the tine in
    // front of the pole piece, falling off to either side.
    const double u = x - key.offset;
    return 1.0 / (1.0 + u * u);
}

bool RhodesSuitcase::damped(int key) const noexcept
{
    const auto& k = keys_[static_cast<std::size_t>(key)];
    return !k.held && !sustainPedal_ && key < kKeys - kUndampedKeys;
}

void RhodesSuitcase::updateKeys() noexcept
{
    const std::array<float, 8> signature{decay_->get(), release_->get(), age_->get(), tuning_->get(), voicing_->get(),
                                         static_cast<float>(sampleRate_), 0.0f, 0.0f};
    if (signature == signature_)
        return;
    signature_ = signature;

    const double rate = sampleRate_;
    const double decay = decay_->get() / 100.0;
    const double release = release_->get() / 1000.0;
    const double age = age_->get() / 100.0;
    const double voicing = voicing_->get() / 100.0;
    for (int k = 0; k < kKeys; ++k)
    {
        auto& key = keys_[static_cast<std::size_t>(k)];
        const double t = static_cast<double>(k) / (kKeys - 1); // 0 at the bottom, 1 at the top
        key.frequency = frequencyOf(kLowestNote + k, tuning_->get() + 6.0 * age * variation(k, 1.0));
        key.ratio = {1.0, 1.0 + 0.0012 + 0.0025 * age * std::abs(variation(k, 2.0)), 5.9 + 0.8 * (1.0 - t) + 0.3 * age * variation(k, 3.0),
                     15.2 + age * variation(k, 4.0)};
        // The tines ring for seconds (longest in the bass); the bell overtones only briefly.
        const double fundamental = 9.0 * std::pow(0.12, t) * decay;
        const std::array<double, kModes> held{fundamental, fundamental * 0.85, 0.6 * std::pow(0.25, t) * std::sqrt(decay),
                                              0.08 * std::pow(0.4, t) * std::sqrt(decay)};
        const double felt = release * (1.5 - t);
        for (std::size_t m = 0; m < kModes; ++m)
        {
            const double frequency = key.frequency * key.ratio[m];
            const double w = 2.0 * kPi * frequency / rate;
            const auto step = [&](double seconds)
            { return frequency < 0.45 * rate ? std::polar(std::pow(10.0, -3.0 / (seconds * rate)), w) : std::complex<double>{}; };
            key.heldStep[m] = step(held[m]);
            key.dampedStep[m] = step(std::min(held[m], felt * (m >= 2 ? 0.5 : 1.0)));
        }
        // The tine's alignment with its pickup (in pickup gaps), and how far it swings.
        key.offset = (1.6 - 0.85 * voicing) * (1.0 + 0.12 * age * variation(k, 5.0));
        key.swing = (0.3 + 0.55 * voicing) * (1.0 + 0.1 * age * variation(k, 6.0)) * (1.15 - 0.3 * t);
        const double c = key.offset;
        const double slope = 2.0 * c / ((1.0 + c * c) * (1.0 + c * c));
        key.normal = 1.0 / (2.0 * kPi * key.frequency / rate * slope); // (fundamental out = displacement)
    }
}

void RhodesSuitcase::strike(int index, double velocity) noexcept
{
    auto& key = keys_[static_cast<std::size_t>(index)];
    const double touch = std::pow(std::clamp(velocity, 0.0, 1.0), static_cast<double>(velocityCurve_->get()));
    const double amplitude = key.swing * touch;
    // The hammer: harder tips and harder playing touch the tine more briefly, which reaches the bell
    // overtones. (The hammers are graded along the keyboard: the contact lasts a similar part of
    // each tine's period.)
    const double hardness = hammer_->get() / 100.0;
    const double contact = std::clamp((0.32 - 0.18 * touch) * (1.25 - 0.6 * hardness), 0.04, 0.45) / key.frequency;
    const double bell = tine_->get() / 100.0;
    const double age = age_->get() / 100.0;
    const std::array<double, kModes> weight{1.0, 0.04 + 0.1 * age, 0.55 * bell, 0.3 * bell};
    if (!key.active)
    {
        key.z.fill({});
        key.previousFlux = flux(key, 0.0);
        key.active = true;
    }
    for (std::size_t m = 0; m < kModes; ++m)
    {
        // (The pickup outputs the tine's velocity, which rises with the mode's frequency: hence the
        // division. Modes above the band are left out.)
        const double frequency = key.frequency * key.ratio[m];
        key.pulse[m] = frequency < 0.45 * sampleRate_ ? amplitude * weight[m] / key.ratio[m] : 0.0;
    }
    // The force lasts the contact (a raised-cosine pulse): modes faster than it barely move.
    key.contactLength = std::max(1, static_cast<int>(std::lround(contact * sampleRate_)));
    key.contactLeft = key.contactLength;
    key.held = true;
}

void RhodesSuitcase::release(int index) noexcept
{
    keys_[static_cast<std::size_t>(index)].held = false;
}

void RhodesSuitcase::handleMidi(const std::uint8_t* data, int size) noexcept
{
    if (size < 3)
        return;
    const int status = data[0] & 0xf0;
    const int note = data[1];
    const int value = data[2];
    const int key = note - kLowestNote;
    if (status == 0x90 && value > 0)
    {
        if (key >= 0 && key < kKeys)
            strike(key, value / 127.0);
    }
    else if (status == 0x80 || (status == 0x90 && value == 0))
    {
        if (key >= 0 && key < kKeys)
            release(key);
    }
    else if (status == 0xb0)
    {
        if (note == 64)
            sustainPedal_ = value >= 64;
        else if (note == 120 || note == 123) // all sound / notes off
        {
            for (auto& k : keys_)
                k.held = false;
            sustainPedal_ = false;
            if (note == 120)
                for (auto& k : keys_)
                    k.active = false;
        }
    }
}

void RhodesSuitcase::render(int from, int to) noexcept
{
    if (to <= from)
        return;
    for (int k = 0; k < kKeys; ++k)
    {
        auto& key = keys_[static_cast<std::size_t>(k)];
        if (!key.active)
            continue;
        const auto& step = damped(k) ? key.dampedStep : key.heldStep;
        for (int i = from; i < to; ++i)
        {
            if (key.contactLeft > 0)
            {
                // The hammer pushing on the tine.
                const double length = key.contactLength;
                const double position = (length - key.contactLeft + 0.5) / length;
                const double force = (1.0 - std::cos(2.0 * kPi * position)) / length; // (sums to 1)
                for (std::size_t m = 0; m < kModes; ++m)
                    key.z[m] += key.pulse[m] * force;
                --key.contactLeft;
            }
            double x = 0.0;
            for (std::size_t m = 0; m < kModes; ++m)
            {
                x += key.z[m].imag();
                key.z[m] *= step[m];
            }
            // The pickup: the voltage is the change of the flux.
            const double f = flux(key, x);
            harp_[static_cast<std::size_t>(i)] += (f - key.previousFlux) * key.normal;
            key.previousFlux = f;
        }
    }
}

void RhodesSuitcase::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept
{
    const int numSamples = buffer.getNumSamples();
    buffer.clear();
    updateKeys();
    std::fill(harp_.begin(), harp_.begin() + numSamples, 0.0);

    int rendered = 0;
    for (const auto metadata : midi)
    {
        const int offset = std::clamp(metadata.samplePosition, 0, std::max(0, numSamples - 1));
        if (offset > rendered)
            render(rendered, offset);
        rendered = std::max(rendered, offset);
        handleMidi(metadata.data, metadata.numBytes);
    }
    render(rendered, numSamples);

    // Keys that have died away stop sounding.
    int sounding = 0;
    std::array<std::uint64_t, 2> mask{};
    for (int k = 0; k < kKeys; ++k)
    {
        auto& key = keys_[static_cast<std::size_t>(k)];
        if (!key.active)
            continue;
        double energy = 0.0;
        for (const auto& z : key.z)
            energy += std::norm(z);
        if (energy < 1.0e-14 && !key.held && key.contactLeft == 0)
        {
            key.active = false;
            continue;
        }
        ++sounding;
        mask[static_cast<std::size_t>(k / 64)] |= std::uint64_t{1} << (k % 64);
    }
    sounding_.store(sounding, std::memory_order_relaxed);
    for (std::size_t w = 0; w < 2; ++w)
        keyMask_[w].store(mask[w], std::memory_order_relaxed);

    // The harp's load: its inductance (and the cable's capacitance) against the input it plugs into
    // makes a low-pass, the lower and more damped the lower the input's impedance.
    const int load = std::clamp(load_->getIndex(), 0, 2);
    constexpr std::array<double, 3> loadCorner{0.0, 6000.0, 1800.0};
    constexpr std::array<double, 3> loadQ{0.7071, 0.9, 0.5};
    const auto loadFilter = dsp::BiquadCoefficients::make(Shape::highCut, sampleRate_, std::min(loadCorner[static_cast<std::size_t>(load)], 0.45 * sampleRate_),
                                                          loadQ[static_cast<std::size_t>(load)], 0.0);
    const double volume = kPreampGain * std::pow(volume_->get() / 7.0, 2.2); // (an audio-taper slider)
    const auto treble = dsp::BiquadCoefficients::make(Shape::highShelf, sampleRate_, std::min(10000.0, 0.45 * sampleRate_), 0.7071, treble_->get());
    const auto bass = dsp::BiquadCoefficients::make(Shape::lowShelf, sampleRate_, 75.0, 0.7071, bass_->get());
    for (int i = 0; i < numSamples; ++i)
    {
        auto& s = harp_[static_cast<std::size_t>(i)];
        double y = (load > 0 ? loadFilter_.process(s, loadFilter) : s) * volume;
        y = bassFilter_.process(trebleFilter_.process(y, treble), bass);
        s = kOpAmpClip * saturate(y / kOpAmpClip, 6.0); // (the op-amps on their +-15 V rails)
    }

    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);
    if (loop_->get())
    {
        // The accessory loop's Send: post-EQ, pre-vibrato, 0 dBu.
        for (int i = 0; i < numSamples; ++i)
            left[i] = right[i] = harp_[static_cast<std::size_t>(i)] * SuitcaseStage::kUnitsToDbfs;
        return;
    }
    stage_.process(stage, harp_.data(), left, right, numSamples);
}

//==================================================================================================
// Suitcase Base

SuitcaseBase::SuitcaseBase()
    : BuiltinProcessor(kTypeId, "Suitcase Base",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
    stage.speed = addFloatWithText("speed", "Speed", {0.0f, 10.0f, 0.01f}, 4.0f, {}, SuitcaseStage::speedText, SuitcaseStage::speedFromText);
    stage.intensity = addFloat("intensity", "Intensity", {0.0f, 10.0f, 0.01f}, 3.0f);
    stage.modulator = addChoice("modulator", "Modulator", {"Opto", "JFET"}, 0);
    stage.output = addChoice("out", "Output", {"Speakers", "Line Out"}, 0);
    stage.cabinet = addChoice("cabinet", "Speakers", {"4x10\"", "2x12\""}, 0);
    stage.master = addFloat("master", "Master", {-24.0f, 6.0f, 0.1f}, 0.0f, "dB");
}

void SuitcaseBase::prepareDsp(double sampleRate, int maximumBlockSize)
{
    mono_.assign(static_cast<std::size_t>(maximumBlockSize), 0.0);
    stage_.prepare(sampleRate);
}

void SuitcaseBase::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);
    for (int i = 0; i < numSamples; ++i)
        mono_[static_cast<std::size_t>(i)] = 0.5 * (left[i] + right[i]) / SuitcaseStage::kUnitsToDbfs; // (-18 dBFS = 0 dBu)
    stage_.process(stage, mono_.data(), left, right, numSamples);
}

} // namespace dojo::devices
