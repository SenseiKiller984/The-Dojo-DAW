#include "devices/Click.h"

#include "devices/dsp/Biquad.h"
#include "devices/dsp/SynthCore.h"

#include <cmath>

namespace dojo::devices
{

Click::Click()
    : BuiltinProcessor(kTypeId, "Click",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
    sound_ = addChoice("sound", "Sound", {"Beep", "Woodblock", "Rim", "Cowbell"}, 0);
    volume_ = addFloat("volume", "Volume", {-60.0f, 6.0f, 0.1f}, 0.0f, "dB");
    accent_ = addBool("accent", "Accent", true);
    subdivision_ = addChoice("subdivision", "Subdivision", {"Quarter", "Eighth", "Sixteenth"}, 0);
    when_ = addChoice("when", "Play", {"Play and Record", "Record Only"}, 0);
}

void Click::prepareDsp(double, int)
{
    voice_ = {};
    lastTick_ = -1;
}

void Click::trigger(bool accented) noexcept
{
    // Each sound: two partials, some noise and a decay; the accent is a fifth up and louder.
    struct Recipe
    {
        double frequency1, frequency2, partial2, noise, decay;
    };
    static constexpr std::array<Recipe, 4> recipes{{
        {1000.0, 0.0, 0.0, 0.0, 0.030},   // beep
        {1800.0, 2700.0, 0.4, 0.1, 0.012}, // woodblock
        {1700.0, 3400.0, 0.3, 0.6, 0.008}, // rim
        {540.0, 800.0, 0.8, 0.0, 0.080},   // cowbell
    }};
    const auto& recipe = recipes[static_cast<std::size_t>(juce::jlimit(0, 3, sound_->getIndex()))];
    const double pitch = accented ? 1.5 : 1.0;

    voice_.active = true;
    voice_.age = 0.0;
    voice_.phase1 = voice_.phase2 = 0.0;
    voice_.frequency1 = recipe.frequency1 * pitch;
    voice_.frequency2 = recipe.frequency2 * pitch;
    voice_.partial2 = recipe.partial2;
    voice_.noise = recipe.noise;
    voice_.decay = recipe.decay;
    voice_.gain = dsp::dbToGain(volume_->get()) * (accented ? 1.0 : dsp::dbToGain(-6.0)) * 0.8;
}

double Click::nextSample() noexcept
{
    if (!voice_.active)
        return 0.0;

    const double dt = 1.0 / sampleRate_;
    const double envelope = std::exp(-voice_.age / voice_.decay) * std::min(1.0, voice_.age / 0.0005); // 0.5 ms attack
    double value = std::sin(dsp::kTwoPi * voice_.phase1);
    if (voice_.partial2 > 0.0)
        value += voice_.partial2 * std::sin(dsp::kTwoPi * voice_.phase2);
    if (voice_.noise > 0.0)
        value += voice_.noise * (random_.nextDouble() * 2.0 - 1.0);
    value *= envelope * voice_.gain / (1.0 + voice_.partial2 + voice_.noise);

    voice_.phase1 = std::fmod(voice_.phase1 + voice_.frequency1 * dt, 1.0);
    voice_.phase2 = std::fmod(voice_.phase2 + voice_.frequency2 * dt, 1.0);
    voice_.age += dt;
    if (voice_.age > voice_.decay * 12.0) // ~ -100 dB
        voice_.active = false;
    return value;
}

void Click::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;

    // Where the transport is, and whether the click should sound now.
    bool playing = false;
    double ppq = 0.0;
    double ppqPerSample = 0.0;
    double barStart = 0.0;
    int beatsPerBar = 4;
    if (auto* head = getPlayHead())
        if (const auto info = head->getPosition())
        {
            const bool recording = info->getIsRecording();
            playing = info->getIsPlaying() && (when_->getIndex() == 0 || recording);
            ppq = info->getPpqPosition().orFallback(0.0);
            ppqPerSample = info->getBpm().orFallback(120.0) / 60.0 / sampleRate_;
            barStart = info->getPpqPositionOfLastBarStart().orFallback(std::floor(ppq / 4.0) * 4.0);
            if (const auto signature = info->getTimeSignature())
                beatsPerBar = std::max(1, signature->numerator);
        }

    const double ticksPerBeat = subdivision_->getIndex() == 2 ? 4.0 : subdivision_->getIndex() == 1 ? 2.0 : 1.0;
    if (!playing)
        lastTick_ = -1; // (a new start clicks on its first beat)

    for (int i = 0; i < numSamples; ++i)
    {
        if (playing)
        {
            // A tick starts at the first sample at or after each subdivision.
            const double position = (ppq + i * ppqPerSample) * ticksPerBeat;
            const double tick = std::ceil(position - 1.0e-9);
            const auto tickIndex = static_cast<long long>(tick);
            if (tick - position < ppqPerSample * ticksPerBeat && tickIndex != lastTick_ && tick >= 0.0)
            {
                lastTick_ = tickIndex;
                const double beat = tick / ticksPerBeat;
                const bool onBeat = std::abs(beat - std::round(beat)) < 1.0e-6;
                // The first beat of a bar (bars counted from the play head's last bar start).
                const auto beatsIntoBar = std::llround(beat - barStart) % beatsPerBar;
                const bool downbeat = onBeat && (beatsIntoBar + beatsPerBar) % beatsPerBar == 0;
                trigger(accent_->get() && downbeat);
                if (!onBeat)
                    voice_.gain *= dsp::dbToGain(-6.0); // offbeats softer still
            }
        }

        const double value = nextSample();
        left[i] += value;
        if (right != nullptr)
            right[i] += value;
    }
}

} // namespace dojo::devices
