#include "devices/WavetableSynth.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices
{
namespace
{
using dsp::kPi;
using dsp::kTwoPi;

constexpr int kTableFrames = 32;

double mix(double a, double b, double t)
{
    return a + (b - a) * t;
}

double triangle(double p)
{
    return p < 0.25 ? 4.0 * p : p < 0.75 ? 2.0 - 4.0 * p : 4.0 * p - 4.0;
}

double basicShapes(double t, double p)
{
    const double sine = std::sin(kTwoPi * p);
    const double tri = triangle(p);
    const double saw = 1.0 - 2.0 * p;
    const double square = p < 0.5 ? 1.0 : -1.0;
    const double s = t * 3.0;
    if (s < 1.0)
        return mix(sine, tri, s);
    if (s < 2.0)
        return mix(tri, saw, s - 1.0);
    return mix(saw, square, std::min(1.0, s - 2.0));
}

double harmonics(double t, double p)
{
    const double count = 1.0 + t * 31.0;
    const int whole = static_cast<int>(count);
    double sum = 0.0;
    for (int k = 1; k <= whole; ++k)
        sum += std::sin(kTwoPi * k * p) / k;
    sum += (count - whole) * std::sin(kTwoPi * (whole + 1) * p) / (whole + 1);
    return sum;
}

double pulseWidth(double t, double p)
{
    return p < 0.5 - t * 0.47 ? 1.0 : -1.0;
}

double hardSync(double t, double p)
{
    const double ratio = 1.0 + t * 7.0;
    const double slave = p * ratio;
    return 1.0 - 2.0 * (slave - std::floor(slave));
}

double formant(double t, double p)
{
    // Vowels A, E, I, O, U (formants F1..F3 in Hz) on a 110 Hz voice.
    constexpr std::array<std::array<double, 3>, 5> vowels{{{800, 1150, 2900}, {400, 1700, 2600}, {350, 2000, 2800}, {450, 800, 2830}, {325, 700, 2530}}};
    const double v = t * 4.0;
    const auto index = static_cast<std::size_t>(std::min(3.0, std::floor(v)));
    const double fraction = v - static_cast<double>(index);
    std::array<double, 3> f{};
    for (std::size_t j = 0; j < 3; ++j)
        f[j] = mix(vowels[index][j], vowels[index + 1][j], fraction);

    double sum = 0.0;
    for (int k = 1; k <= 48; ++k)
    {
        const double hz = 110.0 * k;
        double amplitude = 0.0;
        for (std::size_t j = 0; j < 3; ++j)
        {
            const double distance = (hz - f[j]) / 90.0;
            amplitude += (j == 0 ? 1.0 : 0.6) / (1.0 + distance * distance);
        }
        sum += amplitude / std::sqrt(static_cast<double>(k)) * std::sin(kTwoPi * k * p);
    }
    return sum;
}

double fold(double t, double p)
{
    return std::sin((1.0 + t * 7.0) * std::sin(kTwoPi * p));
}

double organ(double t, double p)
{
    constexpr std::array<int, 6> partials{1, 2, 3, 4, 6, 8};
    constexpr std::array<std::array<double, 6>, 5> presets{{{1, 0, 0, 0, 0, 0},
                                                             {1, 1, 0, 0, 0, 0},
                                                             {1, 1, 1, 0.5, 0, 0},
                                                             {1, 0.8, 0.8, 0.8, 0.6, 0.6},
                                                             {0.6, 1, 0.5, 1, 0.5, 1}}};
    const double v = t * 4.0;
    const auto index = static_cast<std::size_t>(std::min(3.0, std::floor(v)));
    const double fraction = v - static_cast<double>(index);
    double sum = 0.0;
    for (std::size_t j = 0; j < partials.size(); ++j)
        sum += mix(presets[index][j], presets[index + 1][j], fraction) * std::sin(kTwoPi * partials[j] * p);
    return sum;
}

double digital(double t, double p)
{
    const double steps = std::exp2(6.0 - t * 5.0); // 64 .. 2 levels
    const double value = std::sin(kTwoPi * p) + 0.3 * t * std::sin(kTwoPi * 5.0 * p);
    return std::round(value * steps) / steps;
}

double lerpTowards(double value, double target, double coefficient)
{
    return value + (target - value) * coefficient;
}
} // namespace

const std::vector<dsp::BandLimitedWavetable>& WavetableSynth::getTables()
{
    static const std::vector<dsp::BandLimitedWavetable> tables = []
    {
        std::vector<dsp::BandLimitedWavetable> result;
        result.reserve(8);
        result.emplace_back("Basic Shapes", kTableFrames, basicShapes);
        result.emplace_back("Harmonics", kTableFrames, harmonics);
        result.emplace_back("Pulse Width", kTableFrames, pulseWidth);
        result.emplace_back("Sync", kTableFrames, hardSync);
        result.emplace_back("Formant", kTableFrames, formant);
        result.emplace_back("Fold", kTableFrames, fold);
        result.emplace_back("Organ", kTableFrames, organ);
        result.emplace_back("Digital", kTableFrames, digital);
        return result;
    }();
    return tables;
}

juce::StringArray WavetableSynth::getTableNames()
{
    return {"Basic Shapes", "Harmonics", "Pulse Width", "Sync", "Formant", "Fold", "Organ", "Digital"};
}

const juce::StringArray& WavetableSynth::getSourceNames()
{
    static const juce::StringArray names{"Off", "Env 2", "Env 3", "LFO 1", "LFO 2", "Velocity", "Mod Wheel", "Note", "Pressure"};
    return names;
}

const juce::StringArray& WavetableSynth::getDestinationNames()
{
    static const juce::StringArray names{"Off",       "Osc 1 Pos",   "Osc 2 Pos",   "Osc 1 Pitch", "Osc 2 Pitch", "Pitch",
                                         "Cutoff",    "Resonance",   "Volume",      "Osc 1 Level", "Osc 2 Level", "Pan",
                                         "LFO 1 Rate", "LFO 2 Rate", "Sub Level",   "Noise Level"};
    return names;
}

WavetableSynth::WavetableSynth()
    : BuiltinProcessor(kTypeId, "Wavetable", BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true), true)
{
    (void) getTables(); // build the shared tables now, not on the audio thread
    const auto tableNames = getTableNames();

    for (std::size_t o = 0; o < oscillators_.size(); ++o)
    {
        const juce::String id = "o" + juce::String(o + 1);
        const juce::String name = "Osc " + juce::String(o + 1) + " ";
        auto& osc = oscillators_[o];
        osc.on = addBool(id + "on", name + "On", true);
        osc.table = addChoice(id + "table", name + "Table", tableNames, o == 0 ? 0 : 1);
        osc.position = addFloat(id + "pos", name + "Position", {0.0f, 100.0f, 0.1f}, o == 0 ? 30.0f : 10.0f, "%");
        osc.octave = addFloat(id + "oct", name + "Octave", {-3.0f, 3.0f, 1.0f}, 0.0f);
        osc.semi = addFloat(id + "semi", name + "Semi", {-12.0f, 12.0f, 1.0f}, 0.0f, "st");
        osc.fine = addFloat(id + "fine", name + "Fine", {-100.0f, 100.0f, 0.1f}, o == 0 ? 0.0f : 7.0f, "ct");
        osc.level = addFloat(id + "level", name + "Level", {0.0f, 100.0f, 0.1f}, o == 0 ? 80.0f : 50.0f, "%");
    }
    sub_ = addFloat("sub", "Sub", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    noise_ = addFloat("noise", "Noise", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    unison_ = addChoice("unison", "Unison", {"1", "2", "3", "4", "6", "8"}, 0);
    unisonDetune_ = addFloat("unidetune", "Detune", {0.0f, 100.0f, 0.1f}, 25.0f, "%");
    unisonSpread_ = addFloat("unispread", "Spread", {0.0f, 100.0f, 0.1f}, 60.0f, "%");

    filterType_ = addChoice("ftype", "Filter", {"Low 12", "Low 24", "High 12", "Band", "Notch"}, 1);
    cutoff_ = addFloat("cutoff", "Cutoff", skewedRange(20.0f, 20000.0f, 1500.0f), 2500.0f, "Hz");
    resonance_ = addFloat("res", "Resonance", {0.0f, 100.0f, 0.1f}, 15.0f, "%");
    drive_ = addFloat("drive", "Drive", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    keyTrack_ = addFloat("keytrack", "Key Track", {0.0f, 100.0f, 0.1f}, 50.0f, "%");

    constexpr std::array<std::array<float, 4>, 3> envelopeDefaults{{{2.0f, 300.0f, 80.0f, 250.0f},
                                                                    {2.0f, 400.0f, 0.0f, 300.0f},
                                                                    {100.0f, 800.0f, 50.0f, 500.0f}}};
    for (std::size_t e = 0; e < envelopes_.size(); ++e)
    {
        const juce::String id = juce::String(e + 1);
        const juce::String name = "Env " + juce::String(e + 1) + " ";
        auto& env = envelopes_[e];
        env.attack = addFloat("a" + id, name + "Attack", skewedRange(0.5f, 10000.0f, 200.0f), envelopeDefaults[e][0], "ms");
        env.decay = addFloat("d" + id, name + "Decay", skewedRange(1.0f, 20000.0f, 500.0f), envelopeDefaults[e][1], "ms");
        env.sustain = addFloat("s" + id, name + "Sustain", {0.0f, 100.0f, 0.1f}, envelopeDefaults[e][2], "%");
        env.release = addFloat("r" + id, name + "Release", skewedRange(1.0f, 20000.0f, 500.0f), envelopeDefaults[e][3], "ms");
    }

    const juce::StringArray shapes{"Sine", "Triangle", "Saw", "Square", "Random"};
    for (std::size_t l = 0; l < lfos_.size(); ++l)
    {
        const juce::String id = "l" + juce::String(l + 1);
        const juce::String name = "LFO " + juce::String(l + 1) + " ";
        lfos_[l].shape = addChoice(id + "shape", name + "Shape", shapes, 0);
        lfos_[l].rate = addFloat(id + "rate", name + "Rate", skewedRange(0.01f, 30.0f, 2.0f), l == 0 ? 2.0f : 0.3f, "Hz");
    }

    struct SlotDefault
    {
        int source, destination;
        float amount;
    };
    constexpr std::array<SlotDefault, kMatrixSlots> slotDefaults{{{srcEnv2, dstCutoff, 40.0f},
                                                                   {srcLfo1, dstOsc1Position, 20.0f},
                                                                   {srcModWheel, dstCutoff, 30.0f},
                                                                   {srcOff, dstOff, 0.0f},
                                                                   {srcOff, dstOff, 0.0f},
                                                                   {srcOff, dstOff, 0.0f},
                                                                   {srcOff, dstOff, 0.0f},
                                                                   {srcOff, dstOff, 0.0f}}};
    for (std::size_t m = 0; m < matrix_.size(); ++m)
    {
        const juce::String id = "m" + juce::String(m + 1);
        const juce::String name = "Mod " + juce::String(m + 1) + " ";
        matrix_[m].source = addChoice(id + "src", name + "Source", getSourceNames(), slotDefaults[m].source);
        matrix_[m].destination = addChoice(id + "dst", name + "Target", getDestinationNames(), slotDefaults[m].destination);
        matrix_[m].amount = addFloat(id + "amt", name + "Amount", {-100.0f, 100.0f, 0.1f}, slotDefaults[m].amount, "%");
    }

    velocityToAmp_ = addFloat("velamp", "Velocity", {0.0f, 100.0f, 0.1f}, 60.0f, "%");
    glide_ = addFloat("glide", "Glide", skewedRange(0.0f, 2000.0f, 200.0f), 0.0f, "ms");
    volume_ = addFloat("volume", "Volume", {-36.0f, 6.0f, 0.1f}, -6.0f, "dB");
}

void WavetableSynth::prepareDsp(double, int)
{
    for (auto& voice : voices_)
    {
        voice.active = false;
        for (auto& e : voice.envelopes)
            e.reset();
        for (auto& f : voice.filters)
            f.reset();
    }
    sustainPedal_ = false;
    pitchBend_ = 0.0;
    lastPitch_ = -1.0;
    activeVoices_.store(0);
}

double WavetableSynth::nextNoise() noexcept
{
    noiseState_ ^= noiseState_ << 13;
    noiseState_ ^= noiseState_ >> 17;
    noiseState_ ^= noiseState_ << 5;
    return static_cast<double>(noiseState_) / 2147483648.0 - 1.0;
}

void WavetableSynth::noteOn(int note, double velocity) noexcept
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
    {
        // Steal: prefer the oldest released voice, else the oldest.
        for (auto& v : voices_)
            if (v.envelopes[0].isReleasing() && (chosen == nullptr || v.age < chosen->age))
                chosen = &v;
        if (chosen == nullptr)
            for (auto& v : voices_)
                if (chosen == nullptr || v.age < chosen->age)
                    chosen = &v;
    }

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
    lastPitch_ = note;

    if (!wasActive)
    {
        for (auto& f : voice.filters)
            f.reset();
        for (auto& e : voice.envelopes)
            e.reset();
        for (auto& lfo : voice.lfos)
            lfo.reset();
        voice.subPhase = 0.0;
        for (auto& oscillator : voice.phases)
            for (std::size_t u = 0; u < oscillator.size(); ++u)
                oscillator[u] = u == 0 ? 0.0 : 0.5 * (nextNoise() + 1.0);
    }
    for (auto& e : voice.envelopes)
        e.noteOn();
}

void WavetableSynth::noteOff(int note) noexcept
{
    for (auto& voice : voices_)
        if (voice.active && voice.keyDown && voice.note == note)
        {
            voice.keyDown = false;
            if (sustainPedal_)
                voice.sustained = true;
            else
                for (auto& e : voice.envelopes)
                    e.noteOff();
        }
}

void WavetableSynth::handleMidi(const juce::MidiMessage& message) noexcept
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
    else if (message.isChannelPressure())
    {
        pressure_ = message.getChannelPressureValue() / 127.0;
    }
    else if (message.isAftertouch())
    {
        pressure_ = message.getAfterTouchValue() / 127.0;
    }
    else if (message.isController())
    {
        const int cc = message.getControllerNumber();
        const int value = message.getControllerValue();
        if (cc == 1)
        {
            modWheel_ = value / 127.0;
        }
        else if (cc == 64)
        {
            sustainPedal_ = value >= 64;
            if (!sustainPedal_)
                for (auto& voice : voices_)
                    if (voice.active && voice.sustained)
                    {
                        voice.sustained = false;
                        for (auto& e : voice.envelopes)
                            e.noteOff();
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
                }
        }
    }
}

void WavetableSynth::renderVoice(Voice& voice, double* left, double* right, int start, int end) noexcept
{
    const auto& tables = getTables();
    const int unisonCounts[] = {1, 2, 3, 4, 6, 8};
    const int unison = unisonCounts[std::clamp(unison_->getIndex(), 0, 5)];
    const double detune = unisonDetune_->get() / 100.0 * 0.5; // semitones each side
    const double spread = unisonSpread_->get() / 100.0;
    const auto filterType = static_cast<dsp::MultiModeFilter::Type>(std::clamp(filterType_->getIndex(), 0, 4));
    const double drive = drive_->get() / 100.0;
    const double driveGain = 1.0 + drive * 6.0;
    const double velocityGain = 1.0 - velocityToAmp_->get() / 100.0 * (1.0 - voice.velocity);
    const double glideSeconds = glide_->get() / 1000.0;
    const double unisonScale = 1.0 / std::sqrt(static_cast<double>(unison));

    for (int chunkStart = start; chunkStart < end && voice.active; chunkStart += kControlInterval)
    {
        const int n = std::min(kControlInterval, end - chunkStart);

        // --- Modulation sources and the matrix ---------------------------------------------------
        std::array<double, numSources> sources{};
        sources[srcEnv2] = voice.envelopes[1].advance(n);
        sources[srcEnv3] = voice.envelopes[2].advance(n);
        sources[srcVelocity] = voice.velocity;
        sources[srcModWheel] = modWheel_;
        sources[srcNote] = (voice.note - 60) / 60.0;
        sources[srcPressure] = pressure_;

        // LFO rates may be modulated: use last chunk's values for the matrix's LFO terms.
        std::array<double, numDestinations> mod{};
        const auto accumulate = [&]
        {
            mod.fill(0.0);
            for (const auto& slot : matrix_)
            {
                const int source = slot.source->getIndex();
                const int destination = slot.destination->getIndex();
                if (source > srcOff && source < numSources && destination > dstOff && destination < numDestinations)
                    mod[static_cast<std::size_t>(destination)] += slot.amount->get() / 100.0 * sources[static_cast<std::size_t>(source)];
            }
        };
        accumulate();
        for (std::size_t l = 0; l < 2; ++l)
        {
            const double rate = lfos_[l].rate->get() * std::exp2(mod[l == 0 ? dstLfo1Rate : dstLfo2Rate] * 4.0);
            sources[l == 0 ? srcLfo1 : srcLfo2] =
                voice.lfos[l].advance(rate, n, sampleRate_, static_cast<dsp::Lfo::Shape>(std::clamp(lfos_[l].shape->getIndex(), 0, 4)));
        }
        accumulate();

        // --- Pitch, oscillators, filter ----------------------------------------------------------
        if (glideSeconds > 0.0)
            voice.pitch = lerpTowards(voice.pitch, voice.targetPitch, 1.0 - std::exp(-4.6 * n / (glideSeconds * sampleRate_)));
        else
            voice.pitch = voice.targetPitch;

        struct OscillatorState
        {
            bool on;
            const dsp::BandLimitedWavetable* table;
            double position, increment, level;
            int bandLevel;
        };
        std::array<OscillatorState, 2> osc{};
        for (std::size_t o = 0; o < 2; ++o)
        {
            const auto& p = oscillators_[o];
            const double pitch = voice.pitch + p.octave->get() * 12.0 + p.semi->get() + p.fine->get() / 100.0 + pitchBend_ +
                                 mod[dstPitch] * 24.0 + mod[o == 0 ? dstOsc1Pitch : dstOsc2Pitch] * 24.0;
            osc[o].on = p.on->get();
            osc[o].table = &tables[static_cast<std::size_t>(std::clamp(p.table->getIndex(), 0, static_cast<int>(tables.size()) - 1))];
            osc[o].position = std::clamp(p.position->get() / 100.0 + mod[o == 0 ? dstOsc1Position : dstOsc2Position], 0.0, 1.0);
            osc[o].increment = std::min(0.45, dsp::noteToFrequency(pitch) / sampleRate_);
            osc[o].level = p.level->get() / 100.0 * std::clamp(1.0 + mod[o == 0 ? dstOsc1Level : dstOsc2Level], 0.0, 2.0) * unisonScale;
            osc[o].bandLevel = dsp::BandLimitedWavetable::levelFor(osc[o].increment * std::exp2(detune / 12.0));
            displayPosition_[o].store(static_cast<float>(osc[o].position), std::memory_order_relaxed);
        }

        std::array<double, kMaxUnison> detuneRatio{}, gainLeft{}, gainRight{};
        for (int u = 0; u < unison; ++u)
        {
            const double offset = unison == 1 ? 0.0 : static_cast<double>(u) / (unison - 1) * 2.0 - 1.0;
            detuneRatio[static_cast<std::size_t>(u)] = std::exp2(offset * detune / 12.0);
            const double pan = offset * spread; // -1..1
            gainLeft[static_cast<std::size_t>(u)] = std::cos((pan + 1.0) * kPi / 4.0) * std::sqrt(2.0);
            gainRight[static_cast<std::size_t>(u)] = std::sin((pan + 1.0) * kPi / 4.0) * std::sqrt(2.0);
        }

        const double cutoff = cutoff_->get() * std::exp2(mod[dstCutoff] * 5.0 + keyTrack_->get() / 100.0 * (voice.pitch - 60.0) / 12.0);
        const double resonance = std::clamp(resonance_->get() / 100.0 + mod[dstResonance], 0.0, 1.0);
        for (auto& f : voice.filters)
            f.setParameters(cutoff, resonance, sampleRate_);

        const double volume = std::clamp(1.0 + mod[dstVolume], 0.0, 2.0) * velocityGain;
        const double pan = std::clamp(mod[dstPan], -1.0, 1.0);
        const double panLeft = std::min(1.0, 1.0 - pan);
        const double panRight = std::min(1.0, 1.0 + pan);
        const double subLevel = std::clamp(sub_->get() / 100.0 + mod[dstSubLevel], 0.0, 1.0);
        const double noiseLevel = std::clamp(noise_->get() / 100.0 + mod[dstNoiseLevel], 0.0, 1.0);
        const double subIncrement = osc[0].increment * 0.5;

        for (int i = chunkStart; i < chunkStart + n; ++i)
        {
            double l = 0.0, r = 0.0;
            for (std::size_t o = 0; o < 2; ++o)
            {
                if (!osc[o].on || osc[o].level <= 0.0)
                    continue;
                auto& phases = voice.phases[o];
                double sumLeft = 0.0, sumRight = 0.0;
                for (std::size_t u = 0; u < static_cast<std::size_t>(unison); ++u)
                {
                    const double s = osc[o].table->read(phases[u], osc[o].position, osc[o].bandLevel);
                    phases[u] += osc[o].increment * detuneRatio[u];
                    if (phases[u] >= 1.0)
                        phases[u] -= 1.0;
                    sumLeft += s * gainLeft[u];
                    sumRight += s * gainRight[u];
                }
                l += sumLeft * osc[o].level;
                r += sumRight * osc[o].level;
            }

            if (subLevel > 0.0)
            {
                const double s = std::sin(kTwoPi * voice.subPhase) * subLevel;
                l += s;
                r += s;
            }
            voice.subPhase += subIncrement;
            if (voice.subPhase >= 1.0)
                voice.subPhase -= 1.0;

            if (noiseLevel > 0.0)
            {
                l += nextNoise() * noiseLevel * 0.5;
                r += nextNoise() * noiseLevel * 0.5;
            }

            if (drive > 0.0)
            {
                l = std::tanh(l * driveGain) / std::sqrt(driveGain);
                r = std::tanh(r * driveGain) / std::sqrt(driveGain);
            }

            l = voice.filters[0].process(l, filterType);
            r = voice.filters[1].process(r, filterType);

            const double amp = voice.envelopes[0].next() * volume * 0.5;
            left[i] += l * amp * panLeft;
            right[i] += r * amp * panRight;
        }

        if (!voice.envelopes[0].isActive())
            voice.active = false;
    }
}

void WavetableSynth::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept
{
    const int numSamples = buffer.getNumSamples();
    buffer.clear();
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);

    // Envelope settings follow the parameters (automation included).
    for (auto& voice : voices_)
        if (voice.active)
            for (std::size_t e = 0; e < envelopes_.size(); ++e)
                voice.envelopes[e].setParameters(envelopes_[e].attack->get() / 1000.0, envelopes_[e].decay->get() / 1000.0,
                                                 envelopes_[e].sustain->get() / 100.0, envelopes_[e].release->get() / 1000.0,
                                                 sampleRate_);

    int position = 0;
    const auto renderUntil = [&](int end)
    {
        for (auto& voice : voices_)
            if (voice.active && end > position)
                renderVoice(voice, left, right, position, end);
        position = std::max(position, end);
    };

    for (const auto metadata : midi)
    {
        renderUntil(std::clamp(metadata.samplePosition, 0, numSamples));
        if (metadata.numBytes <= 3)
        {
            const auto message = juce::MidiMessage(metadata.data, metadata.numBytes);
            const bool isNoteOn = message.isNoteOn();
            handleMidi(message);
            if (isNoteOn)
                for (auto& voice : voices_)
                    if (voice.active && voice.note == message.getNoteNumber())
                        for (std::size_t e = 0; e < envelopes_.size(); ++e)
                            voice.envelopes[e].setParameters(envelopes_[e].attack->get() / 1000.0, envelopes_[e].decay->get() / 1000.0,
                                                             envelopes_[e].sustain->get() / 100.0,
                                                             envelopes_[e].release->get() / 1000.0, sampleRate_);
        }
    }
    renderUntil(numSamples);

    const double gain = juce::Decibels::decibelsToGain(static_cast<double>(volume_->get()), -100.0);
    buffer.applyGain(0, 0, numSamples, gain);
    buffer.applyGain(1, 0, numSamples, gain);

    int active = 0;
    for (const auto& voice : voices_)
        active += voice.active ? 1 : 0;
    activeVoices_.store(active, std::memory_order_relaxed);
}

} // namespace dojo::devices
