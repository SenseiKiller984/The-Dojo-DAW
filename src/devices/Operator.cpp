#include "devices/Operator.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices
{
namespace
{
using dsp::kPi;
using dsp::kTwoPi;

constexpr double kModulationDepth = 1.5; ///< phase offset (in cycles) of a modulator at full level
constexpr std::uint8_t A = 1, B = 2, C = 4, D = 8;

enum Wave
{
    waveSine,
    waveSaw,
    waveSquare,
    waveTriangle,
    waveNoise,
};

enum LfoDestination
{
    toPitch,
    toCutoff,
    toVolume,
    toModulators,
};

/** Band-limited single-cycle tables for the operator waveforms (shared). */
const std::array<dsp::BandLimitedWavetable, 4>& waveTables()
{
    static const std::array<dsp::BandLimitedWavetable, 4> tables{
        dsp::BandLimitedWavetable("Sine", 1, [](double, double p) { return std::sin(kTwoPi * p); }),
        dsp::BandLimitedWavetable("Saw", 1, [](double, double p) { return 1.0 - 2.0 * p; }),
        dsp::BandLimitedWavetable("Square", 1, [](double, double p) { return p < 0.5 ? 1.0 : -1.0; }),
        dsp::BandLimitedWavetable("Triangle", 1, [](double, double p) { return p < 0.25 ? 4.0 * p : p < 0.75 ? 2.0 - 4.0 * p : 4.0 * p - 4.0; }),
    };
    return tables;
}
} // namespace

const std::array<Operator::Algorithm, Operator::kAlgorithms>& Operator::getAlgorithms()
{
    // modulators[i]: the operators modulating operator i (A = index 0). Modulators always have a
    // higher index than what they modulate, so D, C, B, A is a valid evaluation order.
    static const std::array<Algorithm, kAlgorithms> algorithms{{
        {{B, C, D, 0}, A, "D > C > B > A"},
        {{B, C | D, 0, 0}, A, "C + D > B > A"},
        {{B | C, 0, D, 0}, A, "D > C > A, B > A"},
        {{B | C, D, D, 0}, A, "D > (B, C) > A"},
        {{B, 0, D, 0}, A | C, "B > A, D > C"},
        {{D, D, D, 0}, A | B | C, "D > A, B, C"},
        {{0, C, D, 0}, A | B, "D > C > B, A"},
        {{0, 0, D, 0}, A | B | C, "D > C, A, B"},
        {{B | C | D, 0, 0, 0}, A, "B + C + D > A"},
        {{0, 0, 0, 0}, A | B | C | D, "A + B + C + D"},
        {{C, C, D, 0}, A | B, "D > C > (A, B)"},
    }};
    return algorithms;
}

juce::StringArray Operator::getAlgorithmNames()
{
    // Numbers: the editor draws the routing (and shows the description) next to the selector.
    juce::StringArray names;
    for (int i = 1; i <= kAlgorithms; ++i)
        names.add(juce::String(i));
    return names;
}

double Operator::ratioFor(float coarse, float fine) noexcept
{
    const double base = coarse < 0.5f ? 0.5 : std::round(static_cast<double>(coarse));
    return base * (1.0 + fine / 1000.0);
}

Operator::Operator()
    : BuiltinProcessor(kTypeId, "Operator", BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true), true)
{
    (void) waveTables();
    const juce::StringArray waves{"Sine", "Saw", "Square", "Triangle", "Noise"};
    constexpr std::array<float, kOperators> defaultLevel{80.0f, 35.0f, 0.0f, 0.0f};
    constexpr std::array<float, kOperators> defaultCoarse{1.0f, 2.0f, 1.0f, 1.0f};
    constexpr std::array<std::array<float, 4>, kOperators> defaultEnvelope{{{2.0f, 600.0f, 70.0f, 300.0f},
                                                                            {2.0f, 800.0f, 20.0f, 300.0f},
                                                                            {2.0f, 800.0f, 50.0f, 300.0f},
                                                                            {2.0f, 800.0f, 50.0f, 300.0f}}};

    for (std::size_t o = 0; o < operators_.size(); ++o)
    {
        const juce::String letter = juce::String::charToString(static_cast<juce::juce_wchar>('A' + o));
        const juce::String id = letter.toLowerCase();
        const juce::String name = letter + " ";
        auto& op = operators_[o];
        op.wave = addChoice(id + "wave", name + "Wave", waves, waveSine);
        op.coarse = addFloat(id + "coarse", name + "Coarse", {0.0f, 32.0f, 1.0f}, defaultCoarse[o]);
        op.fine = addFloat(id + "fine", name + "Fine", {0.0f, 1000.0f, 1.0f}, 0.0f);
        op.fixed = addBool(id + "fixed", name + "Fixed", false);
        op.frequency = addFloat(id + "freq", name + "Freq", skewedRange(1.0f, 20000.0f, 440.0f), 440.0f, "Hz");
        op.level = addFloat(id + "level", name + "Level", {0.0f, 100.0f, 0.1f}, defaultLevel[o], "%");
        op.attack = addFloat(id + "a", name + "Attack", skewedRange(0.5f, 10000.0f, 200.0f), defaultEnvelope[o][0], "ms");
        op.decay = addFloat(id + "d", name + "Decay", skewedRange(1.0f, 20000.0f, 500.0f), defaultEnvelope[o][1], "ms");
        op.sustain = addFloat(id + "s", name + "Sustain", {0.0f, 100.0f, 0.1f}, defaultEnvelope[o][2], "%");
        op.release = addFloat(id + "r", name + "Release", skewedRange(1.0f, 20000.0f, 500.0f), defaultEnvelope[o][3], "ms");
        op.velocity = addFloat(id + "vel", name + "Velocity", {0.0f, 100.0f, 0.1f}, o == 0 ? 50.0f : 30.0f, "%");
    }

    algorithm_ = addChoice("algo", "Algorithm", getAlgorithmNames(), 0);
    feedback_ = addFloat("feedback", "Feedback", {0.0f, 100.0f, 0.1f}, 0.0f, "%");

    filterOn_ = addBool("fon", "Filter On", false);
    filterType_ = addChoice("ftype", "Filter", {"Low 12", "Low 24", "High 12", "Band", "Notch"}, 1);
    cutoff_ = addFloat("cutoff", "Cutoff", skewedRange(20.0f, 20000.0f, 1500.0f), 6000.0f, "Hz");
    resonance_ = addFloat("res", "Resonance", {0.0f, 100.0f, 0.1f}, 10.0f, "%");
    filterEnvelope_ = addFloat("fenv", "Filter Env", {-100.0f, 100.0f, 0.1f}, 0.0f, "%");
    filterAttack_ = addFloat("fa", "Filter Attack", skewedRange(0.5f, 10000.0f, 200.0f), 2.0f, "ms");
    filterDecay_ = addFloat("fd", "Filter Decay", skewedRange(1.0f, 20000.0f, 500.0f), 500.0f, "ms");
    filterSustain_ = addFloat("fs", "Filter Sustain", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    filterRelease_ = addFloat("fr", "Filter Release", skewedRange(1.0f, 20000.0f, 500.0f), 300.0f, "ms");

    pitchEnvelope_ = addFloat("penv", "Pitch Env", {-48.0f, 48.0f, 0.1f}, 0.0f, "st");
    pitchDecay_ = addFloat("pdecay", "Pitch Decay", skewedRange(1.0f, 5000.0f, 200.0f), 100.0f, "ms");

    lfoShape_ = addChoice("lshape", "LFO Shape", {"Sine", "Triangle", "Saw", "Square", "Random"}, 0);
    lfoRate_ = addFloat("lrate", "LFO Rate", skewedRange(0.01f, 30.0f, 2.0f), 5.0f, "Hz");
    lfoDestination_ = addChoice("ldest", "LFO Target", {"Pitch", "Cutoff", "Volume", "Modulators"}, 0);
    lfoAmount_ = addFloat("lamount", "LFO Amount", {0.0f, 100.0f, 0.1f}, 0.0f, "%");

    transpose_ = addFloat("transpose", "Transpose", {-48.0f, 48.0f, 1.0f}, 0.0f, "st");
    glide_ = addFloat("glide", "Glide", skewedRange(0.0f, 2000.0f, 200.0f), 0.0f, "ms");
    volume_ = addFloat("volume", "Volume", {-36.0f, 6.0f, 0.1f}, -6.0f, "dB");
}

void Operator::prepareDsp(double, int)
{
    for (auto& voice : voices_)
    {
        voice.active = false;
        for (auto& e : voice.envelopes)
            e.reset();
        voice.filterEnvelope.reset();
        voice.filter.reset();
    }
    sustainPedal_ = false;
    pitchBend_ = 0.0;
    lastPitch_ = -1.0;
    activeVoices_.store(0);
}

double Operator::nextNoise() noexcept
{
    noiseState_ ^= noiseState_ << 13;
    noiseState_ ^= noiseState_ >> 17;
    noiseState_ ^= noiseState_ << 5;
    return static_cast<double>(noiseState_) / 2147483648.0 - 1.0;
}

void Operator::updateEnvelopes(Voice& voice) noexcept
{
    for (std::size_t o = 0; o < operators_.size(); ++o)
    {
        const auto& p = operators_[o];
        voice.envelopes[o].setParameters(p.attack->get() / 1000.0, p.decay->get() / 1000.0, p.sustain->get() / 100.0,
                                         p.release->get() / 1000.0, sampleRate_);
    }
    voice.filterEnvelope.setParameters(filterAttack_->get() / 1000.0, filterDecay_->get() / 1000.0, filterSustain_->get() / 100.0,
                                       filterRelease_->get() / 1000.0, sampleRate_);
}

void Operator::noteOn(int note, double velocity) noexcept
{
    Voice* chosen = nullptr;
    for (auto& v : voices_)
        if (v.active && v.note == note)
            chosen = &v;
    if (chosen == nullptr)
        for (auto& v : voices_)
            if (!v.active)
            {
                chosen = &v;
                break;
            }
    if (chosen == nullptr)
        for (auto& v : voices_)
            if (chosen == nullptr || v.age < chosen->age)
                chosen = &v;

    auto& voice = *chosen;
    const bool wasActive = voice.active;
    voice.active = true;
    voice.note = note;
    voice.velocity = velocity;
    voice.keyDown = true;
    voice.sustained = false;
    voice.age = ++ageCounter_;
    voice.targetPitch = note;
    voice.pitch = glide_->get() > 0.0f && lastPitch_ >= 0.0 ? lastPitch_ : static_cast<double>(note);
    voice.pitchEnvelope = 1.0;
    lastPitch_ = note;

    if (!wasActive)
    {
        voice.phases.fill(0.0);
        voice.outputs.fill(0.0);
        voice.feedbackHistory.fill(0.0);
        for (auto& e : voice.envelopes)
            e.reset();
        voice.filterEnvelope.reset();
        voice.filter.reset();
        voice.lfo.reset();
    }
    updateEnvelopes(voice);
    for (auto& e : voice.envelopes)
        e.noteOn();
    voice.filterEnvelope.noteOn();
}

void Operator::noteOff(int note) noexcept
{
    for (auto& voice : voices_)
        if (voice.active && voice.keyDown && voice.note == note)
        {
            voice.keyDown = false;
            if (sustainPedal_)
            {
                voice.sustained = true;
                continue;
            }
            for (auto& e : voice.envelopes)
                e.noteOff();
            voice.filterEnvelope.noteOff();
        }
}

void Operator::handleMidi(const juce::MidiMessage& message) noexcept
{
    if (message.isNoteOn())
    {
        noteOn(message.getNoteNumber(), message.getFloatVelocity());
    }
    else if (message.isNoteOff())
    {
        noteOff(message.getNoteNumber());
    }
    else if (message.isPitchWheel())
    {
        pitchBend_ = (message.getPitchWheelValue() - 8192) / 8192.0 * 2.0;
    }
    else if (message.isController())
    {
        const int cc = message.getControllerNumber();
        if (cc == 64)
        {
            sustainPedal_ = message.getControllerValue() >= 64;
            if (!sustainPedal_)
                for (auto& voice : voices_)
                    if (voice.active && voice.sustained)
                    {
                        voice.sustained = false;
                        for (auto& e : voice.envelopes)
                            e.noteOff();
                        voice.filterEnvelope.noteOff();
                    }
        }
        else if (cc == 120)
        {
            for (auto& voice : voices_)
                voice.active = false;
        }
        else if (cc == 123)
        {
            for (auto& voice : voices_)
                if (voice.active)
                {
                    voice.keyDown = voice.sustained = false;
                    for (auto& e : voice.envelopes)
                        e.noteOff();
                    voice.filterEnvelope.noteOff();
                }
        }
    }
}

void Operator::renderVoice(Voice& voice, double* output, int start, int end) noexcept
{
    const auto& tables = waveTables();
    const auto& algorithm = getAlgorithms()[static_cast<std::size_t>(std::clamp(algorithm_->getIndex(), 0, kAlgorithms - 1))];
    int carrierCount = 0;
    for (int o = 0; o < kOperators; ++o)
        carrierCount += (algorithm.carriers >> o) & 1;
    const double carrierScale = 1.0 / std::sqrt(static_cast<double>(std::max(1, carrierCount)));
    const double feedback = feedback_->get() / 100.0 * 0.5;
    const bool filterOn = filterOn_->get();
    const auto filterType = static_cast<dsp::MultiModeFilter::Type>(std::clamp(filterType_->getIndex(), 0, 4));
    const int lfoTarget = lfoDestination_->getIndex();
    const double lfoAmount = lfoAmount_->get() / 100.0;
    const double glideSeconds = glide_->get() / 1000.0;
    const double pitchDecay = 1.0 - std::exp(-4.6 / std::max(1.0, pitchDecay_->get() / 1000.0 * sampleRate_));

    std::array<int, kOperators> waves{};
    std::array<double, kOperators> levels{}, velocityGains{};
    for (std::size_t o = 0; o < operators_.size(); ++o)
    {
        waves[o] = operators_[o].wave->getIndex();
        levels[o] = operators_[o].level->get() / 100.0;
        velocityGains[o] = 1.0 - operators_[o].velocity->get() / 100.0 * (1.0 - voice.velocity);
    }

    for (int chunkStart = start; chunkStart < end && voice.active; chunkStart += kControlInterval)
    {
        const int n = std::min(kControlInterval, end - chunkStart);
        const double lfo = voice.lfo.advance(lfoRate_->get(), n, sampleRate_, static_cast<dsp::Lfo::Shape>(std::clamp(lfoShape_->getIndex(), 0, 4)));
        const double filterEnvelope = voice.filterEnvelope.advance(n);

        if (glideSeconds > 0.0)
            voice.pitch += (voice.targetPitch - voice.pitch) * (1.0 - std::exp(-4.6 * n / (glideSeconds * sampleRate_)));
        else
            voice.pitch = voice.targetPitch;

        const double pitch = voice.pitch + transpose_->get() + pitchBend_ + pitchEnvelope_->get() * voice.pitchEnvelope +
                             (lfoTarget == toPitch ? lfo * lfoAmount * 12.0 : 0.0);
        voice.pitchEnvelope *= std::pow(1.0 - pitchDecay, n);
        const double noteFrequency = dsp::noteToFrequency(pitch);

        std::array<double, kOperators> increments{};
        std::array<int, kOperators> bandLevels{};
        std::array<double, kOperators> gains{};
        for (std::size_t o = 0; o < operators_.size(); ++o)
        {
            const auto& p = operators_[o];
            const double frequency = p.fixed->get() ? static_cast<double>(p.frequency->get()) : noteFrequency * ratioFor(p.coarse->get(), p.fine->get());
            increments[o] = std::min(0.49, frequency / sampleRate_);
            bandLevels[o] = dsp::BandLimitedWavetable::levelFor(increments[o]);
            const bool carrier = ((algorithm.carriers >> o) & 1) != 0;
            double gain = levels[o] * velocityGains[o];
            if (!carrier)
            {
                gain *= kModulationDepth;
                if (lfoTarget == toModulators)
                    gain *= std::clamp(1.0 + lfo * lfoAmount, 0.0, 2.0);
            }
            gains[o] = gain;
        }

        if (filterOn)
        {
            const double cutoff = cutoff_->get() * std::exp2(filterEnvelope_->get() / 100.0 * 6.0 * filterEnvelope +
                                                             (lfoTarget == toCutoff ? lfo * lfoAmount * 4.0 : 0.0));
            voice.filter.setParameters(cutoff, resonance_->get() / 100.0, sampleRate_);
        }
        const double tremolo = lfoTarget == toVolume ? 1.0 - lfoAmount * 0.5 * (1.0 - lfo) : 1.0;

        for (int i = chunkStart; i < chunkStart + n; ++i)
        {
            double sum = 0.0;
            for (int o = kOperators - 1; o >= 0; --o)
            {
                const auto index = static_cast<std::size_t>(o);
                double modulation = 0.0;
                const auto mods = algorithm.modulators[index];
                for (int m = o + 1; m < kOperators; ++m)
                    if (((mods >> m) & 1) != 0)
                        modulation += voice.outputs[static_cast<std::size_t>(m)];
                if (o == kOperators - 1 && feedback > 0.0)
                    modulation += feedback * 0.5 * (voice.feedbackHistory[0] + voice.feedbackHistory[1]);

                voice.phases[index] += increments[index];
                if (voice.phases[index] >= 1.0)
                    voice.phases[index] -= 1.0;

                const double envelope = voice.envelopes[index].next();
                double s = 0.0;
                if (waves[index] == waveNoise)
                    s = nextNoise();
                else
                {
                    double phase = voice.phases[index] + modulation;
                    phase -= std::floor(phase);
                    s = tables[static_cast<std::size_t>(std::clamp(waves[index], 0, 3))].read(phase, 0.0, bandLevels[index]);
                }
                voice.outputs[index] = s * envelope * gains[index];
                if (((algorithm.carriers >> o) & 1) != 0)
                    sum += voice.outputs[index];
            }
            voice.feedbackHistory[1] = voice.feedbackHistory[0];
            voice.feedbackHistory[0] = voice.outputs[kOperators - 1];

            double y = sum * carrierScale;
            if (filterOn)
                y = voice.filter.process(y, filterType);
            output[i] += y * tremolo * 0.5;
        }

        bool sounding = false;
        for (int o = 0; o < kOperators; ++o)
            if (((algorithm.carriers >> o) & 1) != 0)
                sounding = sounding || voice.envelopes[static_cast<std::size_t>(o)].isActive();
        if (!sounding)
            voice.active = false;
    }
}

void Operator::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept
{
    const int numSamples = buffer.getNumSamples();
    buffer.clear();
    double* left = buffer.getWritePointer(0);

    for (auto& voice : voices_)
        if (voice.active)
            updateEnvelopes(voice);

    int position = 0;
    const auto renderUntil = [&](int end)
    {
        for (auto& voice : voices_)
            if (voice.active && end > position)
                renderVoice(voice, left, position, end);
        position = std::max(position, end);
    };

    for (const auto metadata : midi)
    {
        renderUntil(std::clamp(metadata.samplePosition, 0, numSamples));
        if (metadata.numBytes <= 3)
            handleMidi(juce::MidiMessage(metadata.data, metadata.numBytes));
    }
    renderUntil(numSamples);

    const double gain = juce::Decibels::decibelsToGain(static_cast<double>(volume_->get()), -100.0);
    buffer.applyGain(0, 0, numSamples, gain);
    buffer.copyFrom(1, 0, buffer, 0, 0, numSamples);

    int active = 0;
    for (const auto& voice : voices_)
        active += voice.active ? 1 : 0;
    activeVoices_.store(active, std::memory_order_relaxed);
}

} // namespace dojo::devices
