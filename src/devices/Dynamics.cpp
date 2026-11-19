#include "devices/Dynamics.h"

#include "devices/dsp/Biquad.h"
#include "devices/dsp/SynthCore.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices
{

//==================================================================================================
// Compressor

Compressor::Compressor()
    : BuiltinProcessor(kTypeId, "Compressor",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true)
                           .withInput("Sidechain", juce::AudioChannelSet::stereo(), true),
                       false)
{
    threshold_ = addFloat("threshold", "Threshold", {-60.0f, 0.0f, 0.1f}, -18.0f, "dB");
    ratio_ = addFloat("ratio", "Ratio", skewedRange(1.0f, 20.0f, 4.0f), 4.0f, ":1");
    attack_ = addFloat("attack", "Attack", skewedRange(0.1f, 100.0f, 10.0f), 10.0f, "ms");
    release_ = addFloat("release", "Release", skewedRange(5.0f, 1000.0f, 120.0f), 120.0f, "ms");
    knee_ = addFloat("knee", "Knee", {0.0f, 24.0f, 0.1f}, 6.0f, "dB");
    makeup_ = addFloat("makeup", "Makeup", {0.0f, 24.0f, 0.1f}, 0.0f, "dB");
    mix_ = addFloat("mix", "Mix", {0.0f, 100.0f, 0.1f}, 100.0f, "%");
    external_ = addBool("sidechain", "External Sidechain", false);
}

double Compressor::gainComputer(double levelDb, double threshold, double ratio, double knee) noexcept
{
    const double over = levelDb - threshold;
    const double slope = 1.0 / ratio - 1.0; // <= 0
    if (knee > 0.0 && std::abs(over) <= knee * 0.5)
    {
        const double x = over + knee * 0.5;
        return slope * x * x / (2.0 * knee);
    }
    return over > 0.0 ? slope * over : 0.0;
}

double Compressor::getOutputLevelDb(double inputDb) const
{
    return inputDb + gainComputer(inputDb, threshold_->get(), ratio_->get(), knee_->get()) + makeup_->get();
}

void Compressor::prepareDsp(double, int)
{
    envelopeDb_ = 0.0;
}

void Compressor::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);

    const bool external = external_->get() && buffer.getNumChannels() >= 4;
    const double* detectLeft = external ? buffer.getReadPointer(2) : left;
    const double* detectRight = external ? buffer.getReadPointer(3) : right;

    const double threshold = threshold_->get();
    const double ratio = std::max(1.0, static_cast<double>(ratio_->get()));
    const double knee = knee_->get();
    const double makeup = makeup_->get();
    const double wet = mix_->get() / 100.0;
    const double attack = dsp::onePoleCoefficient(attack_->get() / 1000.0, sampleRate_);
    const double release = dsp::onePoleCoefficient(release_->get() / 1000.0, sampleRate_);

    double maxReduction = 0.0;
    for (int i = 0; i < numSamples; ++i)
    {
        const double level = std::max(std::abs(detectLeft[i]), std::abs(detectRight[i]));
        const double levelDb = dsp::gainToDb(level);
        const double target = gainComputer(levelDb, threshold, ratio, knee);

        const double coefficient = target < envelopeDb_ ? attack : release;
        envelopeDb_ = target + (envelopeDb_ - target) * coefficient;
        maxReduction = std::min(maxReduction, envelopeDb_);

        const double gain = dsp::dbToGain(envelopeDb_ + makeup);
        left[i] = left[i] * (1.0 - wet) + left[i] * gain * wet;
        right[i] = right[i] * (1.0 - wet) + right[i] * gain * wet;
    }

    const auto reduction = static_cast<float>(-maxReduction);
    float previous = gainReduction_.load(std::memory_order_relaxed);
    while (reduction > previous && !gainReduction_.compare_exchange_weak(previous, reduction, std::memory_order_acq_rel))
    {
    }
}

//==================================================================================================
// Limiter

Limiter::Limiter()
    : BuiltinProcessor(kTypeId, "Limiter",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
    // The first four are the original parameters (kept first, in order, for older projects).
    input_ = addFloat("input", "Input Gain", {0.0f, 24.0f, 0.1f}, 0.0f, "dB");
    ceiling_ = addFloat("ceiling", "Ceiling", {-24.0f, 0.0f, 0.1f}, -0.3f, "dB");
    release_ = addFloat("release", "Release", skewedRange(1.0f, 1000.0f, 80.0f), 80.0f, "ms");
    lookahead_ = addFloat("lookahead", "Lookahead", {0.1f, static_cast<float>(kMaxLookaheadMs), 0.1f}, 5.0f, "ms",
                          false); // changes the latency, so it is not automatable
    maximize_ = addBool("maximize", "Maximize", false);
    threshold_ = addFloat("threshold", "Threshold", {-30.0f, 0.0f, 0.1f}, 0.0f, "dB");
    autoRelease_ = addBool("autorelease", "Auto Release", false);
    mode_ = addChoice("mode", "Mode", {"Standard", "Soft Clip", "True Peak"}, 0, false); // True Peak adds latency
    channels_ = addChoice("channels", "Channels", {"Stereo", "L/R"}, 0);
    link_ = addFloat("link", "Link", {0.0f, 100.0f, 1.0f}, 0.0f, "%");
    lookahead_->addListener(this);
    mode_->addListener(this);

    // True Peak: an 8x interpolator (finer than the 4x of ITU-R BS.1770 meters, so the peaks it
    // misses are smaller). For each of the seven points between two samples, kTruePeakTaps taps of
    // a Kaiser-windowed sinc, normalised to unity gain at DC. Tap 0 is the oldest sample; the points
    // lie between taps kTruePeakTaps - 1 - kTruePeakDelay and kTruePeakTaps - kTruePeakDelay.
    // Up to 20 kHz (at 48 kHz) it reads tones within 0.1 dB.
    const auto besselI0 = [](double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 40; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    };
    constexpr double beta = 6.0;
    for (std::size_t point = 0; point < interpolator_.size(); ++point)
    {
        auto& taps = interpolator_[point];
        double sum = 0.0;
        for (std::size_t tap = 0; tap < taps.size(); ++tap)
        {
            // Distance (in samples) from the tap to the point; never a whole number.
            const double distance = static_cast<double>(kTruePeakTaps - 1 - kTruePeakDelay)
                                    + static_cast<double>(point + 1) / kTruePeakOversampling - static_cast<double>(tap);
            const double sinc = std::sin(dsp::kPi * distance) / (dsp::kPi * distance);
            const double x = distance / kTruePeakDelay; // |x| < 1
            const double window = besselI0(beta * std::sqrt(1.0 - x * x)) / besselI0(beta);
            taps[tap] = sinc * window;
            sum += taps[tap];
        }
        for (auto& coefficient : taps)
            coefficient /= sum;
    }
}

Limiter::~Limiter()
{
    lookahead_->removeListener(this);
    mode_->removeListener(this);
}

double Limiter::softClip(double x, double ceiling) noexcept
{
    const double u = std::abs(x) / ceiling;
    if (u <= kSoftClipKnee)
        return x;
    const double shaped = kSoftClipKnee + (1.0 - kSoftClipKnee) * std::tanh((u - kSoftClipKnee) / (1.0 - kSoftClipKnee));
    return std::copysign(shaped * ceiling, x);
}

int Limiter::lookaheadSamples() const noexcept
{
    return std::clamp(static_cast<int>(std::lround(lookahead_->get() / 1000.0 * sampleRate_)), 1,
                      std::max(1, capacity_ - kTruePeakTaps - 4));
}

void Limiter::parameterValueChanged(int, float)
{
    // Only the lookahead and the mode are listened to. Reporting the new latency lets the engine re-align.
    setLatencySamples(lookaheadSamples() + detectorDelay());
}

void Limiter::GainComputer::prepare(int capacity)
{
    required.assign(static_cast<std::size_t>(capacity), 1.0);
    index.assign(static_cast<std::size_t>(capacity), 0);
    averageRing.assign(static_cast<std::size_t>(capacity), 1.0);
}

void Limiter::GainComputer::reset(int lookahead) noexcept
{
    head = tail = 0;
    std::fill(averageRing.begin(), averageRing.end(), 1.0);
    averageIndex = 0;
    averageSum = static_cast<double>(lookahead);
    gain = fast = slow = 1.0;
}

double Limiter::GainComputer::process(double needed, long long n, int lookahead, double release, bool autoRelease,
                                      double fastRelease, double slowAttack, double slowRelease) noexcept
{
    const int cap = static_cast<int>(required.size());

    // Sliding minimum of the required gain over the last lookahead + 1 samples (a monotonic deque).
    while (tail != head)
    {
        const int back = (tail - 1 + cap) % cap;
        if (required[static_cast<std::size_t>(back)] < needed)
            break;
        tail = back;
    }
    required[static_cast<std::size_t>(tail)] = needed;
    index[static_cast<std::size_t>(tail)] = n;
    tail = (tail + 1) % cap;
    while (n - index[static_cast<std::size_t>(head)] > lookahead)
        head = (head + 1) % cap;
    const double held = required[static_cast<std::size_t>(head)];

    // Average the held gain over the lookahead: the ramp reaches the held value as the peak arrives.
    averageSum += held - averageRing[static_cast<std::size_t>(averageIndex)];
    averageRing[static_cast<std::size_t>(averageIndex)] = held;
    averageIndex = (averageIndex + 1) % lookahead;
    const double smoothed = std::min(1.0, averageSum / lookahead);

    if (autoRelease)
    {
        // Two release paths: a fast one that recovers quickly from short peaks, and a slow one that
        // follows it (with an attack of its own) and so only goes down under sustained limiting.
        // The gain is the lower of the two: quick after transients, smooth (no pumping) on dense material.
        fast = smoothed < fast ? smoothed : smoothed + (fast - smoothed) * fastRelease;
        slow = fast + (slow - fast) * (fast < slow ? slowAttack : slowRelease);
        gain = std::min(fast, slow);
    }
    else
    {
        gain = smoothed < gain ? smoothed : smoothed + (gain - smoothed) * release;
        fast = slow = gain;
    }
    return gain;
}

void Limiter::prepareDsp(double sampleRate, int)
{
    capacity_ = static_cast<int>(std::ceil(kMaxLookaheadMs / 1000.0 * sampleRate)) + kTruePeakTaps + 8;
    for (auto& d : delay_)
        d.assign(static_cast<std::size_t>(capacity_), 0.0);
    for (auto& computer : computers_)
        computer.prepare(capacity_);
    resetState(lookaheadSamples(), detectorDelay());
    setLatencySamples(lookaheadSamples_ + detectorDelay_);
}

void Limiter::resetState(int lookahead, int delay) noexcept
{
    lookaheadSamples_ = lookahead;
    detectorDelay_ = delay;
    for (auto& d : delay_)
        std::fill(d.begin(), d.end(), 0.0);
    delayIndex_ = 0;
    sampleCounter_ = 0;
    for (auto& computer : computers_)
        computer.reset(lookahead);
    for (auto& h : history_)
        h.fill(0.0);
    historyIndex_ = 0;
    previousInterSamplePeak_ = {};
}

double Limiter::interSamplePeak(int channel) const noexcept
{
    // history_ is a ring with the newest sample at historyIndex_, so the oldest follows it.
    const auto& h = history_[static_cast<std::size_t>(channel)];
    double peak = 0.0;
    for (const auto& taps : interpolator_)
    {
        double value = 0.0;
        for (int tap = 0; tap < kTruePeakTaps; ++tap)
            value += h[static_cast<std::size_t>((historyIndex_ + 1 + tap) % kTruePeakTaps)] * taps[static_cast<std::size_t>(tap)];
        peak = std::max(peak, std::abs(value));
    }
    return peak;
}

void Limiter::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    if (const int wanted = lookaheadSamples(), delay = detectorDelay(); wanted != lookaheadSamples_ || delay != detectorDelay_)
        resetState(wanted, delay);

    const int numSamples = buffer.getNumSamples();
    const int numChannels = std::min(2, buffer.getNumChannels());
    if (numChannels == 0)
        return;

    const int L = lookaheadSamples_;
    const int totalDelay = L + detectorDelay_; // the gain computer ramps over L samples after the detector
    const int cap = capacity_;
    const bool truePeak = detectorDelay_ > 0;
    const bool soft = mode_->getIndex() == static_cast<int>(Mode::softClip);

    const double ceilingDb = ceiling_->get();
    const double ceiling = dsp::dbToGain(ceilingDb);
    // Maximize: material at the threshold comes out at the ceiling.
    const double drive = dsp::dbToGain(maximize_->get() ? ceilingDb - threshold_->get() : input_->get());
    // Soft Clip lets peaks through up to kSoftClipHeadroomDb over the ceiling; the clipper rounds them
    // off. True Peak keeps a hair of margin for what the interpolator cannot see (content right at
    // the top of the spectrum) and for the gain moving between samples.
    const double target = soft       ? ceiling * dsp::dbToGain(kSoftClipHeadroomDb)
                          : truePeak ? ceiling * dsp::dbToGain(-kTruePeakMarginDb)
                                     : ceiling;
    const double link = channels_->getIndex() == 1 ? link_->get() / 100.0 : 1.0;
    const bool autoRelease = autoRelease_->get();
    const double release = dsp::onePoleCoefficient(release_->get() / 1000.0, sampleRate_);
    const double fastRelease = dsp::onePoleCoefficient(kAutoFastReleaseMs / 1000.0, sampleRate_);
    const double slowAttack = dsp::onePoleCoefficient(kAutoSlowAttackMs / 1000.0, sampleRate_);
    const double slowRelease = dsp::onePoleCoefficient(kAutoSlowReleaseMs / 1000.0, sampleRate_);

    std::array<double*, 2> data{buffer.getWritePointer(0), buffer.getWritePointer(numChannels - 1)};
    double minGain = 1.0;

    for (int i = 0; i < numSamples; ++i)
    {
        std::array<double, 2> in{};
        std::array<double, 2> needed{1.0, 1.0};
        if (truePeak)
            historyIndex_ = (historyIndex_ + 1) % kTruePeakTaps;

        for (int c = 0; c < numChannels; ++c)
        {
            const auto ch = static_cast<std::size_t>(c);
            in[ch] = data[ch][i] * drive;
            double peak = std::abs(in[ch]);
            if (truePeak)
            {
                // The detector looks at the sample kTruePeakDelay back and at the interpolated points
                // on both sides of it.
                auto& h = history_[ch];
                h[static_cast<std::size_t>(historyIndex_)] = in[ch];
                const double between = interSamplePeak(c);
                const double sample = std::abs(h[static_cast<std::size_t>((historyIndex_ + kTruePeakTaps - kTruePeakDelay) % kTruePeakTaps)]);
                peak = std::max({sample, between, previousInterSamplePeak_[ch]});
                previousInterSamplePeak_[ch] = between;
            }
            needed[ch] = peak > target ? target / peak : 1.0;
        }

        if (numChannels == 2 && link > 0.0)
        {
            // Stereo (or Link 100 %): both channels get the louder one's gain. In between, blend the
            // two in dB; each channel is still reduced at least as much as it needs.
            const double linked = std::min(needed[0], needed[1]);
            for (auto& value : needed)
                value = link >= 1.0 ? linked
                                    : std::min(value, std::exp((1.0 - link) * std::log(std::max(value, 1.0e-12))
                                                               + link * std::log(std::max(linked, 1.0e-12))));
        }

        const long long n = sampleCounter_++;
        const auto readIndex = static_cast<std::size_t>((delayIndex_ - totalDelay + cap) % cap);
        for (int c = 0; c < numChannels; ++c)
        {
            const auto ch = static_cast<std::size_t>(c);
            const double gain = computers_[ch].process(needed[ch], n, L, release, autoRelease, fastRelease, slowAttack, slowRelease);
            minGain = std::min(minGain, gain);

            auto& line = delay_[ch];
            const double delayed = line[readIndex];
            line[static_cast<std::size_t>(delayIndex_)] = in[ch];

            // The final clamp guards against rounding at the ceiling.
            const double out = delayed * gain;
            data[ch][i] = soft ? softClip(out, ceiling) : std::clamp(out, -ceiling, ceiling);
        }
        delayIndex_ = (delayIndex_ + 1) % cap;
    }

    const auto reduction = static_cast<float>(-dsp::gainToDb(minGain));
    float previous = gainReduction_.load(std::memory_order_relaxed);
    while (reduction > previous && !gainReduction_.compare_exchange_weak(previous, reduction, std::memory_order_acq_rel))
    {
    }
}

} // namespace dojo::devices
