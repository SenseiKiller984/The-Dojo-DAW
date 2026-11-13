#include "devices/Meld.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices
{
namespace
{
constexpr std::array<int, 4> kPolyphony{1, 4, 8, 16};

double fraction(double x) noexcept
{
    return x - std::floor(x);
}

double mix(double a, double b, double t) noexcept
{
    return a + (b - a) * t;
}

struct FilterChoice
{
    dsp::FilterMode mode;
    bool steep;
};

/** LP 12, LP 24, HP 12, Band Pass, Notch. */
FilterChoice filterChoice(int type) noexcept
{
    switch (type)
    {
    case 1: return {dsp::FilterMode::lowPass, true};
    case 2: return {dsp::FilterMode::highPass, false};
    case 3: return {dsp::FilterMode::bandPass, false};
    case 4: return {dsp::FilterMode::notch, false};
    default: return {dsp::FilterMode::lowPass, false};
    }
}
} // namespace

const juce::StringArray& Meld::getOscillatorNames()
{
    static const juce::StringArray names{"Basic Shapes", "Swarm",     "Harmonic FM", "Inharmonic FM", "Fold FM", "Squelch", "Pluck",
                                         "Bitgrunge",    "Noise Loop", "Shepard's Tone", "Chip",       "Additive", "Rain"};
    return names;
}

std::pair<juce::String, juce::String> Meld::getMacroNames(Oscillator oscillator)
{
    switch (oscillator)
    {
    case Oscillator::basicShapes: return {"Shape", "Sub"};
    case Oscillator::swarm: return {"Detune", "Spread"};
    case Oscillator::harmonicFm: return {"Ratio", "Index"};
    case Oscillator::inharmonicFm: return {"Ratio", "Index"};
    case Oscillator::foldFm: return {"Feedback", "Fold"};
    case Oscillator::squelch: return {"Squelch", "Resonance"};
    case Oscillator::pluck: return {"Brightness", "Decay"};
    case Oscillator::bitgrunge: return {"Bits", "Crunch"};
    case Oscillator::noiseLoop: return {"Focus", "Octave"};
    case Oscillator::shepard: return {"Rate", "Spread"};
    case Oscillator::chip: return {"Duty", "Crush"};
    case Oscillator::additive: return {"Brightness", "Odd/Even"};
    case Oscillator::rain: return {"Density", "Scatter"};
    }
    return {"Macro 1", "Macro 2"};
}

const juce::StringArray& Meld::getSourceNames()
{
    static const juce::StringArray names{"Off",      "Env 2 A",  "Env 2 B", "LFO 1 A", "LFO 2 A",   "LFO 1 B",  "LFO 2 B",
                                         "Velocity", "Note",     "Pressure", "Slide",  "Mod Wheel", "Random",   "Pitch Bend"};
    return names;
}

const juce::StringArray& Meld::getDestinationNames()
{
    static const juce::StringArray names = []
    {
        juce::StringArray result{"Off"};
        for (const auto* engine : {"A ", "B "})
            for (const auto* target : {"Macro 1", "Macro 2", "Pitch", "Cutoff", "Resonance", "Level", "Pan", "LFO Rate"})
                result.add(juce::String(engine) + target);
        return result;
    }();
    return names;
}

Meld::Meld()
    : BuiltinProcessor(kTypeId, "Meld", BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true), true)
{
    (void) getDestinationNames();
    struct EngineDefaults
    {
        int oscillator;
        float m1, m2, fine, level, pan, cutoff, lfo1, lfo2;
    };
    constexpr std::array<EngineDefaults, 2> defaults{{{1, 40.0f, 60.0f, 0.0f, 80.0f, -20.0f, 4000.0f, 0.5f, 4.0f},
                                                      {11, 50.0f, 30.0f, 5.0f, 60.0f, 20.0f, 3000.0f, 0.3f, 3.0f}}};
    const std::array<const char*, 4> stage{"Attack", "Decay", "Sustain", "Release"};
    const std::array<const char*, 4> suffix{"a", "d", "s", "r"};
    constexpr std::array<float, 4> ampDefaults{10.0f, 800.0f, 70.0f, 800.0f};
    constexpr std::array<float, 4> modDefaults{500.0f, 1500.0f, 30.0f, 1000.0f};

    for (std::size_t e = 0; e < engines_.size(); ++e)
    {
        const juce::String id = e == 0 ? "a" : "b";
        const juce::String name = e == 0 ? "A " : "B ";
        const auto& d = defaults[e];
        auto& engine = engines_[e];
        engine.on = addBool(id + "on", name + "On", true);
        engine.oscillator = addChoice(id + "osc", name + "Oscillator", getOscillatorNames(), d.oscillator);
        engine.macro1 = addFloat(id + "m1", name + "Macro 1", {0.0f, 100.0f, 0.1f}, d.m1, "%");
        engine.macro2 = addFloat(id + "m2", name + "Macro 2", {0.0f, 100.0f, 0.1f}, d.m2, "%");
        engine.octave = addFloat(id + "oct", name + "Octave", {-3.0f, 3.0f, 1.0f}, 0.0f);
        engine.semi = addFloat(id + "semi", name + "Semi", {-24.0f, 24.0f, 1.0f}, 0.0f, "st");
        engine.fine = addFloat(id + "fine", name + "Fine", {-100.0f, 100.0f, 0.1f}, d.fine, "ct");
        engine.level = addFloat(id + "level", name + "Level", {0.0f, 100.0f, 0.1f}, d.level, "%");
        engine.pan = addFloat(id + "pan", name + "Pan", {-100.0f, 100.0f, 0.1f}, d.pan, "%");
        engine.filterType = addChoice(id + "ftype", name + "Filter", {"LP 12", "LP 24", "HP 12", "Band Pass", "Notch"}, 1);
        engine.circuit = addChoice(id + "circuit", name + "Circuit", {"Clean", "Warm", "Ladder", "Diode"}, 1);
        engine.cutoff = addFloat(id + "cutoff", name + "Cutoff", skewedRange(20.0f, 20000.0f, 1500.0f), d.cutoff, "Hz");
        engine.resonance = addFloat(id + "res", name + "Resonance", {0.0f, 100.0f, 0.1f}, 15.0f, "%");
        engine.keyTrack = addFloat(id + "key", name + "Key Track", {0.0f, 100.0f, 0.1f}, 50.0f, "%");
        for (std::size_t k = 0; k < 4; ++k)
        {
            const bool level = k == 2;
            const auto range = level ? juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f)
                                     : skewedRange(k == 0 ? 0.5f : 1.0f, k == 0 ? 20000.0f : 30000.0f, k == 0 ? 300.0f : 800.0f);
            engine.amp[k] = addFloat(id + "a" + suffix[k], name + "Amp " + stage[k], range, ampDefaults[k], level ? "%" : "ms");
            engine.mod[k] = addFloat(id + "m" + suffix[k], name + "Mod " + stage[k], range, modDefaults[k], level ? "%" : "ms");
        }
        for (std::size_t l = 0; l < 2; ++l)
        {
            const juce::String lid = id + "l" + juce::String(l + 1);
            const juce::String lname = name + "LFO " + juce::String(l + 1) + " ";
            engine.lfoShape[l] = addChoice(lid + "shape", lname + "Shape", dsp::waveformNames(), l == 0 ? 0 : 1);
            engine.lfoRate[l] = addFloat(lid + "rate", lname + "Rate", skewedRange(0.01f, 40.0f, 2.0f), l == 0 ? d.lfo1 : d.lfo2, "Hz");
        }
        engine.lfoSync = addBool(id + "lsync", name + "LFO 1 Sync", false);
        engine.lfoDivision = addChoice(id + "ldiv", name + "LFO 1 Division", dsp::syncDivisionNames(), 14);
    }

    mode_ = addChoice("mode", "Mode", {"Layer", "Split"}, 0);
    splitNote_ = addFloat("split", "Split Note", {0.0f, 127.0f, 1.0f}, 60.0f);

    struct SlotDefault
    {
        int source, destination;
        float amount;
    };
    const std::array<SlotDefault, kMatrixSlots> slotDefaults{{{srcLfo1A, destination(0, edMacro1), 20.0f},
                                                              {srcEnv2A, destination(0, edCutoff), 30.0f},
                                                              {srcEnv2B, destination(1, edCutoff), 30.0f},
                                                              {srcPressure, destination(0, edMacro2), 30.0f},
                                                              {srcSlide, destination(1, edMacro1), 40.0f},
                                                              {srcLfo1B, destination(1, edPan), 25.0f},
                                                              {srcOff, 0, 0.0f},
                                                              {srcOff, 0, 0.0f},
                                                              {srcOff, 0, 0.0f},
                                                              {srcOff, 0, 0.0f},
                                                              {srcOff, 0, 0.0f},
                                                              {srcOff, 0, 0.0f}}};
    for (std::size_t m = 0; m < matrix_.size(); ++m)
    {
        const juce::String id = "m" + juce::String(m + 1);
        const juce::String name = "Mod " + juce::String(m + 1) + " ";
        matrix_[m].source = addChoice(id + "src", name + "Source", getSourceNames(), slotDefaults[m].source);
        matrix_[m].destination = addChoice(id + "dst", name + "Target", getDestinationNames(), slotDefaults[m].destination);
        matrix_[m].amount = addFloat(id + "amt", name + "Amount", {-100.0f, 100.0f, 0.1f}, slotDefaults[m].amount, "%");
    }

    polyphony_ = addChoice("voices", "Voices", {"Mono", "4", "8", "16"}, 2);
    glide_ = addFloat("glide", "Glide", skewedRange(0.0f, 2000.0f, 200.0f), 0.0f, "ms");
    mpe_ = addBool("mpe", "MPE", true);
    noteBendRange_ = addFloat("notebend", "Note Bend Range", {0.0f, 96.0f, 1.0f}, 48.0f, "st");
    bendRange_ = addFloat("bend", "Bend Range", {0.0f, 24.0f, 1.0f}, 2.0f, "st");
    velocityToAmp_ = addFloat("velamp", "Velocity > Amp", {0.0f, 100.0f, 0.1f}, 50.0f, "%");
    volume_ = addFloat("volume", "Volume", {-36.0f, 6.0f, 0.1f}, -8.0f, "dB");
}

void Meld::prepareDsp(double sampleRate, int)
{
    pluckCapacity_ = static_cast<int>(sampleRate / 15.0) + 8; // down to 15 Hz
    pluckPool_.assign(static_cast<std::size_t>(kVoices) * 2 * static_cast<std::size_t>(pluckCapacity_), 0.0);
    for (std::size_t v = 0; v < voices_.size(); ++v)
    {
        auto& voice = voices_[v];
        voice.active = false;
        for (std::size_t e = 0; e < 2; ++e)
        {
            voice.engines[e].pluck = pluckPool_.data() + (v * 2 + e) * static_cast<std::size_t>(pluckCapacity_);
            voice.engines[e].pluckWrite = 0;
            voice.engines[e].filter.reset();
        }
    }
    expressions_.reset();
    sustainPedal_ = false;
    lastPitch_ = -1.0;
    activeVoices_.store(0);
}

void Meld::updateEnvelopes(Voice& voice) noexcept
{
    for (std::size_t e = 0; e < 2; ++e)
    {
        const auto& p = engines_[e];
        voice.engines[e].amp.setParameters(p.amp[0]->get() / 1000.0, p.amp[1]->get() / 1000.0, p.amp[2]->get() / 100.0, p.amp[3]->get() / 1000.0, sampleRate_);
        voice.engines[e].mod.setParameters(p.mod[0]->get() / 1000.0, p.mod[1]->get() / 1000.0, p.mod[2]->get() / 100.0, p.mod[3]->get() / 1000.0, sampleRate_);
    }
}

void Meld::startEngine(EngineVoice& engine, int index, double frequency) noexcept
{
    const bool wasOn = engine.on && engine.amp.isActive();
    engine.on = true;
    if (!wasOn)
    {
        engine.filter.reset();
        for (auto& f : engine.inner)
            f.reset();
        engine.amp.reset();
        engine.mod.reset();
        for (std::size_t k = 0; k < engine.phase.size(); ++k)
            engine.phase[k] = k == 0 ? 0.0 : 0.5 * (random_.next() + 1.0);
        engine.aux = engine.aux2 = 0.0;
        engine.shepard = 0.0;
        for (auto& g : engine.grains)
            g.active = false;
        engine.lfoPhase = {0.0, 0.0};
    }
    // Pluck: excite the string with a burst of noise, as bright as Macro 1.
    if (static_cast<Oscillator>(engines_[static_cast<std::size_t>(index)].oscillator->getIndex()) == Oscillator::pluck && engine.pluck != nullptr)
    {
        const double brightness = 0.15 + 0.85 * engines_[static_cast<std::size_t>(index)].macro1->get() / 100.0;
        const int length = std::clamp(static_cast<int>(sampleRate_ / std::max(1.0, frequency)) + 2, 4, pluckCapacity_);
        double state = 0.0;
        for (int i = 0; i < pluckCapacity_; ++i)
        {
            state += (engine.noise.next() - state) * brightness;
            engine.pluck[i] = i < length ? state : 0.0;
        }
        engine.pluckWrite = length % pluckCapacity_;
        engine.pluckState = 0.0;
    }
}

void Meld::startNote(int note, int channel, double velocity) noexcept
{
    expressions_.noteOn(note);
    const int polyphony = kPolyphony[static_cast<std::size_t>(std::clamp(polyphony_->getIndex(), 0, 3))];
    auto& v = dsp::pickVoice(voices_, polyphony, note, channel,
                             [](const Voice& voice) { return voice.engines[0].amp.isReleasing() || voice.engines[1].amp.isReleasing(); });
    const bool wasActive = v.active;
    v.active = true;
    v.note = note;
    v.channel = channel;
    v.velocity = velocity;
    v.random = random_.next();
    v.keyDown = true;
    v.sustained = false;
    v.age = ++ageCounter_;
    v.targetPitch = note;
    v.pitch = glide_->get() > 0.0f && lastPitch_ >= 0.0 && (polyphony == 1 || wasActive) ? lastPitch_ : static_cast<double>(note);
    lastPitch_ = note;

    // Layer: both engines; Split: A below the split note, B from it up.
    const bool split = mode_->getIndex() == 1;
    const auto splitNote = static_cast<int>(splitNote_->get());
    for (std::size_t e = 0; e < 2; ++e)
    {
        auto& engine = v.engines[e];
        const bool wanted = engines_[e].on->get() && (!split || (e == 0 ? note < splitNote : note >= splitNote));
        if (!wanted)
        {
            engine.on = false;
            continue;
        }
        startEngine(engine, static_cast<int>(e), dsp::noteToFrequency(note));
    }
    updateEnvelopes(v);
    for (auto& engine : v.engines)
        if (engine.on)
        {
            engine.amp.noteOn();
            engine.mod.noteOn();
        }
    if (!v.engines[0].on && !v.engines[1].on)
        v.active = false;
}

void Meld::releaseNote(int note, int channel) noexcept
{
    for (auto& v : voices_)
        if (v.active && v.keyDown && v.note == note && v.channel == channel)
        {
            v.keyDown = false;
            if (sustainPedal_)
            {
                v.sustained = true;
                continue;
            }
            for (auto& engine : v.engines)
            {
                engine.amp.noteOff();
                engine.mod.noteOff();
            }
        }
}

void Meld::handleMidi(const juce::MidiMessage& message) noexcept
{
    if (message.isNoteOn())
        startNote(message.getNoteNumber(), message.getChannel(), message.getFloatVelocity());
    else if (message.isNoteOff())
        releaseNote(message.getNoteNumber(), message.getChannel());
    else if (expressions_.handle(message))
        return;
    else if (message.isSustainPedalOn())
        sustainPedal_ = true;
    else if (message.isSustainPedalOff())
    {
        sustainPedal_ = false;
        for (auto& v : voices_)
            if (v.active && v.sustained)
            {
                v.sustained = false;
                for (auto& engine : v.engines)
                {
                    engine.amp.noteOff();
                    engine.mod.noteOff();
                }
            }
    }
    else if (message.isAllSoundOff())
    {
        for (auto& v : voices_)
            v.active = false;
    }
    else if (message.isAllNotesOff())
    {
        for (auto& v : voices_)
            if (v.active)
            {
                v.keyDown = v.sustained = false;
                for (auto& engine : v.engines)
                {
                    engine.amp.noteOff();
                    engine.mod.noteOff();
                }
            }
    }
}

double Meld::oscillate(EngineVoice& ev, const EngineState& st) noexcept
{
    const double dt = std::clamp(st.dt, 0.0, 0.45);
    const double m1 = st.m1, m2 = st.m2;
    auto& ph = ev.phase;
    switch (st.oscillator)
    {
    case Oscillator::basicShapes:
    {
        const double p = ph[0];
        const double sine = std::sin(dsp::kTwoPi * p);
        const double triangle = p < 0.5 ? 4.0 * p - 1.0 : 3.0 - 4.0 * p;
        const double saw = 2.0 * p - 1.0 - dsp::polyBlep(p, dt);
        const double square = (p < 0.5 ? 1.0 : -1.0) + dsp::polyBlep(p, dt) - dsp::polyBlep(fraction(p + 0.5), dt);
        const double x = m1 * 3.0;
        const double y = x < 1.0 ? mix(sine, triangle, x) : x < 2.0 ? mix(triangle, saw, x - 1.0) : mix(saw, square, std::min(1.0, x - 2.0));
        const double sub = std::sin(dsp::kTwoPi * ph[1]);
        ph[0] = fraction(p + dt);
        ph[1] = fraction(ph[1] + dt * 0.5);
        return (y + m2 * sub * 0.8) / (1.0 + m2 * 0.4);
    }
    case Oscillator::swarm:
    {
        // Seven saws spread up to +-60 cents; Spread brings the outer six in.
        const double side = 0.15 + 0.6 * m2;
        double total = 0.0;
        for (int k = 0; k < 7; ++k)
        {
            const double offset = (k - 3) / 3.0;
            const double d = std::min(0.45, dt * (1.0 + offset * m1 * 0.0347)); // ~ +-60 cents
            auto& p = ph[static_cast<std::size_t>(k)];
            total += (2.0 * p - 1.0 - dsp::polyBlep(p, d)) * (k == 3 ? 1.0 : side);
            p = fraction(p + d);
        }
        return total / std::sqrt(1.0 + 6.0 * side * side) * 0.8;
    }
    case Oscillator::harmonicFm:
    case Oscillator::inharmonicFm:
    {
        const double ratio = st.oscillator == Oscillator::harmonicFm ? std::round(1.0 + m1 * 7.0) : 0.5 + m1 * 6.5;
        const double index = m2 * (st.oscillator == Oscillator::harmonicFm ? 6.0 : 5.0);
        const double y = std::sin(dsp::kTwoPi * ph[0] + index * std::sin(dsp::kTwoPi * ph[1]));
        ph[0] = fraction(ph[0] + dt);
        ph[1] = fraction(ph[1] + std::min(0.45, dt * ratio));
        return y;
    }
    case Oscillator::foldFm:
    {
        const double y = std::sin(dsp::kTwoPi * ph[0] + m1 * 1.6 * ev.aux);
        ev.aux = y;
        ph[0] = fraction(ph[0] + dt);
        return std::sin(dsp::kPi * 0.5 * (1.0 + m2 * 6.0) * y);
    }
    case Oscillator::squelch:
    {
        const double saw = 2.0 * ph[0] - 1.0 - dsp::polyBlep(ph[0], dt);
        ph[0] = fraction(ph[0] + dt);
        const auto o = ev.inner[0].process(saw);
        return o.lowPass * 0.8 + o.bandPass * 0.5;
    }
    case Oscillator::pluck:
    {
        if (ev.pluck == nullptr)
            return 0.0;
        const double length = std::clamp(1.0 / std::max(dt, 1.0e-6), 2.0, static_cast<double>(pluckCapacity_ - 2));
        double position = ev.pluckWrite - length;
        if (position < 0.0)
            position += pluckCapacity_;
        const auto i = static_cast<int>(position);
        const double f = position - i;
        const double a = ev.pluck[i], b = ev.pluck[(i + 1) % pluckCapacity_];
        const double y = a + (b - a) * f;
        ev.pluckState += (y - ev.pluckState) * ev.aux; // brightness (damping)
        ev.pluck[ev.pluckWrite] = ev.pluckState * ev.aux2; // decay
        ev.pluckWrite = (ev.pluckWrite + 1) % pluckCapacity_;
        return y * 1.5;
    }
    case Oscillator::bitgrunge:
    {
        const double saw = 2.0 * ph[0] - 1.0;
        ph[0] = fraction(ph[0] + dt);
        ph[1] += dt * (2.0 + (1.0 - m2) * 30.0);
        if (ph[1] >= 1.0)
        {
            ph[1] -= std::floor(ph[1]);
            const double levels = std::exp2(1.0 + (1.0 - m1) * 6.0);
            ev.aux = std::round(saw * levels) / levels;
        }
        return ev.aux;
    }
    case Oscillator::noiseLoop:
    {
        const double n = ev.noise.next();
        const double a = ev.inner[0].process(n).bandPass;
        const double b = ev.inner[1].process(n).bandPass;
        return (a * (1.0 - m2 * 0.7) + b * m2) * ev.aux;
    }
    case Oscillator::shepard:
    {
        // Eight octave-spaced sines under a bell curve, gliding up or down forever.
        ev.shepard += (m1 * 2.0 - 1.0) / sampleRate_;
        if (ev.shepard >= 1.0)
        {
            // An octave up: every partial takes over the next one's place (phase and level).
            ev.shepard -= 1.0;
            for (std::size_t k = ph.size() - 1; k > 0; --k)
            {
                ph[k] = ph[k - 1];
                ev.weights[k] = ev.weights[k - 1];
            }
            ph[0] = 0.0;
            ev.weights[0] = 0.0;
        }
        else if (ev.shepard < 0.0)
        {
            ev.shepard += 1.0;
            for (std::size_t k = 0; k + 1 < ph.size(); ++k)
            {
                ph[k] = ph[k + 1];
                ev.weights[k] = ev.weights[k + 1];
            }
            ph[ph.size() - 1] = 0.0;
            ev.weights[ev.weights.size() - 1] = 0.0;
        }
        double increment = dt * std::exp2(ev.shepard - 3.5);
        double total = 0.0;
        for (std::size_t k = 0; k < ph.size(); ++k, increment *= 2.0)
        {
            if (increment < 0.45)
                total += ev.weights[k] * std::sin(dsp::kTwoPi * ph[k]);
            ph[k] = fraction(ph[k] + std::min(increment, 0.45));
        }
        return total;
    }
    case Oscillator::chip:
    {
        const double duty = 0.125 + m1 * 0.375;
        const double p = ph[0];
        double y = (p < duty ? 1.0 : -1.0) + dsp::polyBlep(p, dt) - dsp::polyBlep(fraction(p - duty + 1.0), dt) + 1.0 - 2.0 * duty;
        ph[0] = fraction(p + dt);
        const double levels = std::exp2(4.0 - m2 * 3.0);
        return std::round(y * levels) / levels * 0.8;
    }
    case Oscillator::additive:
    {
        const double theta = dsp::kTwoPi * ph[0];
        const double c = 2.0 * std::cos(theta);
        double previous = 0.0, current = std::sin(theta), total = 0.0;
        for (std::size_t k = 0; k < ev.partialGain.size(); ++k)
        {
            total += ev.partialGain[k] * current;
            const double next = c * current - previous;
            previous = current;
            current = next;
        }
        ph[0] = fraction(ph[0] + dt);
        return total;
    }
    case Oscillator::rain:
    {
        const double density = 2.0 + m1 * 60.0;
        if (0.5 * (ev.noise.next() + 1.0) < density / sampleRate_)
            for (auto& g : ev.grains)
                if (!g.active)
                {
                    g.active = true;
                    g.phase = 0.0;
                    g.age = 0.0;
                    g.increment = std::min(0.45, dt * std::exp2(m2 * 2.0 * ev.noise.next()));
                    g.length = (0.02 + 0.05 * (ev.noise.next() + 1.0)) * sampleRate_;
                    break;
                }
        double total = 0.0;
        for (auto& g : ev.grains)
        {
            if (!g.active)
                continue;
            const double window = std::sin(dsp::kPi * g.age / g.length);
            total += std::sin(dsp::kTwoPi * g.phase) * window * window;
            g.phase = fraction(g.phase + g.increment);
            g.age += 1.0;
            if (g.age >= g.length)
                g.active = false;
        }
        return total * 0.6;
    }
    }
    return 0.0;
}

void Meld::renderVoice(Voice& v, double* left, double* right, int start, int end, const dsp::TransportState& transport) noexcept
{
    const double glideSeconds = glide_->get() / 1000.0;
    const double velocityAmp = 1.0 - velocityToAmp_->get() / 100.0 * (1.0 - v.velocity);
    const bool newest = v.age == ageCounter_;

    for (int chunk = start; chunk < end && v.active; chunk += kControlInterval)
    {
        const int n = std::min(kControlInterval, end - chunk);
        const double seconds = n / sampleRate_;
        v.pitch = glideSeconds > 0.0 ? v.targetPitch + (v.pitch - v.targetPitch) * std::exp(-4.6 * seconds / glideSeconds) : v.targetPitch;
        const double bend = expressions_.bend(v.channel);

        std::array<double, numSources> sources{};
        sources[srcEnv2A] = v.engines[0].on ? v.engines[0].mod.advance(n) : 0.0;
        sources[srcEnv2B] = v.engines[1].on ? v.engines[1].mod.advance(n) : 0.0;
        sources[srcVelocity] = v.velocity;
        sources[srcNote] = (v.note - 60) / 60.0;
        sources[srcPressure] = expressions_.pressure(v.channel, v.note);
        sources[srcSlide] = expressions_.slide(v.channel);
        sources[srcModWheel] = expressions_.modWheel();
        sources[srcRandom] = v.random;
        sources[srcPitchBend] = std::clamp(bend / 48.0, -1.0, 1.0);

        std::array<double, numDestinations> mod{};
        const auto accumulate = [&]
        {
            mod.fill(0.0);
            for (const auto& slot : matrix_)
            {
                const int source = slot.source->getIndex();
                const int target = slot.destination->getIndex();
                if (source > srcOff && source < numSources && target > 0 && target < numDestinations)
                    mod[static_cast<std::size_t>(target)] += slot.amount->get() / 100.0 * sources[static_cast<std::size_t>(source)];
            }
        };
        accumulate(); // (without the LFOs: for their rates)
        for (std::size_t e = 0; e < 2; ++e)
        {
            const auto& p = engines_[e];
            auto& ev = v.engines[e];
            const double rateScale = std::exp2(mod[static_cast<std::size_t>(destination(static_cast<int>(e), edLfoRate))] * 3.0);
            const double rate1 = p.lfoSync->get() ? transport.bpm / 60.0 / dsp::syncDivisionBeats(p.lfoDivision->getIndex())
                                                  : p.lfoRate[0]->get() * rateScale;
            ev.lfoPhase[0] += seconds * rate1;
            ev.lfoPhase[1] += seconds * p.lfoRate[1]->get() * rateScale;
            const auto seed = static_cast<std::uint32_t>(v.age * 7 + e * 3);
            const double l1 = dsp::waveformAt(static_cast<dsp::Waveform>(std::clamp(p.lfoShape[0]->getIndex(), 0, 7)), ev.lfoPhase[0], seed);
            const double l2 = dsp::waveformAt(static_cast<dsp::Waveform>(std::clamp(p.lfoShape[1]->getIndex(), 0, 7)), ev.lfoPhase[1], seed + 1);
            sources[e == 0 ? srcLfo1A : srcLfo1B] = l1;
            sources[e == 0 ? srcLfo2A : srcLfo2B] = l2;
        }
        accumulate();

        std::array<EngineState, 2> state{};
        for (std::size_t e = 0; e < 2; ++e)
        {
            auto& ev = v.engines[e];
            if (!ev.on)
                continue;
            const auto& p = engines_[e];
            const auto at = [&](EngineDestination d) { return mod[static_cast<std::size_t>(destination(static_cast<int>(e), d))]; };
            auto& st = state[e];
            st.oscillator = static_cast<Oscillator>(std::clamp(p.oscillator->getIndex(), 0, 12));
            st.m1 = std::clamp(p.macro1->get() / 100.0 + at(edMacro1), 0.0, 1.0);
            st.m2 = std::clamp(p.macro2->get() / 100.0 + at(edMacro2), 0.0, 1.0);
            const double pitch = v.pitch + p.octave->get() * 12.0 + p.semi->get() + p.fine->get() / 100.0 + bend + at(edPitch) * 24.0;
            const double frequency = dsp::noteToFrequency(pitch);
            st.dt = frequency / sampleRate_;
            const auto choice = filterChoice(p.filterType->getIndex());
            st.filterMode = choice.mode;
            const double cutoff = std::clamp(p.cutoff->get() * std::exp2(p.keyTrack->get() / 100.0 * (pitch - 60.0) / 12.0 + at(edCutoff) * 5.0), 20.0,
                                             std::min(20000.0, sampleRate_ * 0.45));
            const double resonance = std::clamp(p.resonance->get() / 100.0 + at(edResonance), 0.0, 1.0) * 0.97;
            ev.filter.set(cutoff, resonance, sampleRate_, static_cast<dsp::FilterCircuit>(std::clamp(p.circuit->getIndex(), 0, 3)), choice.steep);
            st.level = p.level->get() / 100.0 * std::clamp(1.0 + at(edLevel), 0.0, 2.0) * velocityAmp * 0.4;
            const double pan = std::clamp(p.pan->get() / 100.0 + at(edPan), -1.0, 1.0);
            st.panLeft = std::min(1.0, 1.0 - pan);
            st.panRight = std::min(1.0, 1.0 + pan);

            // Per-oscillator settings that change slowly.
            const double nyquist = sampleRate_ * 0.45;
            switch (st.oscillator)
            {
            case Oscillator::squelch:
                ev.inner[0].set(std::min(frequency * (1.5 + st.m1 * 14.0), nyquist), 0.5 + st.m2 * 0.48, sampleRate_, dsp::FilterCircuit::warm);
                break;
            case Oscillator::noiseLoop:
            {
                const double focus = 0.6 + st.m1 * 0.39;
                ev.inner[0].set(std::min(frequency, nyquist), focus, sampleRate_, dsp::FilterCircuit::clean);
                ev.inner[1].set(std::min(frequency * 2.0, nyquist), focus, sampleRate_, dsp::FilterCircuit::clean);
                ev.aux = 1.5 / std::sqrt(std::max(0.02, 2.0 - 1.97 * focus));
                break;
            }
            case Oscillator::pluck:
            {
                const double t60 = 0.15 * std::exp2(st.m2 * 6.0); // 0.15 .. 9.6 s
                ev.aux = 0.15 + 0.85 * st.m1;
                ev.aux2 = std::pow(0.001, 1.0 / (std::max(st.dt, 1.0e-6) * t60 * sampleRate_));
                break;
            }
            case Oscillator::shepard:
            {
                const double sigma = 0.5 + st.m2 * 2.0;
                double power = 0.0;
                for (std::size_t k = 0; k < ev.weights.size(); ++k)
                {
                    const double position = static_cast<double>(k) - 3.5 + ev.shepard;
                    ev.weights[k] = std::exp(-position * position / (2.0 * sigma * sigma));
                    power += ev.weights[k] * ev.weights[k];
                }
                for (auto& w : ev.weights)
                    w *= 0.7 / std::sqrt(std::max(1.0e-9, power));
                break;
            }
            case Oscillator::additive:
            {
                const double exponent = 2.2 - 1.8 * st.m1;
                double total = 0.0;
                for (std::size_t k = 0; k < ev.partialGain.size(); ++k)
                {
                    const double harmonic = static_cast<double>(k + 1);
                    const bool audible = harmonic * st.dt < 0.45;
                    const double even = (k % 2 == 1) ? 1.0 - st.m2 : 1.0;
                    ev.partialGain[k] = audible ? std::pow(harmonic, -exponent) * even : 0.0;
                    total += ev.partialGain[k];
                }
                for (auto& g : ev.partialGain)
                    g *= 1.2 / std::max(1.0e-9, total);
                break;
            }
            case Oscillator::basicShapes:
            case Oscillator::swarm:
            case Oscillator::harmonicFm:
            case Oscillator::inharmonicFm:
            case Oscillator::foldFm:
            case Oscillator::bitgrunge:
            case Oscillator::chip:
            case Oscillator::rain: break; // (nothing to prepare)
            }
            if (newest)
            {
                displayMacros_[e * 2].store(static_cast<float>(st.m1), std::memory_order_relaxed);
                displayMacros_[e * 2 + 1].store(static_cast<float>(st.m2), std::memory_order_relaxed);
            }
        }

        for (int i = chunk; i < chunk + n; ++i)
            for (std::size_t e = 0; e < 2; ++e)
            {
                auto& ev = v.engines[e];
                if (!ev.on)
                    continue;
                const double y = ev.filter.process(oscillate(ev, state[e]), state[e].filterMode) * ev.amp.next() * state[e].level;
                left[i] += y * state[e].panLeft;
                right[i] += y * state[e].panRight;
            }

        for (auto& ev : v.engines)
            if (ev.on && !ev.amp.isActive())
                ev.on = false;
        if (!v.engines[0].on && !v.engines[1].on)
            v.active = false;
    }
}

void Meld::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept
{
    const int numSamples = buffer.getNumSamples();
    buffer.clear();
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);
    const auto transport = dsp::readTransport(getPlayHead());
    expressions_.setMpe(mpe_->get(), noteBendRange_->get(), bendRange_->get());
    for (auto& v : voices_)
        if (v.active)
            updateEnvelopes(v);

    int position = 0;
    const auto renderUntil = [&](int until)
    {
        for (auto& v : voices_)
            if (v.active && until > position)
                renderVoice(v, left, right, position, until, transport);
        position = std::max(position, until);
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
    buffer.applyGain(1, 0, numSamples, gain);
    int active = 0;
    for (const auto& v : voices_)
        active += v.active ? 1 : 0;
    activeVoices_.store(active, std::memory_order_relaxed);
}

} // namespace dojo::devices
