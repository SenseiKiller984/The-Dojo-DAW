#include "devices/Echo.h"

#include "devices/TimeEffects.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

double saturate(double x, double drive) noexcept
{
    // Unity gain for small signals; soft ceiling that keeps a 100 % feedback loop bounded.
    return std::tanh(x * drive) / drive;
}
} // namespace

//==================================================================================================
void Echo::SmallReverb::prepare(double sampleRate)
{
    constexpr std::array<int, 4> combLengths{1116, 1188, 1277, 1356};
    constexpr std::array<int, 2> allPassLengths{556, 441};
    const double scale = sampleRate / 44'100.0;
    for (std::size_t side = 0; side < 2; ++side)
    {
        const int spread = side == 0 ? 0 : 23;
        for (std::size_t i = 0; i < combs[side].size(); ++i)
            combs[side][i].data.assign(static_cast<std::size_t>(std::max(1.0, (combLengths[i] + spread) * scale)), 0.0);
        for (std::size_t i = 0; i < allPasses[side].size(); ++i)
            allPasses[side][i].data.assign(static_cast<std::size_t>(std::max(1.0, (allPassLengths[i] + spread) * scale)), 0.0);
    }
    reset();
}

void Echo::SmallReverb::reset() noexcept
{
    for (auto& side : combs)
        for (auto& comb : side)
        {
            std::fill(comb.data.begin(), comb.data.end(), 0.0);
            comb.index = 0;
            comb.store = 0.0;
        }
    for (auto& side : allPasses)
        for (auto& allPass : side)
        {
            std::fill(allPass.data.begin(), allPass.data.end(), 0.0);
            allPass.index = 0;
        }
}

void Echo::SmallReverb::process(double inLeft, double inRight, double feedback, double damping, double& outLeft,
                                double& outRight) noexcept
{
    const double input[2] = {inLeft, inRight};
    double output[2] = {0.0, 0.0};
    for (std::size_t side = 0; side < 2; ++side)
    {
        const double in = input[side] * 0.06;
        double sum = 0.0;
        for (auto& comb : combs[side])
        {
            auto& slot = comb.data[static_cast<std::size_t>(comb.index)];
            const double out = slot;
            comb.store = out * (1.0 - damping) + comb.store * damping;
            slot = in + comb.store * feedback;
            comb.index = (comb.index + 1) % static_cast<int>(comb.data.size());
            sum += out;
        }
        for (auto& allPass : allPasses[side])
        {
            auto& slot = allPass.data[static_cast<std::size_t>(allPass.index)];
            const double buffered = slot;
            const double out = buffered - sum;
            slot = sum + buffered * 0.5;
            allPass.index = (allPass.index + 1) % static_cast<int>(allPass.data.size());
            sum = out;
        }
        output[side] = sum;
    }
    outLeft = output[0];
    outRight = output[1];
}

double Echo::Svf::lowPass(double x, double g, double k) noexcept
{
    const double a1 = 1.0 / (1.0 + g * (g + k));
    const double a2 = g * a1;
    const double a3 = g * a2;
    const double v3 = x - ic2;
    const double v1 = a1 * ic1 + a2 * v3;
    const double v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.0 * v1 - ic1;
    ic2 = 2.0 * v2 - ic2;
    return v2;
}

//==================================================================================================
Echo::Echo()
    : BuiltinProcessor(kTypeId, "Echo",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
    const auto& divisions = Delay::divisionNames();
    mode_ = addChoice("mode", "Mode", {"Stereo", "Ping Pong", "Mid/Side"}, 0);
    sync_ = addBool("sync", "Sync", true);
    divisionLeft_ = addChoice("divl", "Left", divisions, 6);   // 1/8 dotted
    divisionRight_ = addChoice("divr", "Right", divisions, 5); // 1/8
    timeLeft_ = addFloat("timel", "Left Time", skewedRange(1.0f, static_cast<float>(kMaxSeconds * 1000.0), 300.0f), 375.0f, "ms");
    timeRight_ = addFloat("timer", "Right Time", skewedRange(1.0f, static_cast<float>(kMaxSeconds * 1000.0), 300.0f), 250.0f, "ms");
    link_ = addBool("link", "Link", true);
    feedback_ = addFloat("feedback", "Feedback", {0.0f, 100.0f, 0.1f}, 50.0f, "%");
    invert_ = addBool("invert", "Invert", false);
    input_ = addFloat("input", "Input", {-24.0f, 12.0f, 0.1f}, 0.0f, "dB");
    filter_ = addBool("filter", "Filter", true);
    highPass_ = addFloat("hp", "High Pass", skewedRange(20.0f, 2000.0f, 200.0f), 100.0f, "Hz");
    lowPass_ = addFloat("lp", "Low Pass", skewedRange(500.0f, 20000.0f, 4000.0f), 6000.0f, "Hz");
    resonance_ = addFloat("res", "Resonance", {0.0f, 100.0f, 0.1f}, 15.0f, "%");
    modRate_ = addFloat("modrate", "Mod Rate", skewedRange(0.05f, 10.0f, 1.0f), 0.5f, "Hz");
    modDepth_ = addFloat("moddepth", "Mod Time", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    modFilter_ = addFloat("modfilter", "Mod Filter", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    drive_ = addFloat("drive", "Drive", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    noise_ = addFloat("noise", "Noise", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    wobble_ = addFloat("wobble", "Wobble", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    duck_ = addFloat("duck", "Duck", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    reverb_ = addFloat("reverb", "Reverb", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    reverbDecay_ = addFloat("reverbdecay", "Rev Decay", {0.0f, 100.0f, 0.1f}, 50.0f, "%");
    reverbLocation_ = addChoice("reverbloc", "Rev Place", {"Pre", "Post", "Feedback"}, 1);
    width_ = addFloat("width", "Width", {0.0f, 200.0f, 0.1f}, 100.0f, "%");
    mix_ = addFloat("mix", "Dry/Wet", {0.0f, 100.0f, 0.1f}, 50.0f, "%");
}

double Echo::targetDelaySeconds(bool right, double bpm) const noexcept
{
    const bool useRight = right && !link_->get();
    double seconds = (useRight ? timeRight_ : timeLeft_)->get() / 1000.0;
    if (sync_->get())
        seconds = Delay::beatsForDivision((useRight ? divisionRight_ : divisionLeft_)->getIndex()) * 60.0 / std::max(20.0, bpm);
    return std::clamp(seconds, 0.001, kMaxSeconds);
}

void Echo::prepareDsp(double sampleRate, int)
{
    // Room for the longest time plus modulation and wobble.
    const auto capacity = static_cast<std::size_t>(std::ceil(kMaxSeconds * 1.1 * sampleRate)) + 16;
    for (auto& line : lines_)
        line.assign(capacity, 0.0);
    write_ = 0;

    double bpm = 120.0;
    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
            if (const auto tempo = position->getBpm())
                bpm = *tempo;
    delay_[0] = targetDelaySeconds(false, bpm) * sampleRate;
    delay_[1] = targetDelaySeconds(true, bpm) * sampleRate;
    glide_ = 1.0 - std::exp(-1.0 / (0.08 * sampleRate));
    lfoPhase_ = 0.0;
    wobbleValue_ = wobbleTarget_ = 0.0;
    wobbleCountdown_ = 0;
    duckEnvelope_ = 0.0;
    duckAttack_ = 1.0 - std::exp(-1.0 / (0.002 * sampleRate));
    duckRelease_ = 1.0 - std::exp(-1.0 / (0.25 * sampleRate));
    for (auto& f : highPassFilter_)
        f.reset();
    for (auto& f : lowPassFilter_)
        f.reset();
    highPassFor_ = -1.0f;
    reverbProcessor_.prepare(sampleRate);
}

double Echo::read(int channel, double delaySamples) const noexcept
{
    const auto& line = lines_[static_cast<std::size_t>(channel)];
    const int size = static_cast<int>(line.size());
    double position = static_cast<double>(write_) - delaySamples;
    while (position < 0.0)
        position += size;
    const int i1 = static_cast<int>(position) % size;
    const double t = position - std::floor(position);
    const double y0 = line[static_cast<std::size_t>((i1 - 1 + size) % size)];
    const double y1 = line[static_cast<std::size_t>(i1)];
    const double y2 = line[static_cast<std::size_t>((i1 + 1) % size)];
    const double y3 = line[static_cast<std::size_t>((i1 + 2) % size)];
    // 4-point, 3rd-order Hermite.
    const double c1 = 0.5 * (y2 - y0);
    const double c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
    const double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

double Echo::nextRandom() noexcept
{
    random_ ^= random_ << 13;
    random_ ^= random_ >> 17;
    random_ ^= random_ << 5;
    return static_cast<double>(random_) / 2147483648.0 - 1.0;
}

void Echo::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);
    const int size = static_cast<int>(lines_[0].size());
    if (size == 0)
        return;

    double bpm = 120.0;
    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
            if (const auto tempo = position->getBpm())
                bpm = *tempo;

    const int mode = mode_->getIndex();
    const double target[2] = {targetDelaySeconds(false, bpm) * sampleRate_, targetDelaySeconds(true, bpm) * sampleRate_};
    const double feedback = feedback_->get() / 100.0 * (invert_->get() ? -1.0 : 1.0);
    const double inputGain = juce::Decibels::decibelsToGain(static_cast<double>(input_->get()));
    const bool filter = filter_->get();
    const double lowPassHz = lowPass_->get();
    const double k = std::sqrt(2.0) - 1.24 * (resonance_->get() / 100.0); // Q 0.7 .. ~6
    const double modIncrement = modRate_->get() / sampleRate_;
    const double modDepth = modDepth_->get() / 100.0 * 0.03;
    const double modFilter = modFilter_->get() / 100.0 * 2.0; // octaves
    const double drive = 1.0 + 4.0 * drive_->get() / 100.0;
    const double noise = noise_->get() / 100.0 * 0.01;
    const double wobble = wobble_->get() / 100.0 * 0.01;
    const double duck = duck_->get() / 100.0;
    const double reverb = reverb_->get() / 100.0;
    const double reverbFeedback = 0.7 + 0.28 * reverbDecay_->get() / 100.0;
    const int reverbLocation = reverbLocation_->getIndex();
    const double width = width_->get() / 100.0;
    const double mix = mix_->get() / 100.0;
    const double maxDelay = static_cast<double>(size) - 4.0;

    if (filter && std::abs(highPass_->get() - highPassFor_) > 1.0e-3f)
    {
        highPassFor_ = highPass_->get();
        highPassCoefficients_ = dsp::BiquadCoefficients::make(dsp::BiquadCoefficients::Shape::lowCut, sampleRate_, highPassFor_,
                                                              std::sqrt(0.5), 0.0);
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const double dryLeft = left[i];
        const double dryRight = right[i];
        double inL = dryLeft * inputGain;
        double inR = dryRight * inputGain;
        if (noise > 0.0)
        {
            inL += nextRandom() * noise;
            inR += nextRandom() * noise;
        }

        double a = inL, b = inR;
        if (mode == 1)
        {
            a = 0.5 * (inL + inR);
            b = 0.0;
        }
        else if (mode == 2)
        {
            a = 0.5 * (inL + inR);
            b = 0.5 * (inL - inR);
        }

        if (reverb > 0.0 && reverbLocation == 0)
        {
            double rl = 0.0, rr = 0.0;
            reverbProcessor_.process(a, b, reverbFeedback, 0.3, rl, rr);
            a += rl * reverb;
            b += rr * reverb;
        }

        lfoPhase_ += modIncrement;
        if (lfoPhase_ >= 1.0)
            lfoPhase_ -= 1.0;
        const double lfo[2] = {std::sin(kTwoPi * lfoPhase_), std::sin(kTwoPi * (lfoPhase_ + 0.25))};

        if (wobble > 0.0)
        {
            if (--wobbleCountdown_ <= 0)
            {
                wobbleTarget_ = nextRandom();
                wobbleCountdown_ = static_cast<int>(sampleRate_ * 0.05);
            }
            wobbleValue_ += (wobbleTarget_ - wobbleValue_) * 0.0005;
        }

        double echoes[2];
        for (int ch = 0; ch < 2; ++ch)
        {
            auto& d = delay_[static_cast<std::size_t>(ch)];
            d += (target[ch] - d) * glide_;
            const double modulated = d * (1.0 + modDepth * lfo[ch] + wobble * wobbleValue_);
            double y = read(ch, std::clamp(modulated, 1.0, maxDelay));
            if (filter)
            {
                const double cutoff = std::clamp(lowPassHz * std::exp2(modFilter * lfo[ch]), 20.0, sampleRate_ * 0.45);
                const double g = std::tan(kPi * cutoff / sampleRate_);
                y = highPassFilter_[static_cast<std::size_t>(ch)].process(y, highPassCoefficients_);
                y = lowPassFilter_[static_cast<std::size_t>(ch)].lowPass(y, g, k);
            }
            echoes[ch] = y;
        }

        double back[2] = {saturate(echoes[0], drive) * feedback, saturate(echoes[1], drive) * feedback};
        if (reverb > 0.0 && reverbLocation == 2)
        {
            double rl = 0.0, rr = 0.0;
            reverbProcessor_.process(back[0], back[1], reverbFeedback, 0.3, rl, rr);
            back[0] += rl * reverb;
            back[1] += rr * reverb;
        }

        if (mode == 1)
        {
            lines_[0][static_cast<std::size_t>(write_)] = a + back[1];
            lines_[1][static_cast<std::size_t>(write_)] = back[0];
        }
        else
        {
            lines_[0][static_cast<std::size_t>(write_)] = a + back[0];
            lines_[1][static_cast<std::size_t>(write_)] = b + back[1];
        }
        write_ = write_ + 1 == size ? 0 : write_ + 1;

        double wetL = echoes[0], wetR = echoes[1];
        if (mode == 2)
        {
            wetL = echoes[0] + echoes[1];
            wetR = echoes[0] - echoes[1];
        }

        if (reverb > 0.0 && reverbLocation == 1)
        {
            double rl = 0.0, rr = 0.0;
            reverbProcessor_.process(wetL, wetR, reverbFeedback, 0.3, rl, rr);
            wetL += rl * reverb;
            wetR += rr * reverb;
        }

        const double mid = 0.5 * (wetL + wetR);
        const double side = 0.5 * (wetL - wetR) * width;
        wetL = mid + side;
        wetR = mid - side;

        if (duck > 0.0)
        {
            const double level = std::max(std::abs(dryLeft), std::abs(dryRight));
            duckEnvelope_ += (level - duckEnvelope_) * (level > duckEnvelope_ ? duckAttack_ : duckRelease_);
            const double gain = 1.0 - duck * std::min(1.0, duckEnvelope_ * 3.0);
            wetL *= gain;
            wetR *= gain;
        }

        left[i] = dryLeft * (1.0 - mix) + wetL * mix;
        right[i] = dryRight * (1.0 - mix) + wetR * mix;
    }
}

} // namespace dojo::devices
