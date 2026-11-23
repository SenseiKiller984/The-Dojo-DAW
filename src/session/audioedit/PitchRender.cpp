#include "session/audioedit/PitchRender.h"

#include "session/audioedit/Stft.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <tuple>

namespace dojo::audioedit::pitch
{
namespace
{
constexpr double kMargin = 0.06; ///< seconds around what an edit touches
constexpr double kFade = 0.01;   ///< the crossfade into and out of a span
constexpr double kTwoPi = 2.0 * std::numbers::pi;

bool differs(double a, double b, double tolerance) noexcept { return std::abs(a - b) > tolerance; }
bool isCancelled(const std::atomic<bool>* cancel) noexcept { return cancel != nullptr && cancel->load(std::memory_order_relaxed); }

std::vector<const Edit*> byStart(const std::vector<Edit>& edits)
{
    std::vector<const Edit*> order;
    for (const auto& e : edits)
        order.push_back(&e);
    std::sort(order.begin(), order.end(), [](const Edit* a, const Edit* b) { return a->note.start < b->note.start; });
    return order;
}

/** Melodic: the pitch offset (semitones) and gain (dB) at `t`: the note's there; between notes that
    nearly touch, a glide from one to the other; 20 ms ramps to nothing at free ends. */
std::pair<double, double> melodicOffset(const std::vector<const Edit*>& order, double t)
{
    constexpr double kTouch = 0.03;
    constexpr double kRamp = 0.02;
    const auto after = std::upper_bound(order.begin(), order.end(), t, [](double v, const Edit* e) { return v < e->note.start; });
    const Edit* previous = after == order.begin() ? nullptr : *std::prev(after);
    const Edit* next = after == order.end() ? nullptr : *after;
    if (previous != nullptr && t < previous->note.end)
        return {previous->offsetAt(t), previous->gainDb};
    const auto endOf = [](const Edit* e) { return std::pair<double, double>{e->offsetAt(e->note.end), e->gainDb}; };
    const auto startOf = [](const Edit* e) { return std::pair<double, double>{e->offsetAt(e->note.start), e->gainDb}; };
    if (previous != nullptr && next != nullptr && next->note.start - previous->note.end <= kTouch)
    {
        const double x = (t - previous->note.end) / std::max(1.0e-9, next->note.start - previous->note.end);
        const auto [o0, g0] = endOf(previous);
        const auto [o1, g1] = startOf(next);
        return {o0 + (o1 - o0) * x, g0 + (g1 - g0) * x};
    }
    double offset = 0.0, gain = 0.0;
    if (previous != nullptr && t - previous->note.end < kRamp)
    {
        const double w = 1.0 - (t - previous->note.end) / kRamp;
        offset += endOf(previous).first * w;
        gain += endOf(previous).second * w;
    }
    if (next != nullptr && next->note.start - t < kRamp)
    {
        const double w = 1.0 - (next->note.start - t) / kRamp;
        offset += startOf(next).first * w;
        gain += startOf(next).second * w;
    }
    return {offset, gain};
}

/** A peak of the previous output frame, for carrying its phase on. */
struct Peak
{
    double omega = 0.0;    ///< analysed frequency (radians a sample)
    double omegaOut = 0.0; ///< where it went
    double psi = 0.0;      ///< its output phase
};
} // namespace

Edit Edit::of(const Note& note)
{
    Edit e;
    e.note = note;
    e.newStart = note.start;
    e.newEnd = note.end;
    e.newPitch = note.pitch;
    return e;
}

bool Edit::pitchEdited() const noexcept { return differs(newPitch, note.pitch, 1.0e-4) || differs(drift, 1.0, 1.0e-4); }
bool Edit::timeEdited() const noexcept { return differs(newStart, note.start, 1.0e-6) || differs(newEnd, note.end, 1.0e-6); }
bool Edit::edited() const noexcept { return pitchEdited() || timeEdited() || differs(gainDb, 0.0, 1.0e-4); }
double Edit::offsetAt(double t) const noexcept { return (newPitch - note.pitch) + (drift - 1.0) * note.driftAt(t); }

std::vector<Span> editedSpans(const std::vector<Edit>& edits, const RenderOptions& options, double duration)
{
    const bool timing = options.mode == Mode::melodic;
    std::vector<Span> spans;
    for (const auto& e : edits)
    {
        const bool timeEdit = timing && e.timeEdited();
        if (!e.pitchEdited() && !timeEdit && !differs(e.gainDb, 0.0, 1.0e-4))
            continue;
        double from = e.note.start;
        double to = e.note.end;
        if (timeEdit)
        {
            from = std::min(from, e.newStart);
            to = std::max(to, e.newEnd);
        }
        spans.push_back({std::max(0.0, from - kMargin), std::min(duration, to + kMargin)});
    }
    std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.from < b.from; });
    std::vector<Span> merged;
    for (const auto& s : spans)
    {
        if (s.to <= s.from)
            continue;
        if (!merged.empty() && s.from <= merged.back().to + 2.0 * kFade)
            merged.back().to = std::max(merged.back().to, s.to);
        else
            merged.push_back(s);
    }
    return merged;
}

std::uint64_t spanKey(const std::vector<Edit>& edits, const RenderOptions& options, Span span)
{
    juce::String key;
    key << span.from << ":" << span.to << ":" << static_cast<int>(options.mode) << ":" << (options.preserveFormants ? 1 : 0);
    for (const auto& e : edits)
    {
        const double from = std::min(e.note.start, e.newStart);
        const double to = std::max(e.note.end, e.newEnd);
        if (to < span.from - 0.2 || from > span.to + 0.2)
            continue;
        key << "|" << e.note.start << "," << e.note.end << "," << e.note.pitch << "," << static_cast<int>(e.note.drift.size()) << ","
            << e.newStart << "," << e.newEnd << "," << e.newPitch << "," << e.drift << "," << e.gainDb;
    }
    return static_cast<std::uint64_t>(key.hashCode64());
}

juce::AudioBuffer<float> renderSpan(const juce::AudioBuffer<float>& input, double sampleRate, const std::vector<Edit>& edits,
                                    const RenderOptions& options, Span span, const std::atomic<bool>* cancel)
{
    const bool poly = options.mode == Mode::polyphonic;
    int order = orderFor(sampleRate * (poly ? 0.09 : 0.045));
    if ((1 << order) > sampleRate * (poly ? 0.12 : 0.06))
        --order;
    RealFft fft(order);
    const int n = fft.size();
    const int bins = fft.bins();
    const int hop = n / 4;
    const int lag = n / 8; // the second analysis, for frequencies
    const auto window = hannWindow(n);
    const int total = input.getNumSamples();
    const int channels = input.getNumChannels();

    const auto a = static_cast<long long>(std::floor(span.from * sampleRate));
    const auto b = static_cast<long long>(std::ceil(span.to * sampleRate));
    const int length = static_cast<int>(std::max(0LL, b - a));
    juce::AudioBuffer<float> out(channels, length);
    out.clear();
    if (length == 0)
        return out;

    // The time warp (melodic), output -> input, through the notes' moved boundaries.
    std::vector<std::pair<double, double>> anchors{{span.from, span.from}, {span.to, span.to}};
    if (!poly)
        for (const auto& e : edits)
            for (const auto& [in, outTime] : {std::pair{e.note.start, e.newStart}, std::pair{e.note.end, e.newEnd}})
                if (in > span.from && in < span.to)
                    anchors.emplace_back(in, std::clamp(outTime, span.from, span.to));
    std::sort(anchors.begin(), anchors.end());
    for (std::size_t i = 1; i < anchors.size(); ++i)
        anchors[i].second = std::max(anchors[i].second, anchors[i - 1].second);
    const auto inputAt = [&anchors](double t)
    {
        if (t <= anchors.front().second)
            return t - anchors.front().second + anchors.front().first;
        for (std::size_t i = 1; i < anchors.size(); ++i)
            if (t <= anchors[i].second)
            {
                const auto& [i0, o0] = anchors[i - 1];
                const auto& [i1, o1] = anchors[i];
                return o1 - o0 < 1.0e-9 ? i1 : i0 + (i1 - i0) * (t - o0) / (o1 - o0);
            }
        return t - anchors.back().second + anchors.back().first;
    };

    // The frames: output centres from a - n on, every hop, past b + n / 2.
    const long long base = a - n - n / 2; // the overlap-add buffer's first sample
    const int frames = static_cast<int>((b + n / 2 - (a - n)) / hop) + 2;
    std::vector<double> inputCentre(static_cast<std::size_t>(frames));
    std::vector<double> frameOffset(static_cast<std::size_t>(frames), 0.0), frameGain(static_cast<std::size_t>(frames), 0.0);
    std::vector<bool> steady(static_cast<std::size_t>(frames), true); // input advancing a hop a frame
    const auto order_ = byStart(edits);
    for (int m = 0; m < frames; ++m)
    {
        const auto mu = static_cast<std::size_t>(m);
        const double centre = static_cast<double>(a - n + static_cast<long long>(m) * hop);
        inputCentre[mu] = inputAt(centre / sampleRate) * sampleRate;
        if (m > 0)
            steady[mu] = std::abs(inputCentre[mu] - inputCentre[mu - 1] - hop) < 0.01;
        if (!poly)
            std::tie(frameOffset[mu], frameGain[mu]) = melodicOffset(order_, inputCentre[mu] / sampleRate);
    }

    std::vector<float> frame1(static_cast<std::size_t>(n)), frame2(static_cast<std::size_t>(n)), synth(static_cast<std::size_t>(n)),
        cepstrum(static_cast<std::size_t>(n));
    std::vector<std::complex<float>> x1(static_cast<std::size_t>(bins)), x2(static_cast<std::size_t>(bins)), y(static_cast<std::size_t>(bins)),
        logSpectrum(static_cast<std::size_t>(bins));
    std::vector<double> mag(static_cast<std::size_t>(bins)), phase1(static_cast<std::size_t>(bins)), envelope(static_cast<std::size_t>(bins));
    std::vector<float> ola(static_cast<std::size_t>(frames * hop + n), 0.0f);
    constexpr float kOverlapGain = 1.0f / 1.5f; // Hann analysis and synthesis at four times overlap

    for (int ch = 0; ch < channels; ++ch)
    {
        const float* source = input.getReadPointer(ch);
        std::fill(ola.begin(), ola.end(), 0.0f);
        std::vector<Peak> previous, current;
        for (int m = 0; m < frames; ++m)
        {
            if ((m & 31) == 0 && isCancelled(cancel))
                return {};
            const auto mu = static_cast<std::size_t>(m);
            const auto p = static_cast<long long>(std::llround(inputCentre[mu]));
            const double t = inputCentre[mu] / sampleRate;
            for (int i = 0; i < n; ++i)
            {
                const long long i1 = p - n / 2 + i;
                const long long i2 = i1 + lag;
                frame1[static_cast<std::size_t>(i)] = i1 >= 0 && i1 < total ? source[i1] * window[static_cast<std::size_t>(i)] : 0.0f;
                frame2[static_cast<std::size_t>(i)] = i2 >= 0 && i2 < total ? source[i2] * window[static_cast<std::size_t>(i)] : 0.0f;
            }
            fft.forward(frame1.data(), x1.data());
            fft.forward(frame2.data(), x2.data());
            double loudest = 0.0;
            for (int k = 0; k < bins; ++k)
            {
                const auto ku = static_cast<std::size_t>(k);
                mag[ku] = std::abs(x1[ku]);
                phase1[ku] = std::arg(x1[ku]);
                loudest = std::max(loudest, mag[ku]);
            }
            current.clear();
            std::fill(y.begin(), y.end(), std::complex<float>{});
            if (loudest > 1.0e-9)
            {
                // Peaks, and the regions around them (to the valleys between).
                std::vector<int> peaks;
                for (int k = 2; k < bins - 2; ++k)
                {
                    const double v = mag[static_cast<std::size_t>(k)];
                    if (v > loudest * 1.0e-4 && v > mag[static_cast<std::size_t>(k - 1)] && v >= mag[static_cast<std::size_t>(k + 1)]
                        && v > mag[static_cast<std::size_t>(k - 2)] && v >= mag[static_cast<std::size_t>(k + 2)])
                        peaks.push_back(k);
                }
                // The spectral envelope (melodic, preserving formants, when the pitch moves here).
                const bool formants = !poly && options.preserveFormants && std::abs(frameOffset[mu]) > 0.01;
                if (formants)
                {
                    for (int k = 0; k < bins; ++k)
                        logSpectrum[static_cast<std::size_t>(k)] = {static_cast<float>(std::log(mag[static_cast<std::size_t>(k)] + loudest * 1.0e-6)), 0.0f};
                    fft.inverse(logSpectrum.data(), cepstrum.data());
                    // Liftered under the pitch period of the note sounding.
                    double f0 = 200.0;
                    for (const auto* e : order_)
                        if (t >= e->note.start && t < e->note.end)
                            f0 = midiToHz(e->note.pitch + e->note.driftAt(t));
                    const int cutoff = std::clamp(static_cast<int>(0.6 * sampleRate / f0), 12, n / 8);
                    for (int q = cutoff + 1; q < n - cutoff; ++q)
                        cepstrum[static_cast<std::size_t>(q)] = 0.0f;
                    fft.forward(cepstrum.data(), logSpectrum.data());
                    for (int k = 0; k < bins; ++k)
                        envelope[static_cast<std::size_t>(k)] = static_cast<double>(logSpectrum[static_cast<std::size_t>(k)].real());
                }

                int regionStart = 0;
                for (std::size_t j = 0; j < peaks.size(); ++j)
                {
                    const int k = peaks[j];
                    int regionEnd = bins - 1;
                    if (j + 1 < peaks.size())
                    {
                        regionEnd = k;
                        for (int q = k; q < peaks[j + 1]; ++q)
                            if (mag[static_cast<std::size_t>(q)] < mag[static_cast<std::size_t>(regionEnd)])
                                regionEnd = q;
                    }
                    const auto ku = static_cast<std::size_t>(k);
                    const double expected = kTwoPi * k * lag / n;
                    const double omega = kTwoPi * k / n + princarg(std::arg(x2[ku]) - phase1[ku] - expected) / lag;

                    // How far this peak moves, and its gain.
                    double offset = frameOffset[mu];
                    double gainDb = frameGain[mu];
                    if (poly)
                    {
                        offset = 0.0;
                        gainDb = 0.0;
                        const double hz = omega * sampleRate / kTwoPi;
                        double bestCents = 40.0;
                        for (const auto* e : order_)
                        {
                            if (t < e->note.start - 0.05 || t > e->note.end + 0.05 || !e->edited())
                                continue;
                            const double f0 = midiToHz(e->note.pitch + e->note.driftAt(t));
                            const double h = std::round(hz / f0);
                            if (h < 1.0 || h > 40.0)
                                continue;
                            const double cents = std::abs(1200.0 * std::log2(hz / (h * f0)));
                            if (cents < bestCents)
                            {
                                bestCents = cents;
                                // Faded in and out over 50 ms beyond the note's ends.
                                const double outside = std::max(e->note.start - t, t - e->note.end);
                                const double w = outside > 0.0 ? std::clamp(1.0 - outside / 0.05, 0.0, 1.0) : 1.0;
                                offset = e->offsetAt(t) * w;
                                gainDb = e->gainDb * w;
                            }
                        }
                    }
                    const double ratio = std::exp2(offset / 12.0);
                    const double omegaOut = omega * ratio;
                    const int shift = static_cast<int>(std::lround(omegaOut * n / kTwoPi)) - k;

                    // The phase, carried on from the same peak in the previous frame.
                    double psi = phase1[ku];
                    const Peak* match = nullptr;
                    double distance = 1.5 * kTwoPi / n;
                    for (const auto& q : previous)
                        if (std::abs(q.omega - omega) < distance)
                        {
                            distance = std::abs(q.omega - omega);
                            match = &q;
                        }
                    if (match != nullptr)
                    {
                        psi = match->psi + 0.5 * (match->omegaOut + omegaOut) * hop;
                        if (std::abs(ratio - 1.0) < 1.0e-6 && steady[mu])
                            psi += 0.5 * princarg(phase1[ku] - psi); // (back to the original's phase)
                    }
                    current.push_back({omega, omegaOut, psi});
                    const auto rotation = std::polar(static_cast<float>(juce::Decibels::decibelsToGain(gainDb, -120.0)), static_cast<float>(psi - phase1[ku]));
                    for (int q = regionStart; q <= regionEnd; ++q)
                    {
                        const int destination = q + shift;
                        if (destination < 0 || destination >= bins)
                            continue;
                        auto value = x1[static_cast<std::size_t>(q)] * rotation;
                        if (formants)
                            value *= static_cast<float>(std::clamp(
                                std::exp(envelope[static_cast<std::size_t>(destination)] - envelope[static_cast<std::size_t>(q)]), 0.05, 20.0));
                        y[static_cast<std::size_t>(destination)] += value;
                    }
                    regionStart = regionEnd + 1;
                }
            }
            fft.inverse(y.data(), synth.data());
            float* target = ola.data() + static_cast<std::ptrdiff_t>(m) * hop;
            for (int i = 0; i < n; ++i)
                target[i] += synth[static_cast<std::size_t>(i)] * window[static_cast<std::size_t>(i)] * kOverlapGain;
            std::swap(previous, current);
        }
        const auto offset = static_cast<std::size_t>(a - base);
        std::copy(ola.begin() + static_cast<std::ptrdiff_t>(offset), ola.begin() + static_cast<std::ptrdiff_t>(offset) + length, out.getWritePointer(ch));
    }
    return out;
}

bool Renderer::render(const juce::AudioBuffer<float>& input, double sampleRate, const std::vector<Edit>& edits, const RenderOptions& options,
                      juce::AudioBuffer<float>& output, const std::atomic<bool>* cancel)
{
    spansRendered_ = 0;
    output.makeCopyOf(input);
    const int total = input.getNumSamples();
    const double duration = static_cast<double>(total) / sampleRate;
    std::map<std::uint64_t, juce::AudioBuffer<float>> used;
    const int fade = std::max(1, static_cast<int>(std::lround(kFade * sampleRate)));
    for (const auto& span : editedSpans(edits, options, duration))
    {
        const auto key = spanKey(edits, options, span);
        juce::AudioBuffer<float> rendered;
        if (const auto found = cache_.find(key); found != cache_.end())
            rendered = found->second;
        else
        {
            rendered = renderSpan(input, sampleRate, edits, options, span, cancel);
            if (isCancelled(cancel))
                return false;
            ++spansRendered_;
        }
        const auto a = static_cast<int>(std::floor(span.from * sampleRate));
        const int length = rendered.getNumSamples();
        for (int ch = 0; ch < std::min(output.getNumChannels(), rendered.getNumChannels()); ++ch)
        {
            const float* in = input.getReadPointer(ch);
            const float* r = rendered.getReadPointer(ch);
            float* o = output.getWritePointer(ch);
            for (int i = 0; i < length && a + i < total; ++i)
            {
                if (a + i < 0)
                    continue;
                const float w = std::min({1.0f, static_cast<float>(i) / static_cast<float>(fade), static_cast<float>(length - 1 - i) / static_cast<float>(fade)});
                o[a + i] = in[a + i] * (1.0f - w) + r[i] * w;
            }
        }
        used.emplace(key, std::move(rendered));
    }
    cache_ = std::move(used);
    return true;
}

} // namespace dojo::audioedit::pitch
