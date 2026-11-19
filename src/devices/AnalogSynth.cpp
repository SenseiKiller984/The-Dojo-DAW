#include "devices/AnalogSynth.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices
{
namespace
{
struct FilterSetting
{
    dsp::FilterMode mode;
    dsp::FilterCircuit circuit;
    bool steep;
};

/** LP12, LP24, BP12, BP24, HP12, HP24, Notch: 12 dB types are OTA state-variable filters, 24 dB
    types transistor ladders. */
FilterSetting filterSetting(int type) noexcept
{
    switch (type)
    {
    case 1: return {dsp::FilterMode::lowPass, dsp::FilterCircuit::ladder, true};
    case 2: return {dsp::FilterMode::bandPass, dsp::FilterCircuit::warm, false};
    case 3: return {dsp::FilterMode::bandPass, dsp::FilterCircuit::ladder, true};
    case 4: return {dsp::FilterMode::highPass, dsp::FilterCircuit::warm, false};
    case 5: return {dsp::FilterMode::highPass, dsp::FilterCircuit::ladder, true};
    case 6: return {dsp::FilterMode::notch, dsp::FilterCircuit::warm, false};
    default: return {dsp::FilterMode::lowPass, dsp::FilterCircuit::warm, false};
    }
}

constexpr std::array<int, 5> kPolyphony{1, 2, 4, 8, 16};
} // namespace

AnalogSynth::AnalogSynth()
    : BuiltinProcessor(kTypeId, "Analog", BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true), true)
{
    for (std::size_t o = 0; o < oscillators_.size(); ++o)
    {
        const juce::String id = "o" + juce::String(o + 1);
        const juce::String name = "Osc " + juce::String(o + 1) + " ";
        auto& osc = oscillators_[o];
        osc.on = addBool(id + "on", name + "On", true);
        osc.shape = addChoice(id + "shape", name + "Shape", {"Sine", "Saw", "Rectangle", "Noise"}, o == 0 ? 1 : 2);
        osc.octave = addFloat(id + "oct", name + "Octave", {-3.0f, 3.0f, 1.0f}, 0.0f);
        osc.semi = addFloat(id + "semi", name + "Semi", {-12.0f, 12.0f, 1.0f}, 0.0f, "st");
        osc.detune = addFloat(id + "detune", name + "Detune", {-100.0f, 100.0f, 0.1f}, o == 0 ? 0.0f : 8.0f, "ct");
        osc.width = addFloat(id + "pw", name + "Pulse Width", {5.0f, 95.0f, 0.1f}, 50.0f, "%");
        osc.pwm = addFloat(id + "pwm", name + "PWM", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
        osc.lfoPitch = addFloat(id + "lfo", name + "LFO Pitch", skewedRange(0.0f, 12.0f, 1.0f, 0.01f), 0.0f, "st");
        osc.level = addFloat(id + "level", name + "Level", {0.0f, 100.0f, 0.1f}, o == 0 ? 80.0f : 50.0f, "%");
        osc.toFilter2 = addFloat(id + "f2", name + "To Filter 2", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    }
    sync_ = addBool("o2sync", "Osc 2 Sync", false);
    noise_ = addFloat("noise", "Noise", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    noiseColour_ = addFloat("noisecolor", "Noise Colour", skewedRange(100.0f, 20000.0f, 3000.0f), 20000.0f, "Hz");
    noiseToFilter2_ = addFloat("noisef2", "Noise To Filter 2", {0.0f, 100.0f, 0.1f}, 0.0f, "%");

    const juce::StringArray filterTypes{"LP12", "LP24", "BP12", "BP24", "HP12", "HP24", "Notch"};
    for (std::size_t f = 0; f < filters_.size(); ++f)
    {
        const juce::String id = "f" + juce::String(f + 1);
        const juce::String name = "Filter " + juce::String(f + 1) + " ";
        auto& filter = filters_[f];
        filter.type = addChoice(id + "type", name + "Type", filterTypes, 1);
        filter.cutoff = addFloat(id + "cutoff", name + "Cutoff", skewedRange(20.0f, 20000.0f, 1500.0f), f == 0 ? 1200.0f : 3000.0f, "Hz");
        filter.resonance = addFloat(id + "res", name + "Resonance", {0.0f, 100.0f, 0.1f}, f == 0 ? 25.0f : 10.0f, "%");
        filter.drive = addFloat(id + "drive", name + "Drive", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
        filter.keyTrack = addFloat(id + "key", name + "Key Track", {0.0f, 100.0f, 0.1f}, 50.0f, "%");
        filter.envelope = addFloat(id + "env", name + "Env Amount", {-100.0f, 100.0f, 0.1f}, f == 0 ? 40.0f : 0.0f, "%");
        filter.lfo = addFloat(id + "lfo", name + "LFO Amount", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    }
    routing_ = addChoice("route", "Filter Routing", {"Parallel", "Serial"}, 0);

    const auto addAdsr = [this](Adsr& adsr, const juce::String& id, const juce::String& name, std::array<float, 4> defaults)
    {
        adsr.attack = addFloat(id + "a", name + " Attack", skewedRange(0.5f, 10000.0f, 200.0f), defaults[0], "ms");
        adsr.decay = addFloat(id + "d", name + " Decay", skewedRange(1.0f, 20000.0f, 500.0f), defaults[1], "ms");
        adsr.sustain = addFloat(id + "s", name + " Sustain", {0.0f, 100.0f, 0.1f}, defaults[2], "%");
        adsr.release = addFloat(id + "r", name + " Release", skewedRange(1.0f, 20000.0f, 500.0f), defaults[3], "ms");
    };
    addAdsr(filterEnvelopes_[0], "fe1", "Filter Env 1", {2.0f, 400.0f, 0.0f, 300.0f});
    addAdsr(filterEnvelopes_[1], "fe2", "Filter Env 2", {5.0f, 600.0f, 30.0f, 400.0f});
    addAdsr(ampEnvelopes_[0], "ae1", "Amp Env 1", {2.0f, 300.0f, 80.0f, 300.0f});
    addAdsr(ampEnvelopes_[1], "ae2", "Amp Env 2", {2.0f, 300.0f, 80.0f, 300.0f});
    for (std::size_t a = 0; a < amps_.size(); ++a)
    {
        const juce::String id = "a" + juce::String(a + 1);
        const juce::String name = "Amp " + juce::String(a + 1) + " ";
        amps_[a].level = addFloat(id + "level", name + "Level", {0.0f, 100.0f, 0.1f}, 100.0f, "%");
        amps_[a].pan = addFloat(id + "pan", name + "Pan", {-100.0f, 100.0f, 0.1f}, 0.0f, "%");
        amps_[a].tremolo = addFloat(id + "trem", name + "Tremolo", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    }
    for (std::size_t l = 0; l < lfos_.size(); ++l)
    {
        const juce::String id = "l" + juce::String(l + 1);
        const juce::String name = "LFO " + juce::String(l + 1) + " ";
        lfos_[l].shape = addChoice(id + "shape", name + "Shape", {"Sine", "Triangle", "Saw", "Square", "S&H"}, l == 0 ? 0 : 1);
        lfos_[l].rate = addFloat(id + "rate", name + "Rate", skewedRange(0.01f, 30.0f, 2.0f), l == 0 ? 5.0f : 0.5f, "Hz");
        lfos_[l].sync = addBool(id + "sync", name + "Sync", false);
        lfos_[l].division = addChoice(id + "div", name + "Division", dsp::syncDivisionNames(), 9);
    }
    polyphony_ = addChoice("voices", "Voices", {"Mono", "2", "4", "8", "16"}, 3);
    legato_ = addBool("legato", "Legato", false);
    glide_ = addFloat("glide", "Glide", skewedRange(0.0f, 2000.0f, 200.0f), 0.0f, "ms");
    unison_ = addChoice("unison", "Unison", {"1", "2", "3", "4"}, 0);
    unisonDetune_ = addFloat("unidetune", "Unison Detune", {0.0f, 100.0f, 0.1f}, 20.0f, "%");
    vibratoAmount_ = addFloat("vibamt", "Vibrato", {0.0f, 100.0f, 0.1f}, 0.0f, "ct");
    vibratoRate_ = addFloat("vibrate", "Vibrato Rate", {1.0f, 12.0f, 0.01f}, 5.5f, "Hz");
    vibratoDelay_ = addFloat("vibdelay", "Vibrato Delay", skewedRange(0.0f, 2000.0f, 300.0f), 300.0f, "ms");
    velocityToAmp_ = addFloat("velamp", "Velocity > Amp", {0.0f, 100.0f, 0.1f}, 60.0f, "%");
    velocityToFilter_ = addFloat("velfilter", "Velocity > Filter", {0.0f, 100.0f, 0.1f}, 30.0f, "%");
    drift_ = addFloat("drift", "Drift", {0.0f, 100.0f, 0.1f}, 20.0f, "%");
    bendRange_ = addFloat("bend", "Bend Range", {0.0f, 24.0f, 1.0f}, 2.0f, "st");
    volume_ = addFloat("volume", "Volume", {-36.0f, 6.0f, 0.1f}, -6.0f, "dB");
}

int AnalogSynth::polyphony() const noexcept
{
    return kPolyphony[static_cast<std::size_t>(std::clamp(polyphony_->getIndex(), 0, 4))];
}

void AnalogSynth::prepareDsp(double, int)
{
    for (auto& v : voices_)
    {
        v.active = false;
        for (auto& f : v.filters)
            f.reset();
    }
    expressions_.reset();
    held_.clear();
    sustainPedal_ = false;
    lastPitch_ = -1.0;
    activeVoices_.store(0);
}

void AnalogSynth::updateEnvelopes(Voice& voice) noexcept
{
    for (std::size_t e = 0; e < 2; ++e)
    {
        const auto& f = filterEnvelopes_[e];
        voice.filterEnvelopes[e].setParameters(f.attack->get() / 1000.0, f.decay->get() / 1000.0, f.sustain->get() / 100.0,
                                               f.release->get() / 1000.0, sampleRate_);
        const auto& a = ampEnvelopes_[e];
        voice.ampEnvelopes[e].setParameters(a.attack->get() / 1000.0, a.decay->get() / 1000.0, a.sustain->get() / 100.0,
                                            a.release->get() / 1000.0, sampleRate_);
    }
}

void AnalogSynth::startNote(int note, int channel, double velocity) noexcept
{
    expressions_.noteOn(note);
    const bool mono = polyphony() == 1;
    Voice* chosen = nullptr;
    bool retrigger = true;
    if (mono)
    {
        const bool keyHeld = !held_.empty();
        held_.push(note, channel, velocity);
        chosen = &voices_[0];
        retrigger = !(legato_->get() && keyHeld && chosen->active);
    }
    else
    {
        chosen = &dsp::pickVoice(voices_, polyphony(), note, channel, [](const Voice& v) { return v.ampEnvelopes[0].isReleasing(); });
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
        return; // legato: glide to the new note, envelopes carry on

    v.velocity = velocity;
    v.age = ++ageCounter_;
    v.seconds = 0.0;
    v.pitch = glide_->get() > 0.0f && lastPitch_ >= 0.0 ? lastPitch_ : static_cast<double>(note);
    lastPitch_ = note;
    if (!wasActive)
    {
        for (auto& f : v.filters)
            f.reset();
        for (auto& e : v.filterEnvelopes)
            e.reset();
        for (auto& e : v.ampEnvelopes)
            e.reset();
        for (auto& lfo : v.lfos)
            lfo.reset();
        for (auto& oscillator : v.oscillators)
            for (std::size_t u = 0; u < oscillator.size(); ++u)
                oscillator[u].reset(u == 0 ? 0.0 : 0.5 * (random_.next() + 1.0));
        v.driftOffset = random_.next();
        v.driftPhase = 0.5 * (random_.next() + 1.0);
    }
    updateEnvelopes(v);
    for (auto& e : v.filterEnvelopes)
        e.noteOn();
    for (auto& e : v.ampEnvelopes)
        e.noteOn();
}

void AnalogSynth::releaseNote(int note, int channel) noexcept
{
    if (polyphony() == 1)
    {
        held_.remove(note, channel);
        auto& v = voices_[0];
        if (!v.active)
            return;
        if (!held_.empty())
        {
            // Back to the most recent key still held.
            const auto top = held_.top();
            if (top.note != v.note || top.channel != v.channel)
            {
                v.note = top.note;
                v.channel = top.channel;
                v.targetPitch = top.note;
                lastPitch_ = top.note;
                if (!legato_->get())
                {
                    for (auto& e : v.filterEnvelopes)
                        e.noteOn();
                    for (auto& e : v.ampEnvelopes)
                        e.noteOn();
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
            for (auto& e : v.filterEnvelopes)
                e.noteOff();
            for (auto& e : v.ampEnvelopes)
                e.noteOff();
        }
}

void AnalogSynth::handleMidi(const juce::MidiMessage& message) noexcept
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
                for (auto& e : v.filterEnvelopes)
                    e.noteOff();
                for (auto& e : v.ampEnvelopes)
                    e.noteOff();
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
                for (auto& e : v.ampEnvelopes)
                    e.noteOff();
                for (auto& e : v.filterEnvelopes)
                    e.noteOff();
            }
    }
}

void AnalogSynth::renderVoice(Voice& v, double* left, double* right, int start, int end, const dsp::TransportState& transport) noexcept
{
    constexpr std::array<double, kMaxUnison> unisonOffsets{0.0, 1.0, -1.0, 0.5};
    const int unison = std::clamp(unison_->getIndex() + 1, 1, kMaxUnison);
    const double unisonScale = 1.0 / std::sqrt(static_cast<double>(unison));
    const double detuneCents = unisonDetune_->get() * 0.25; // up to +-25 cents
    const bool serial = routing_->getIndex() == 1;
    const bool sync = sync_->get();
    const double glideSeconds = glide_->get() / 1000.0;
    const double velocityAmp = 1.0 - velocityToAmp_->get() / 100.0 * (1.0 - v.velocity);
    const double velocityFilter = 1.0 - velocityToFilter_->get() / 100.0 * (1.0 - v.velocity);
    const double drift = drift_->get() / 100.0;
    const double noiseLevel = noise_->get() / 100.0;
    const double noiseSplit = noiseToFilter2_->get() / 100.0;
    v.noiseColour.setFrequency(noiseColour_->get(), sampleRate_);

    std::array<dsp::BlepOscillator::Wave, 2> waves{};
    std::array<bool, 2> isNoise{}, on{};
    std::array<double, 2> level{}, split{};
    for (std::size_t o = 0; o < 2; ++o)
    {
        const int shape = oscillators_[o].shape->getIndex();
        waves[o] = shape == 0 ? dsp::BlepOscillator::Wave::sine : shape == 1 ? dsp::BlepOscillator::Wave::saw : dsp::BlepOscillator::Wave::pulse;
        isNoise[o] = shape == 3;
        on[o] = oscillators_[o].on->get();
        level[o] = oscillators_[o].level->get() / 100.0 * unisonScale;
        split[o] = oscillators_[o].toFilter2->get() / 100.0;
    }
    std::array<FilterSetting, 2> filterTypes{filterSetting(filters_[0].type->getIndex()), filterSetting(filters_[1].type->getIndex())};
    std::array<double, 2> driveGain{};
    for (std::size_t f = 0; f < 2; ++f)
        driveGain[f] = 1.0 + filters_[f].drive->get() / 100.0 * 3.0;

    for (int chunk = start; chunk < end && v.active; chunk += kControlInterval)
    {
        const int n = std::min(kControlInterval, end - chunk);
        const double seconds = n / sampleRate_;
        v.seconds += seconds;

        // LFOs (per voice).
        std::array<double, 2> lfo{};
        for (std::size_t l = 0; l < 2; ++l)
        {
            const double rate = lfos_[l].sync->get() ? transport.bpm / 60.0 / dsp::syncDivisionBeats(lfos_[l].division->getIndex())
                                                     : static_cast<double>(lfos_[l].rate->get());
            lfo[l] = v.lfos[l].advance(rate, n, sampleRate_, static_cast<dsp::Lfo::Shape>(std::clamp(lfos_[l].shape->getIndex(), 0, 4)));
        }

        // Pitch: glide, bend, delayed vibrato, drift.
        v.pitch = glideSeconds > 0.0 ? v.targetPitch + (v.pitch - v.targetPitch) * std::exp(-4.6 * seconds / glideSeconds) : v.targetPitch;
        const double vibratoRamp = std::clamp((v.seconds - vibratoDelay_->get() / 1000.0) / 0.3, 0.0, 1.0);
        const double vibrato = vibratoAmount_->get() / 100.0 * vibratoRamp * std::sin(dsp::kTwoPi * vibratoRate_->get() * v.seconds);
        v.driftPhase += seconds * 0.13;
        const double driftCents = drift * (8.0 * v.driftOffset + 5.0 * std::sin(dsp::kTwoPi * v.driftPhase));
        const double common = v.pitch + expressions_.bend(v.channel) + vibrato + driftCents / 100.0;

        std::array<double, 2> dt{}, width{};
        for (std::size_t o = 0; o < 2; ++o)
        {
            const auto& p = oscillators_[o];
            const double pitch = common + p.octave->get() * 12.0 + p.semi->get() + p.detune->get() / 100.0 + lfo[0] * p.lfoPitch->get();
            dt[o] = dsp::noteToFrequency(pitch) / sampleRate_;
            width[o] = std::clamp(p.width->get() / 100.0 + p.pwm->get() / 100.0 * lfo[0] * 0.45, 0.05, 0.95);
        }

        // Filters: cutoff with envelope, LFO 2 and key tracking.
        for (std::size_t f = 0; f < 2; ++f)
        {
            const auto& p = filters_[f];
            const double envelope = v.filterEnvelopes[f].advance(n) * velocityFilter;
            const double octaves = envelope * p.envelope->get() / 100.0 * 6.0 + lfo[1] * p.lfo->get() / 100.0 * 3.0 +
                                   p.keyTrack->get() / 100.0 * (v.pitch - 60.0) / 12.0;
            const double cutoff = std::clamp(p.cutoff->get() * std::exp2(octaves), 20.0, std::min(20000.0, sampleRate_ * 0.45));
            v.filters[f].set(cutoff, p.resonance->get() / 100.0 * 0.98, sampleRate_, filterTypes[f].circuit, filterTypes[f].steep);
        }

        std::array<double, 2> ampLeft{}, ampRight{};
        for (std::size_t a = 0; a < 2; ++a)
        {
            const double tremolo = 1.0 - amps_[a].tremolo->get() / 100.0 * (1.0 - 0.5 * (lfo[1] + 1.0));
            const double gain = amps_[a].level->get() / 100.0 * tremolo * velocityAmp * 0.5;
            const double pan = amps_[a].pan->get() / 100.0;
            ampLeft[a] = gain * std::min(1.0, 1.0 - pan);
            ampRight[a] = gain * std::min(1.0, 1.0 + pan);
        }

        for (int i = chunk; i < chunk + n; ++i)
        {
            std::array<double, 2> osc{};
            for (std::size_t o = 0; o < 2; ++o)
            {
                // (with sync, oscillator 1 runs even when silent: it is oscillator 2's clock)
                const bool audible = on[o] && level[o] > 0.0;
                if (!audible && !(o == 0 && sync))
                    continue;
                double total = 0.0;
                if (isNoise[o])
                {
                    total = v.noise.next() * 0.6;
                }
                else
                {
                    for (int u = 0; u < unison; ++u)
                    {
                        const double ratio = std::exp2(unisonOffsets[static_cast<std::size_t>(u)] * detuneCents / 1200.0);
                        auto& oscillator = v.oscillators[o][static_cast<std::size_t>(u)];
                        total += oscillator.next(waves[o], dt[o] * ratio, width[o]);
                        if (o == 0 && sync && oscillator.justWrapped())
                            v.oscillators[1][static_cast<std::size_t>(u)].sync(oscillator.getPhase(), dt[0] * ratio, dt[1] * ratio);
                    }
                }
                osc[o] = audible ? total * level[o] : 0.0;
            }
            const double noise = noiseLevel > 0.0 ? v.noiseColour.lowPass(v.noise.next()) * noiseLevel * 0.6 : 0.0;
            const double in1 = osc[0] * (1.0 - split[0]) + osc[1] * (1.0 - split[1]) + noise * (1.0 - noiseSplit);
            const double in2 = osc[0] * split[0] + osc[1] * split[1] + noise * noiseSplit;

            const double y1 = v.filters[0].process(in1 * driveGain[0], filterTypes[0].mode) / std::sqrt(driveGain[0]);
            const double amp1 = v.ampEnvelopes[0].next();
            const double amp2 = v.ampEnvelopes[1].next();
            if (serial)
            {
                const double y2 = v.filters[1].process((y1 + in2) * driveGain[1], filterTypes[1].mode) / std::sqrt(driveGain[1]);
                left[i] += y2 * amp1 * ampLeft[0];
                right[i] += y2 * amp1 * ampRight[0];
            }
            else
            {
                const double y2 = v.filters[1].process(in2 * driveGain[1], filterTypes[1].mode) / std::sqrt(driveGain[1]);
                left[i] += y1 * amp1 * ampLeft[0] + y2 * amp2 * ampLeft[1];
                right[i] += y1 * amp1 * ampRight[0] + y2 * amp2 * ampRight[1];
            }
        }

        // (amplifier 2 counts only when filter 2 gets a signal)
        const bool usesSecond = !serial && (split[0] > 0.0 || split[1] > 0.0 || noiseSplit > 0.0);
        const bool sounding = v.ampEnvelopes[0].isActive() || (usesSecond && v.ampEnvelopes[1].isActive());
        if (!sounding)
            v.active = false;
    }
}

void AnalogSynth::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept
{
    const int numSamples = buffer.getNumSamples();
    buffer.clear();
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);
    const auto transport = dsp::readTransport(getPlayHead());
    expressions_.setMpe(false, 48.0, bendRange_->get());
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
