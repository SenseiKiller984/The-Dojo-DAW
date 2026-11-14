#include "devices/DriftSynth.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices
{
namespace
{
constexpr std::array<int, 4> kVoiceCounts{4, 8, 12, 16};

/** The LFO's shapes: seven periodic ones and two one-shot envelopes (1 falling to 0). */
double lfoValue(int shape, double totalPhase, std::uint32_t seed) noexcept
{
    switch (shape)
    {
    case 0: return dsp::waveformAt(dsp::Waveform::sine, totalPhase, seed);
    case 1: return dsp::waveformAt(dsp::Waveform::triangle, totalPhase, seed);
    case 2: return dsp::waveformAt(dsp::Waveform::sawUp, totalPhase, seed);
    case 3: return dsp::waveformAt(dsp::Waveform::sawDown, totalPhase, seed);
    case 4: return dsp::waveformAt(dsp::Waveform::square, totalPhase, seed);
    case 5: return dsp::waveformAt(dsp::Waveform::sampleHold, totalPhase, seed);
    case 6: return dsp::waveformAt(dsp::Waveform::wander, totalPhase, seed);
    case 7: return std::max(0.0, 1.0 - totalPhase);
    case 8: return std::exp(-4.0 * totalPhase);
    default: return 0.0;
    }
}

double fraction(double x) noexcept
{
    return x - std::floor(x);
}

/** Oscillator 2: sine, triangle, saw, square (the last two band-limited). */
double osc2Sample(int shape, double p, double dt) noexcept
{
    switch (shape)
    {
    case 0: return std::sin(dsp::kTwoPi * p);
    case 1: return p < 0.5 ? 4.0 * p - 1.0 : 3.0 - 4.0 * p;
    case 2: return 2.0 * p - 1.0 - dsp::polyBlep(p, dt);
    default: return (p < 0.5 ? 1.0 : -1.0) + dsp::polyBlep(p, dt) - dsp::polyBlep(fraction(p + 0.5), dt);
    }
}
} // namespace

const juce::StringArray& DriftSynth::getSourceNames()
{
    static const juce::StringArray names{"Off", "Env 1", "Env 2", "LFO", "Velocity", "Note", "Pressure", "Slide", "Mod Wheel", "Random"};
    return names;
}

const juce::StringArray& DriftSynth::getDestinationNames()
{
    static const juce::StringArray names{"Off",        "Osc 1 Shape", "Osc 1 Gain",  "Osc 2 Gain", "Osc 2 Detune", "Noise Gain",
                                         "Pitch",      "Filter Freq", "Filter Res",  "LFO Rate",   "LFO Amount",   "Pan"};
    return names;
}

double DriftSynth::osc1Wave(Osc1Shape shape, double p, double amount) noexcept
{
    const double s = std::clamp(amount, 0.0, 1.0);
    switch (shape)
    {
    case Osc1Shape::sine: return std::sin(dsp::kTwoPi * p + s * 2.5 * std::sin(dsp::kTwoPi * p));
    case Osc1Shape::triangle:
    {
        const double c = 0.5 + 0.48 * s;
        return p < c ? -1.0 + 2.0 * p / c : 1.0 - 2.0 * (p - c) / (1.0 - c);
    }
    case Osc1Shape::sharkTooth:
    {
        const double k = 0.5 + 5.0 * s;
        return 2.0 * (std::exp(k * p) - 1.0) / (std::exp(k) - 1.0) - 1.0;
    }
    case Osc1Shape::saturated:
    {
        const double g = 1.0 + 9.0 * s;
        return std::tanh(g * std::sin(dsp::kTwoPi * p)) / std::tanh(g);
    }
    case Osc1Shape::saw: return (1.0 - 0.5 * s) * (2.0 * p - 1.0) + 0.5 * s * (2.0 * fraction(2.0 * p) - 1.0);
    case Osc1Shape::pulse:
    {
        const double w = 0.5 - 0.45 * s;
        return (p < w ? 1.0 : -1.0) + 1.0 - 2.0 * w;
    }
    }
    return 0.0;
}

DriftSynth::DriftSynth()
    : BuiltinProcessor(kTypeId, "Drift", BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true), true)
{
    osc1Shape_ = addChoice("o1shape", "Osc 1 Shape", {"Sine", "Triangle", "Shark Tooth", "Saturated", "Saw", "Pulse"}, 4);
    osc1Amount_ = addFloat("o1amt", "Osc 1 Shape Amount", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    osc1Octave_ = addFloat("o1oct", "Osc 1 Octave", {-2.0f, 3.0f, 1.0f}, 0.0f);
    osc1ShapeSource_ = addChoice("o1modsrc", "Osc 1 Shape Mod Source", {"Env 2", "LFO"}, 1);
    osc1ShapeMod_ = addFloat("o1mod", "Osc 1 Shape Mod", {-100.0f, 100.0f, 0.1f}, 0.0f, "%");
    osc2Shape_ = addChoice("o2shape", "Osc 2 Shape", {"Sine", "Triangle", "Saw", "Square"}, 2);
    osc2Octave_ = addFloat("o2oct", "Osc 2 Octave", {-2.0f, 3.0f, 1.0f}, 0.0f);
    osc2Detune_ = addFloat("o2detune", "Osc 2 Detune", {-100.0f, 100.0f, 0.1f}, 7.0f, "ct");
    osc1Level_ = addFloat("o1level", "Osc 1 Level", {0.0f, 100.0f, 0.1f}, 100.0f, "%");
    osc2Level_ = addFloat("o2level", "Osc 2 Level", {0.0f, 100.0f, 0.1f}, 50.0f, "%");
    noiseLevel_ = addFloat("noise", "Noise", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    pitchEnv_ = addFloat("pitchenv", "Pitch Env 2", {-24.0f, 24.0f, 0.01f}, 0.0f, "st");
    pitchLfo_ = addFloat("pitchlfo", "Pitch LFO", skewedRange(0.0f, 12.0f, 1.0f, 0.01f), 0.0f, "st");
    filterType_ = addChoice("ftype", "Filter Type", {"LP I", "LP II", "High Pass"}, 1);
    frequency_ = addFloat("freq", "Frequency", skewedRange(20.0f, 20000.0f, 1500.0f), 2500.0f, "Hz");
    resonance_ = addFloat("res", "Resonance", {0.0f, 100.0f, 0.1f}, 20.0f, "%");
    keyTrack_ = addFloat("key", "Key Track", {0.0f, 100.0f, 0.1f}, 50.0f, "%");
    filterEnv_ = addFloat("fenv", "Filter Env 2", {-100.0f, 100.0f, 0.1f}, 30.0f, "%");
    filterLfo_ = addFloat("flfo", "Filter LFO", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    highPass_ = addFloat("hp", "High Pass", skewedRange(10.0f, 1000.0f, 100.0f), 10.0f, "Hz");

    const std::array<const char*, 4> stage{"Attack", "Decay", "Sustain", "Release"};
    const std::array<const char*, 4> suffix{"a", "d", "s", "r"};
    constexpr std::array<float, 4> env1Defaults{2.0f, 500.0f, 80.0f, 300.0f};
    constexpr std::array<float, 4> env2Defaults{5.0f, 800.0f, 0.0f, 400.0f};
    for (std::size_t k = 0; k < 4; ++k)
    {
        const bool level = k == 2;
        const auto range = level ? juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f)
                                 : skewedRange(k == 0 ? 0.5f : 1.0f, k == 0 ? 10000.0f : 20000.0f, k == 0 ? 200.0f : 500.0f);
        env1_[k] = addFloat(juce::String("e1") + suffix[k], juce::String("Env 1 ") + stage[k], range, env1Defaults[k], level ? "%" : "ms");
        env2_[k] = addFloat(juce::String("e2") + suffix[k], juce::String("Env 2 ") + stage[k], range, env2Defaults[k], level ? "%" : "ms");
    }
    env2Cycle_ = addBool("e2cycle", "Env 2 Cycle", false);
    lfoShape_ = addChoice("lfoshape", "LFO Shape",
                          {"Sine", "Triangle", "Saw Up", "Saw Down", "Square", "S&H", "Wander", "Linear Env", "Exp Env"}, 0);
    lfoRate_ = addFloat("lforate", "LFO Rate", skewedRange(0.01f, 40.0f, 2.0f), 1.5f, "Hz");
    lfoSync_ = addBool("lfosync", "LFO Sync", false);
    lfoDivision_ = addChoice("lfodiv", "LFO Division", dsp::syncDivisionNames(), 9);
    lfoAmount_ = addFloat("lfoamt", "LFO Amount", {0.0f, 100.0f, 0.1f}, 100.0f, "%");
    lfoRetrigger_ = addBool("lforetrig", "LFO Retrigger", true);

    struct SlotDefault
    {
        int source, destination;
        float amount;
    };
    constexpr std::array<SlotDefault, kMatrixSlots> slotDefaults{
        {{srcVelocity, dstFilterFrequency, 30.0f}, {srcPressure, dstFilterFrequency, 40.0f}, {srcSlide, dstOsc1Shape, 50.0f}}};
    for (std::size_t m = 0; m < matrix_.size(); ++m)
    {
        const juce::String id = "m" + juce::String(m + 1);
        const juce::String name = "Mod " + juce::String(m + 1) + " ";
        matrix_[m].source = addChoice(id + "src", name + "Source", getSourceNames(), slotDefaults[m].source);
        matrix_[m].destination = addChoice(id + "dst", name + "Target", getDestinationNames(), slotDefaults[m].destination);
        matrix_[m].amount = addFloat(id + "amt", name + "Amount", {-100.0f, 100.0f, 0.1f}, slotDefaults[m].amount, "%");
    }

    voiceMode_ = addChoice("mode", "Voice Mode", {"Poly", "Mono", "Stereo", "Unison"}, 0);
    voiceCount_ = addChoice("voices", "Voices", {"4", "8", "12", "16"}, 1);
    spread_ = addFloat("spread", "Spread", {0.0f, 100.0f, 0.1f}, 30.0f, "%");
    glide_ = addFloat("glide", "Glide", skewedRange(0.0f, 2000.0f, 200.0f), 0.0f, "ms");
    legato_ = addBool("legato", "Legato", false);
    drift_ = addFloat("drift", "Drift", {0.0f, 100.0f, 0.1f}, 25.0f, "%");
    mpe_ = addBool("mpe", "MPE", true);
    noteBendRange_ = addFloat("notebend", "Note Bend Range", {0.0f, 96.0f, 1.0f}, 48.0f, "st");
    bendRange_ = addFloat("bend", "Bend Range", {0.0f, 24.0f, 1.0f}, 2.0f, "st");
    velocityToAmp_ = addFloat("velamp", "Velocity > Amp", {0.0f, 100.0f, 0.1f}, 50.0f, "%");
    volume_ = addFloat("volume", "Volume", {-36.0f, 6.0f, 0.1f}, -6.0f, "dB");
}

int DriftSynth::polyphony() const noexcept
{
    return voiceMode_->getIndex() == 1 ? 1 : kVoiceCounts[static_cast<std::size_t>(std::clamp(voiceCount_->getIndex(), 0, 3))];
}

void DriftSynth::prepareDsp(double, int)
{
    for (auto& v : voices_)
    {
        v.active = false;
        for (auto& f : v.filters)
            f.reset();
        for (auto& h : v.highPass)
            h.reset();
    }
    expressions_.reset();
    held_.clear();
    sustainPedal_ = false;
    lastPitch_ = -1.0;
    activeVoices_.store(0);
}

void DriftSynth::updateEnvelopes(Voice& voice) noexcept
{
    voice.env1.setParameters(env1_[0]->get() / 1000.0, env1_[1]->get() / 1000.0, env1_[2]->get() / 100.0, env1_[3]->get() / 1000.0, sampleRate_);
    voice.env2.setParameters(env2_[0]->get() / 1000.0, env2_[1]->get() / 1000.0, env2_[2]->get() / 100.0, env2_[3]->get() / 1000.0, sampleRate_);
}

void DriftSynth::startNote(int note, int channel, double velocity) noexcept
{
    expressions_.noteOn(note);
    Voice* chosen = nullptr;
    bool retrigger = true;
    if (polyphony() == 1)
    {
        const bool keyHeld = !held_.empty();
        held_.push(note, channel, velocity);
        chosen = &voices_[0];
        retrigger = !(legato_->get() && keyHeld && chosen->active);
    }
    else
    {
        chosen = &dsp::pickVoice(voices_, polyphony(), note, channel, [](const Voice& v) { return v.env1.isReleasing(); });
    }
    auto& v = *chosen;
    const bool wasActive = v.active;
    v.note = note;
    v.channel = channel;
    v.targetPitch = note;
    v.keyDown = true;
    v.sustained = false;
    v.active = true;
    if (!retrigger)
        return;

    v.velocity = velocity;
    v.random = random_.next();
    v.age = ++ageCounter_;
    v.pitch = glide_->get() > 0.0f && lastPitch_ >= 0.0 ? lastPitch_ : static_cast<double>(note);
    lastPitch_ = note;
    v.cyclePhase = 0.0;
    v.lfoPhase = lfoRetrigger_->get() ? 0.0 : freeLfoPhase_;
    if (!wasActive)
    {
        for (auto& f : v.filters)
            f.reset();
        for (auto& h : v.highPass)
            h.reset();
        v.env1.reset();
        v.env2.reset();
        for (std::size_t s = 0; s < v.osc1Phase.size(); ++s)
        {
            v.osc1Phase[s] = s == 0 ? 0.0 : 0.5 * (random_.next() + 1.0);
            v.osc2Phase[s] = 0.5 * (random_.next() + 1.0);
        }
        // Every voice its own slightly-off tuning and filter, wandering at its own pace.
        v.driftOffset = random_.next();
        v.driftCutoff = random_.next();
        v.driftTime = 10.0 * (random_.next() + 1.0);
        v.seed = static_cast<std::uint32_t>(ageCounter_ * 2654435761u);
    }
    updateEnvelopes(v);
    v.env1.noteOn();
    v.env2.noteOn();
}

void DriftSynth::releaseNote(int note, int channel) noexcept
{
    if (polyphony() == 1)
    {
        held_.remove(note, channel);
        auto& v = voices_[0];
        if (!v.active)
            return;
        if (!held_.empty())
        {
            const auto top = held_.top();
            if (top.note != v.note || top.channel != v.channel)
            {
                v.note = top.note;
                v.channel = top.channel;
                v.targetPitch = top.note;
                lastPitch_ = top.note;
                if (!legato_->get())
                {
                    v.env1.noteOn();
                    v.env2.noteOn();
                }
            }
            return;
        }
        if (v.note != note || v.channel != channel)
            return;
    }
    for (auto& v : voices_)
        if (v.active && v.keyDown && v.note == note && v.channel == channel)
        {
            v.keyDown = false;
            if (sustainPedal_)
            {
                v.sustained = true;
                continue;
            }
            v.env1.noteOff();
            v.env2.noteOff();
        }
}

void DriftSynth::handleMidi(const juce::MidiMessage& message) noexcept
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
                v.env1.noteOff();
                v.env2.noteOff();
            }
    }
    else if (message.isAllSoundOff())
    {
        for (auto& v : voices_)
            v.active = false;
        held_.clear();
    }
    else if (message.isAllNotesOff())
    {
        held_.clear();
        for (auto& v : voices_)
            if (v.active)
            {
                v.keyDown = v.sustained = false;
                v.env1.noteOff();
                v.env2.noteOff();
            }
    }
}

void DriftSynth::renderVoice(Voice& v, double* left, double* right, int start, int end, const dsp::TransportState& transport) noexcept
{
    // The stack of sub-voices for the mode: detune (semitones) and pan of each.
    const int mode = voiceMode_->getIndex();
    const double spread = spread_->get() / 100.0;
    int stack = 1;
    std::array<double, kMaxStack> stackDetune{}, stackPan{};
    if (mode == 2)
    {
        stack = 2;
        stackDetune = {-0.12 * spread, 0.12 * spread, 0.0, 0.0};
        stackPan = {-(0.5 + 0.5 * spread), 0.5 + 0.5 * spread, 0.0, 0.0};
    }
    else if (mode == 3)
    {
        stack = 4;
        constexpr std::array<double, kMaxStack> offsets{-1.0, -1.0 / 3.0, 1.0 / 3.0, 1.0};
        for (std::size_t s = 0; s < offsets.size(); ++s)
        {
            stackDetune[s] = offsets[s] * 0.2 * spread;
            stackPan[s] = offsets[s] * spread;
        }
    }
    const double stackScale = 1.0 / std::sqrt(static_cast<double>(stack));
    std::array<double, kMaxStack> stackLeft{}, stackRight{};
    for (int s = 0; s < stack; ++s)
    {
        stackLeft[static_cast<std::size_t>(s)] = std::min(1.0, 1.0 - stackPan[static_cast<std::size_t>(s)]) * stackScale;
        stackRight[static_cast<std::size_t>(s)] = std::min(1.0, 1.0 + stackPan[static_cast<std::size_t>(s)]) * stackScale;
    }

    const auto shape1 = static_cast<Osc1Shape>(std::clamp(osc1Shape_->getIndex(), 0, 5));
    const int shape2 = osc2Shape_->getIndex();
    const int filterType = filterType_->getIndex();
    const auto filterMode = filterType == 2 ? dsp::FilterMode::highPass : dsp::FilterMode::lowPass;
    const auto circuit = filterType == 1 ? dsp::FilterCircuit::ladder : dsp::FilterCircuit::warm;
    const bool steep = filterType == 1;
    const double glideSeconds = glide_->get() / 1000.0;
    const double drift = drift_->get() / 100.0;
    const double velocityAmp = 1.0 - velocityToAmp_->get() / 100.0 * (1.0 - v.velocity);
    const bool cycle = env2Cycle_->get();
    const double cycleSeconds = std::max(0.002, (env2_[0]->get() + env2_[1]->get()) / 1000.0);
    const double cycleRise = env2_[0]->get() / 1000.0 / cycleSeconds;
    const int lfoShape = lfoShape_->getIndex();
    const double lfoBaseRate = lfoSync_->get() ? transport.bpm / 60.0 / dsp::syncDivisionBeats(lfoDivision_->getIndex())
                                               : static_cast<double>(lfoRate_->get());
    for (auto& h : v.highPass)
        h.setFrequency(highPass_->get(), sampleRate_);
    const bool highPass = highPass_->get() > 10.5f;

    for (int chunk = start; chunk < end && v.active; chunk += kControlInterval)
    {
        const int n = std::min(kControlInterval, end - chunk);
        const double seconds = n / sampleRate_;

        // Sources.
        std::array<double, numSources> sources{};
        sources[srcEnv1] = v.env1.getLevel();
        if (cycle)
        {
            v.cyclePhase = fraction(v.cyclePhase + seconds / cycleSeconds);
            sources[srcEnv2] = v.cyclePhase < cycleRise ? v.cyclePhase / std::max(1.0e-6, cycleRise)
                                                         : 1.0 - (v.cyclePhase - cycleRise) / std::max(1.0e-6, 1.0 - cycleRise);
            v.env2.advance(n);
        }
        else
        {
            sources[srcEnv2] = v.env2.advance(n);
        }
        sources[srcVelocity] = v.velocity;
        sources[srcNote] = (v.note - 60) / 60.0;
        sources[srcPressure] = expressions_.pressure(v.channel, v.note);
        sources[srcSlide] = expressions_.slide(v.channel);
        sources[srcModWheel] = expressions_.modWheel();
        sources[srcRandom] = v.random;

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
        accumulate(); // (without this chunk's LFO, for its rate)
        v.lfoPhase += seconds * lfoBaseRate * std::exp2(mod[dstLfoRate] * 3.0);
        const double lfoAmount = lfoAmount_->get() / 100.0 * std::clamp(1.0 + mod[dstLfoAmount], 0.0, 2.0);
        sources[srcLfo] = lfoValue(lfoShape, v.lfoPhase, v.seed) * lfoAmount;
        accumulate();
        const double lfo = sources[srcLfo];
        const double env2 = sources[srcEnv2];

        // Pitch: glide, bend, envelope, LFO, matrix, drift.
        v.pitch = glideSeconds > 0.0 ? v.targetPitch + (v.pitch - v.targetPitch) * std::exp(-4.6 * seconds / glideSeconds) : v.targetPitch;
        v.driftTime += seconds;
        const double wander = dsp::waveformAt(dsp::Waveform::wander, v.driftTime * 0.35, v.seed);
        const double driftCents = drift * (10.0 * v.driftOffset + 6.0 * wander);
        const double pitch = v.pitch + expressions_.bend(v.channel) + pitchEnv_->get() * env2 + pitchLfo_->get() * lfo + mod[dstPitch] * 24.0 +
                             driftCents / 100.0;
        const double dt1 = dsp::noteToFrequency(pitch + osc1Octave_->get() * 12.0) / sampleRate_;
        const double dt2 = dsp::noteToFrequency(pitch + osc2Octave_->get() * 12.0 + (osc2Detune_->get() + mod[dstOsc2Detune] * 100.0) / 100.0) / sampleRate_;
        const double shapeSource = osc1ShapeSource_->getIndex() == 0 ? env2 : lfo;
        const double shapeAmount = std::clamp(osc1Amount_->get() / 100.0 + osc1ShapeMod_->get() / 100.0 * shapeSource + mod[dstOsc1Shape], 0.0, 1.0);
        const double level1 = osc1Level_->get() / 100.0 * std::clamp(1.0 + mod[dstOsc1Gain], 0.0, 2.0);
        const double level2 = osc2Level_->get() / 100.0 * std::clamp(1.0 + mod[dstOsc2Gain], 0.0, 2.0);
        const double noiseLevel = noiseLevel_->get() / 100.0 * std::clamp(1.0 + mod[dstNoiseGain], 0.0, 2.0);

        const double octaves = filterEnv_->get() / 100.0 * env2 * 5.0 + filterLfo_->get() / 100.0 * lfo * 3.0 +
                               keyTrack_->get() / 100.0 * (v.pitch - 60.0) / 12.0 + mod[dstFilterFrequency] * 5.0 + drift * v.driftCutoff * 0.25;
        const double cutoff = std::clamp(frequency_->get() * std::exp2(octaves), 20.0, std::min(20000.0, sampleRate_ * 0.45));
        const double resonance = std::clamp(resonance_->get() / 100.0 + mod[dstFilterResonance], 0.0, 1.0) * 0.98;
        for (auto& f : v.filters)
            f.set(cutoff, resonance, sampleRate_, circuit, steep);
        displayCutoff_.store(static_cast<float>(cutoff), std::memory_order_relaxed);

        const double pan = std::clamp(mod[dstPan], -1.0, 1.0);
        const double panLeft = std::min(1.0, 1.0 - pan), panRight = std::min(1.0, 1.0 + pan);
        const double gain = velocityAmp * 0.35;

        for (int i = chunk; i < chunk + n; ++i)
        {
            double l = 0.0, r = 0.0;
            for (std::size_t s = 0; s < static_cast<std::size_t>(stack); ++s)
            {
                const double ratio = std::exp2(stackDetune[s] / 12.0);
                const double d1 = std::min(0.45, dt1 * ratio), d2 = std::min(0.45, dt2 * ratio);
                double& p1 = v.osc1Phase[s];
                double& p2 = v.osc2Phase[s];
                double o1 = osc1Wave(shape1, p1, shapeAmount);
                // Band-limit the jumps of the saw-like and pulse shapes.
                if (shape1 == Osc1Shape::saw)
                    o1 -= (1.0 - 0.5 * shapeAmount) * dsp::polyBlep(p1, d1) + 0.5 * shapeAmount * dsp::polyBlep(fraction(2.0 * p1), std::min(0.45, 2.0 * d1));
                else if (shape1 == Osc1Shape::sharkTooth)
                    o1 -= dsp::polyBlep(p1, d1);
                else if (shape1 == Osc1Shape::pulse)
                    o1 += dsp::polyBlep(p1, d1) - dsp::polyBlep(fraction(p1 - (0.5 - 0.45 * shapeAmount) + 1.0), d1);
                const double o2 = osc2Sample(shape2, p2, d2);
                p1 = fraction(p1 + d1);
                p2 = fraction(p2 + d2);
                const double voiceSample = o1 * level1 + o2 * level2;
                l += voiceSample * stackLeft[s];
                r += voiceSample * stackRight[s];
            }
            if (noiseLevel > 0.0)
            {
                const double noise = noise_.next() * noiseLevel * 0.5;
                l += noise;
                r += noise;
            }
            l = v.filters[0].process(l, filterMode);
            r = v.filters[1].process(r, filterMode);
            if (highPass)
            {
                l = v.highPass[0].highPass(l);
                r = v.highPass[1].highPass(r);
            }
            const double amp = v.env1.next() * gain;
            left[i] += l * amp * panLeft;
            right[i] += r * amp * panRight;
        }
        if (!v.env1.isActive())
            v.active = false;
    }
}

void DriftSynth::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept
{
    const int numSamples = buffer.getNumSamples();
    buffer.clear();
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);
    const auto transport = dsp::readTransport(getPlayHead());
    expressions_.setMpe(mpe_->get(), noteBendRange_->get(), bendRange_->get());
    freeLfoPhase_ += numSamples / sampleRate_ * static_cast<double>(lfoRate_->get());
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
