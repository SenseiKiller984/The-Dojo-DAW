#include "session/audioedit/Spectral.h"

#include "session/audioedit/Stft.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace dojo::audioedit::spectral
{
namespace
{
constexpr double kMargin = 0.05;
constexpr double kFade = 0.01;
constexpr double kTwoPi = 2.0 * std::numbers::pi;

bool isCancelled(const std::atomic<bool>* cancel) noexcept { return cancel != nullptr && cancel->load(std::memory_order_relaxed); }

int orderAt(double sampleRate) noexcept
{
    int order = orderFor(sampleRate * 0.045);
    if ((1 << order) > sampleRate * 0.06)
        --order;
    return order;
}

/** 1 inside [low, high], falling to 0 a twelfth of an octave outside. */
double frequencyWeight(double hz, double low, double high) noexcept
{
    constexpr double kRamp = 1.0 / 12.0;
    if (hz >= low && hz <= high)
        return 1.0;
    if (hz < low)
        return low <= 1.0 ? 1.0 : std::clamp(1.0 - std::log2(low / std::max(hz, 1.0)) / kRamp, 0.0, 1.0);
    return std::clamp(1.0 - std::log2(hz / high) / kRamp, 0.0, 1.0);
}

/** Repairs cover every frame whose window touches their region; the rest fade in and out over 10 ms. */
double timeWeight(const Operation& op, double t, double halfWindow) noexcept
{
    if (op.kind == Kind::repair)
        return t >= op.start - halfWindow && t <= op.end + halfWindow ? 1.0 : 0.0;
    const double fade = std::max(1.0e-4, std::min(kFade, 0.25 * (op.end - op.start)));
    if (t < op.start - fade || t > op.end + fade)
        return 0.0;
    if (t < op.start)
        return (t - (op.start - fade)) / fade;
    if (t > op.end)
        return ((op.end + fade) - t) / fade;
    return 1.0;
}

/** A Hann-windowed frame of a channel centred at `centre` (zeros beyond the ends). */
void readFrame(const float* x, int total, long long centre, const std::vector<float>& window, std::vector<float>& frame)
{
    const int n = static_cast<int>(window.size());
    for (int i = 0; i < n; ++i)
    {
        const long long index = centre - n / 2 + i;
        frame[static_cast<std::size_t>(i)] = index >= 0 && index < total ? x[index] * window[static_cast<std::size_t>(i)] : 0.0f;
    }
}

/** What a repair rebuilds from: the spectra just before and just after (per channel). */
struct RepairReference
{
    std::vector<double> left, right; ///< magnitudes (averaged over four frames)
    std::vector<double> phase;       ///< of the last frame before
    std::vector<double> omega;       ///< each bin's frequency there (radians a sample)
    double leftCentre = 0.0;         ///< samples
};

RepairReference referenceFor(const float* x, int total, const Operation& op, double sampleRate, RealFft& fft, const std::vector<float>& window)
{
    const int n = fft.size();
    const int bins = fft.bins();
    const int hop = n / 4;
    RepairReference r;
    r.left.assign(static_cast<std::size_t>(bins), 0.0);
    r.right.assign(static_cast<std::size_t>(bins), 0.0);
    r.phase.assign(static_cast<std::size_t>(bins), 0.0);
    r.omega.assign(static_cast<std::size_t>(bins), 0.0);
    std::vector<float> frame(static_cast<std::size_t>(n));
    std::vector<std::complex<float>> spectrum(static_cast<std::size_t>(bins)), previous(static_cast<std::size_t>(bins));
    const auto firstLeft = static_cast<long long>(std::floor(op.start * sampleRate)) - n / 2;
    const auto firstRight = static_cast<long long>(std::ceil(op.end * sampleRate)) + n / 2;
    r.leftCentre = static_cast<double>(firstLeft);
    for (int j = 0; j < 4; ++j)
    {
        readFrame(x, total, firstLeft - static_cast<long long>(j) * hop, window, frame);
        fft.forward(frame.data(), spectrum.data());
        for (int k = 0; k < bins; ++k)
            r.left[static_cast<std::size_t>(k)] += std::abs(spectrum[static_cast<std::size_t>(k)]) / 4.0;
        if (j == 0)
        {
            for (int k = 0; k < bins; ++k)
                r.phase[static_cast<std::size_t>(k)] = std::arg(spectrum[static_cast<std::size_t>(k)]);
            previous = spectrum;
        }
        else if (j == 1)
            for (int k = 0; k < bins; ++k)
            {
                const auto ku = static_cast<std::size_t>(k);
                const double expected = kTwoPi * k * hop / n;
                r.omega[ku] = kTwoPi * k / n + princarg(std::arg(previous[ku]) - std::arg(spectrum[ku]) - expected) / hop;
            }
        readFrame(x, total, firstRight + static_cast<long long>(j) * hop, window, frame);
        fft.forward(frame.data(), spectrum.data());
        for (int k = 0; k < bins; ++k)
            r.right[static_cast<std::size_t>(k)] += std::abs(spectrum[static_cast<std::size_t>(k)]) / 4.0;
    }
    return r;
}

/** A profile learnt at another FFT size, read at this one's bins. */
std::vector<double> profileAt(const std::vector<float>& profile, int bins)
{
    std::vector<double> out(static_cast<std::size_t>(bins), 0.0);
    if (profile.empty())
        return out;
    const double scale = static_cast<double>(profile.size() - 1) / std::max(1, bins - 1);
    // (a noise's magnitudes grow with the square root of the window's length)
    const double level = std::sqrt(static_cast<double>(bins - 1) / static_cast<double>(std::max<std::size_t>(1, profile.size() - 1)));
    for (int k = 0; k < bins; ++k)
    {
        const double x = k * scale;
        const auto i = std::min(static_cast<std::size_t>(x), profile.size() - 1);
        const auto j = std::min(i + 1, profile.size() - 1);
        const double f = x - static_cast<double>(i);
        out[static_cast<std::size_t>(k)] = (profile[i] + (profile[j] - profile[i]) * f) * level;
    }
    return out;
}
} // namespace

int fftSizeFor(double sampleRate) noexcept { return 1 << orderAt(sampleRate); }

std::vector<float> learnProfile(const juce::AudioBuffer<float>& audio, double sampleRate, double start, double end)
{
    RealFft fft(orderAt(sampleRate));
    const int n = fft.size();
    const int bins = fft.bins();
    const int hop = n / 4;
    const auto window = hannWindow(n);
    std::vector<double> sum(static_cast<std::size_t>(bins), 0.0);
    std::vector<float> frame(static_cast<std::size_t>(n));
    std::vector<std::complex<float>> spectrum(static_cast<std::size_t>(bins));
    const auto a = static_cast<long long>(start * sampleRate);
    const auto b = static_cast<long long>(end * sampleRate);
    std::vector<long long> centres;
    for (auto c = a + n / 2; c + n / 2 <= b; c += hop)
        centres.push_back(c);
    if (centres.empty())
        centres.push_back((a + b) / 2);
    int count = 0;
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        for (const auto c : centres)
        {
            readFrame(audio.getReadPointer(ch), audio.getNumSamples(), c, window, frame);
            fft.forward(frame.data(), spectrum.data());
            for (int k = 0; k < bins; ++k)
                sum[static_cast<std::size_t>(k)] += std::abs(spectrum[static_cast<std::size_t>(k)]);
            ++count;
        }
    std::vector<float> profile(static_cast<std::size_t>(bins));
    for (int k = 0; k < bins; ++k)
        profile[static_cast<std::size_t>(k)] = static_cast<float>(sum[static_cast<std::size_t>(k)] / std::max(1, count));
    return profile;
}

double detectHum(const juce::AudioBuffer<float>& audio, double sampleRate)
{
    constexpr int kOrder = 14; // (fine bins: a few hertz at 44.1 kHz)
    RealFft fft(kOrder);
    const int n = fft.size();
    const int bins = fft.bins();
    const auto window = hannWindow(n);
    std::vector<double> power(static_cast<std::size_t>(bins), 0.0);
    std::vector<float> frame(static_cast<std::size_t>(n));
    std::vector<std::complex<float>> spectrum(static_cast<std::size_t>(bins));
    const int total = audio.getNumSamples();
    const int frames = std::clamp(total / (n / 2), 1, 40);
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        for (int i = 0; i < frames; ++i)
        {
            const long long centre = frames == 1 ? total / 2 : static_cast<long long>(n / 2) + static_cast<long long>(i) * (total - n) / std::max(1, frames - 1);
            readFrame(audio.getReadPointer(ch), total, centre, window, frame);
            fft.forward(frame.data(), spectrum.data());
            for (int k = 0; k < bins; ++k)
                power[static_cast<std::size_t>(k)] += std::norm(spectrum[static_cast<std::size_t>(k)]);
        }
    const double binHz = sampleRate / n;
    const auto at = [&](double hz)
    {
        const double x = hz / binHz;
        const auto i = std::clamp(static_cast<int>(x), 0, bins - 2);
        const double f = x - i;
        return power[static_cast<std::size_t>(i)] * (1.0 - f) + power[static_cast<std::size_t>(i + 1)] * f;
    };
    const auto score = [&](double f0)
    {
        double s = 0.0;
        for (int h = 1; h <= 6; ++h)
        {
            // Against the spectrum a few hertz either side (hum is a narrow line).
            const double line = at(h * f0);
            const double around = 0.5 * (at(h * f0 - 6.0) + at(h * f0 + 6.0)) + 1.0e-20;
            s += std::log10(1.0 + line / around);
        }
        return s;
    };
    const double base = score(50.0) >= score(60.0) ? 50.0 : 60.0;
    double best = base;
    double bestScore = -1.0;
    for (double f = base - 1.5; f <= base + 1.5 + 1.0e-9; f += 0.05)
        if (const double s = score(f); s > bestScore)
        {
            bestScore = s;
            best = f;
        }
    return best;
}

std::uint64_t keyOf(const std::vector<Operation>& operations)
{
    juce::String key;
    for (const auto& op : operations)
    {
        if (!op.enabled)
            continue;
        key << static_cast<int>(op.kind) << ":" << op.start << "," << op.end << "," << op.low << "," << op.high << "," << op.gainDb << "," << op.amount
            << "," << op.reductionDb << "," << op.sensitivity << "," << op.humHz << "," << op.harmonics << "," << op.depthDb << ",";
        if (!op.profile.empty())
            key << juce::String::toHexString(op.profile.data(), static_cast<int>(op.profile.size() * sizeof(float)), 0).hashCode64();
        key << "|";
    }
    return static_cast<std::uint64_t>(key.hashCode64());
}

juce::AudioBuffer<float> apply(const juce::AudioBuffer<float>& audio, double sampleRate, const std::vector<Operation>& operations,
                               const std::atomic<bool>* cancel)
{
    juce::AudioBuffer<float> output;
    output.makeCopyOf(audio);
    std::vector<Operation> ops;
    for (const auto& op : operations)
        if (op.enabled && op.end > op.start)
            ops.push_back(op);
    if (ops.empty())
        return output;

    RealFft fft(orderAt(sampleRate));
    const int n = fft.size();
    const int bins = fft.bins();
    const int hop = n / 4;
    const auto window = hannWindow(n);
    const int total = audio.getNumSamples();
    const double duration = static_cast<double>(total) / sampleRate;
    const double halfWindow = 0.5 * n / sampleRate;
    const double binHz = sampleRate / n;

    // The spans (merged).
    std::vector<std::pair<double, double>> spans;
    for (const auto& op : ops)
    {
        if (op.kind == Kind::dehum)
            continue; // (in the time domain, below)
        const double extra = op.kind == Kind::repair ? halfWindow : 0.0;
        spans.emplace_back(std::max(0.0, op.start - extra - kMargin), std::min(duration, op.end + extra + kMargin));
    }
    std::sort(spans.begin(), spans.end());
    std::vector<std::pair<double, double>> merged;
    for (const auto& s : spans)
        if (!merged.empty() && s.first <= merged.back().second + 2.0 * kFade)
            merged.back().second = std::max(merged.back().second, s.second);
        else if (s.second > s.first)
            merged.push_back(s);

    // What does not depend on the channel.
    double hum = 0.0;
    std::vector<std::vector<double>> profiles(ops.size());
    std::vector<std::vector<double>> bandWeights(ops.size());
    for (std::size_t o = 0; o < ops.size(); ++o)
    {
        const auto& op = ops[o];
        bandWeights[o].resize(static_cast<std::size_t>(bins));
        for (int k = 0; k < bins; ++k)
            bandWeights[o][static_cast<std::size_t>(k)] = op.kind == Kind::dehum ? 1.0 : frequencyWeight(k * binHz, op.low, op.high);
        if (op.kind == Kind::denoise)
            profiles[o] = profileAt(op.profile, bins);
        if (op.kind == Kind::dehum && op.humHz <= 0.0 && hum <= 0.0)
            hum = detectHum(audio, sampleRate);
    }

    std::vector<float> frame(static_cast<std::size_t>(n)), synth(static_cast<std::size_t>(n));
    std::vector<std::complex<float>> x(static_cast<std::size_t>(bins));
    std::vector<double> rawGain(static_cast<std::size_t>(bins));
    constexpr float kOverlapGain = 1.0f / 1.5f;
    const int fade = std::max(1, static_cast<int>(std::lround(kFade * sampleRate)));

    for (const auto& [from, to] : merged)
    {
        const auto a = static_cast<long long>(std::floor(from * sampleRate));
        const auto b = std::min(static_cast<long long>(total), static_cast<long long>(std::ceil(to * sampleRate)));
        const int length = static_cast<int>(std::max(0LL, b - a));
        if (length == 0)
            continue;
        const long long base = a - n - n / 2;
        const int frames = static_cast<int>((b + n / 2 - (a - n)) / hop) + 2;
        std::vector<float> ola(static_cast<std::size_t>(frames * hop + n));
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        {
            const float* in = audio.getReadPointer(ch);
            std::fill(ola.begin(), ola.end(), 0.0f);
            std::vector<RepairReference> references(ops.size());
            for (std::size_t o = 0; o < ops.size(); ++o)
                if (ops[o].kind == Kind::repair && ops[o].end + halfWindow >= from && ops[o].start - halfWindow <= to)
                    references[o] = referenceFor(in, total, ops[o], sampleRate, fft, window);
            std::vector<std::vector<double>> smoothed(ops.size()); // (denoise gains, frame to frame)
            for (int m = 0; m < frames; ++m)
            {
                if ((m & 31) == 0 && isCancelled(cancel))
                    return {};
                const long long centre = a - n + static_cast<long long>(m) * hop;
                const double t = static_cast<double>(centre) / sampleRate;
                readFrame(in, total, centre, window, frame);
                fft.forward(frame.data(), x.data());
                for (std::size_t o = 0; o < ops.size(); ++o)
                {
                    const auto& op = ops[o];
                    const double wt = timeWeight(op, t, halfWindow);
                    if (wt <= 0.0)
                        continue;
                    const auto& band = bandWeights[o];
                    switch (op.kind)
                    {
                        case Kind::gain:
                        case Kind::erase:
                        {
                            const double g = op.kind == Kind::erase ? 0.0 : juce::Decibels::decibelsToGain(op.gainDb, -200.0);
                            for (int k = 0; k < bins; ++k)
                                if (const double w = wt * band[static_cast<std::size_t>(k)]; w > 0.0)
                                    x[static_cast<std::size_t>(k)] *= static_cast<float>(1.0 + w * (g - 1.0));
                            break;
                        }
                        case Kind::denoise:
                        {
                            const auto& noise = profiles[o];
                            const double floor = juce::Decibels::decibelsToGain(-std::abs(op.reductionDb), -200.0);
                            for (int k = 0; k < bins; ++k)
                            {
                                const double magnitude = std::abs(x[static_cast<std::size_t>(k)]);
                                const double p = noise[static_cast<std::size_t>(k)] * op.sensitivity;
                                rawGain[static_cast<std::size_t>(k)] =
                                    magnitude > 1.0e-12 ? std::max(floor, std::sqrt(std::max(0.0, 1.0 - (p * p) / (magnitude * magnitude)))) : floor;
                            }
                            auto& previous = smoothed[o];
                            if (previous.empty())
                                previous.assign(static_cast<std::size_t>(bins), 1.0);
                            for (int k = 0; k < bins; ++k)
                            {
                                const auto ku = static_cast<std::size_t>(k);
                                const double across = 0.25 * rawGain[k > 0 ? ku - 1 : ku] + 0.5 * rawGain[ku] + 0.25 * rawGain[k + 1 < bins ? ku + 1 : ku];
                                previous[ku] = 0.5 * previous[ku] + 0.5 * across;
                                const double w = wt * band[ku] * op.amount;
                                x[ku] *= static_cast<float>(1.0 + w * (previous[ku] - 1.0));
                            }
                            break;
                        }
                        case Kind::dehum:
                            break;
                        case Kind::repair:
                        {
                            const auto& r = references[o];
                            if (r.left.empty())
                                break;
                            const double from_ = op.start - halfWindow;
                            const double u = std::clamp((t - from_) / std::max(1.0e-9, op.end + halfWindow - from_), 0.0, 1.0);
                            for (int k = 0; k < bins; ++k)
                            {
                                const auto ku = static_cast<std::size_t>(k);
                                const double w = wt * band[ku] * op.amount;
                                if (w <= 0.0)
                                    continue;
                                const double magnitude = std::exp((1.0 - u) * std::log(r.left[ku] + 1.0e-12) + u * std::log(r.right[ku] + 1.0e-12));
                                const double phase = r.phase[ku] + r.omega[ku] * (static_cast<double>(centre) - r.leftCentre);
                                const auto rebuilt = std::polar(static_cast<float>(magnitude), static_cast<float>(phase));
                                x[ku] += static_cast<float>(w) * (rebuilt - x[ku]);
                            }
                            break;
                        }
                    }
                }
                fft.inverse(x.data(), synth.data());
                float* target = ola.data() + static_cast<std::ptrdiff_t>(m) * hop;
                for (int i = 0; i < n; ++i)
                    target[i] += synth[static_cast<std::size_t>(i)] * window[static_cast<std::size_t>(i)] * kOverlapGain;
            }
            // Crossfaded in.
            float* out = output.getWritePointer(ch);
            const auto offset = static_cast<std::size_t>(a - base);
            for (int i = 0; i < length; ++i)
            {
                const float w = std::min({1.0f, static_cast<float>(i) / static_cast<float>(fade), static_cast<float>(length - 1 - i) / static_cast<float>(fade)});
                out[a + i] = in[a + i] * (1.0f - w) + ola[offset + static_cast<std::size_t>(i)] * w;
            }
        }
    }
    // De-hum: notches at the mains frequency and its harmonics (narrow biquads, run forwards and
    // backwards: no phase shift), mixed in over the operation's time with its soft edges.
    for (const auto& op : ops)
    {
        if (op.kind != Kind::dehum)
            continue;
        const double f0 = op.humHz > 0.0 ? op.humHz : hum;
        const double depth = 1.0 - juce::Decibels::decibelsToGain(-std::abs(op.depthDb), -200.0);
        const auto first = static_cast<int>(std::max(0.0, std::floor((op.start - 0.3) * sampleRate)));
        const auto last = static_cast<int>(std::min(static_cast<double>(total), std::ceil((op.end + 0.3) * sampleRate)));
        if (last <= first)
            continue;
        struct Notch
        {
            double b0, b1, b2, a1, a2;
        };
        std::vector<Notch> notches;
        for (int h = 1; h <= op.harmonics && h * f0 < sampleRate * 0.45; ++h)
        {
            const double f = h * f0;
            const double w0 = kTwoPi * f / sampleRate;
            const double q = f / std::max(3.0, f * 0.01); // (a few hertz wide)
            const double alpha = std::sin(w0) / (2.0 * q);
            const double a0 = 1.0 + alpha;
            notches.push_back({1.0 / a0, -2.0 * std::cos(w0) / a0, 1.0 / a0, -2.0 * std::cos(w0) / a0, (1.0 - alpha) / a0});
        }
        std::vector<double> segment(static_cast<std::size_t>(last - first));
        for (int ch = 0; ch < output.getNumChannels(); ++ch)
        {
            if (isCancelled(cancel))
                return {};
            float* out = output.getWritePointer(ch);
            for (int i = first; i < last; ++i)
                segment[static_cast<std::size_t>(i - first)] = out[i];
            for (const auto& notch : notches)
                for (int pass = 0; pass < 2; ++pass)
                {
                    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
                    const auto filter = [&](double& v)
                    {
                        const double y = notch.b0 * v + notch.b1 * x1 + notch.b2 * x2 - notch.a1 * y1 - notch.a2 * y2;
                        x2 = x1;
                        x1 = v;
                        y2 = y1;
                        y1 = y;
                        v = y;
                    };
                    if (pass == 0)
                        for (auto& v : segment)
                            filter(v);
                    else
                        for (auto it = segment.rbegin(); it != segment.rend(); ++it)
                            filter(*it);
                }
            for (int i = first; i < last; ++i)
            {
                const double w = timeWeight(op, i / sampleRate, halfWindow) * op.amount * depth;
                if (w > 0.0)
                    out[i] = static_cast<float>(out[i] + w * (segment[static_cast<std::size_t>(i - first)] - out[i]));
            }
        }
    }
    return output;
}

double Spectrogram::rowHz(double row) const noexcept { return lowHz * std::pow(highHz / lowHz, row / std::max(1, rows - 1)); }

double Spectrogram::hzRow(double hz) const noexcept
{
    return std::log(std::max(hz, 1.0e-6) / lowHz) / std::log(highHz / lowHz) * std::max(1, rows - 1);
}

Spectrogram analyse(const juce::AudioBuffer<float>& audio, double sampleRate, int rows, const std::atomic<bool>* cancel)
{
    Spectrogram s;
    const int total = audio.getNumSamples();
    if (total <= 0 || sampleRate <= 0.0)
        return s;
    RealFft fft(orderAt(sampleRate));
    const int n = fft.size();
    const int bins = fft.bins();
    const auto window = hannWindow(n);
    const int hop = std::max(n / 4, (total + 15999) / 16000);
    s.rows = rows;
    s.hop = static_cast<double>(hop) / sampleRate;
    s.lowHz = 20.0;
    s.highHz = std::min(20000.0, sampleRate * 0.49);
    s.duration = static_cast<double>(total) / sampleRate;
    s.frames = total / hop + 1;
    s.levels.assign(static_cast<std::size_t>(s.frames * rows), 0);

    std::vector<float> mono(static_cast<std::size_t>(total), 0.0f);
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
    {
        const float* in = audio.getReadPointer(ch);
        for (int i = 0; i < total; ++i)
            mono[static_cast<std::size_t>(i)] += in[i] / static_cast<float>(audio.getNumChannels());
    }
    // Each row: the loudest bin centred in its band, or (narrower than a bin) the spectrum read at its
    // frequency between the two bins around it.
    std::vector<int> lo(static_cast<std::size_t>(rows)), hi(static_cast<std::size_t>(rows));
    std::vector<double> at(static_cast<std::size_t>(rows));
    const double binHz = sampleRate / n;
    for (int r = 0; r < rows; ++r)
    {
        const auto ru = static_cast<std::size_t>(r);
        lo[ru] = std::clamp(static_cast<int>(std::ceil(s.rowHz(r - 0.5) / binHz)), 0, bins - 1);
        hi[ru] = std::clamp(static_cast<int>(std::floor(s.rowHz(r + 0.5) / binHz)), 0, bins - 1);
        at[ru] = std::clamp(s.rowHz(r) / binHz, 0.0, static_cast<double>(bins - 2));
    }
    std::vector<float> frame(static_cast<std::size_t>(n));
    std::vector<std::complex<float>> spectrum(static_cast<std::size_t>(bins));
    std::vector<float> magnitude(static_cast<std::size_t>(bins));
    for (int f = 0; f < s.frames; ++f)
    {
        if ((f & 63) == 0 && isCancelled(cancel))
            return {};
        readFrame(mono.data(), total, static_cast<long long>(f) * hop, window, frame);
        fft.forward(frame.data(), spectrum.data());
        for (int k = 0; k < bins; ++k)
            magnitude[static_cast<std::size_t>(k)] = std::abs(spectrum[static_cast<std::size_t>(k)]) * 4.0f / static_cast<float>(n);
        for (int r = 0; r < rows; ++r)
        {
            const auto ru = static_cast<std::size_t>(r);
            float peak = 0.0f;
            if (lo[ru] <= hi[ru])
                for (int k = lo[ru]; k <= hi[ru]; ++k)
                    peak = std::max(peak, magnitude[static_cast<std::size_t>(k)]);
            else
            {
                const auto k = static_cast<std::size_t>(at[ru]);
                const auto fraction = static_cast<float>(at[ru] - static_cast<double>(k));
                peak = magnitude[k] * (1.0f - fraction) + magnitude[k + 1] * fraction;
            }
            const double db = 20.0 * std::log10(peak + 1.0e-9);
            s.levels[static_cast<std::size_t>(f * rows + r)] = static_cast<std::uint8_t>(std::clamp((db + 110.0) / 110.0 * 255.0, 0.0, 255.0));
        }
    }
    return s;
}

} // namespace dojo::audioedit::spectral
