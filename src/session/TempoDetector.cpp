#include "session/TempoDetector.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace dojo::session::tempo
{
namespace
{
constexpr double kAnalysisRate = 22050.0;
constexpr int kFftOrder = 10;
constexpr int kFftSize = 1 << kFftOrder; // 46 ms
constexpr int kHop = 128;                // 5.8 ms
constexpr int kGridDivisions = 4;        // sixteenth notes

/** `in` at `rate` resampled to the analysis rate (a moving average first when decimating). */
std::vector<float> toAnalysisRate(const float* in, int numSamples, double rate)
{
    if (numSamples <= 0)
        return {};
    if (std::abs(rate - kAnalysisRate) < 1.0e-6)
        return std::vector<float>(in, in + numSamples);

    std::vector<float> filtered(in, in + numSamples);
    const int width = std::max(1, static_cast<int>(std::round(rate / kAnalysisRate)));
    if (width > 1)
    {
        double sum = 0.0;
        for (int i = 0; i < numSamples; ++i)
        {
            sum += in[i];
            if (i >= width)
                sum -= in[i - width];
            filtered[static_cast<std::size_t>(i)] = static_cast<float>(sum / width);
        }
    }
    const auto count = static_cast<int>(std::floor(static_cast<double>(numSamples) * kAnalysisRate / rate));
    std::vector<float> out(static_cast<std::size_t>(std::max(0, count)));
    for (int k = 0; k < count; ++k)
    {
        const double position = static_cast<double>(k) * rate / kAnalysisRate;
        const auto i = static_cast<int>(position);
        const double frac = position - i;
        const float a = filtered[static_cast<std::size_t>(i)];
        const float b = filtered[static_cast<std::size_t>(std::min(i + 1, numSamples - 1))];
        out[static_cast<std::size_t>(k)] = static_cast<float>(a + (b - a) * frac);
    }
    return out;
}

/** Minus the local mean (half a second either side), half-wave rectified. */
std::vector<double> rectifyAroundMean(const std::vector<double>& odf)
{
    const auto frames = static_cast<int>(odf.size());
    const int half = static_cast<int>(0.5 * kAnalysisRate / kHop);
    std::vector<double> prefix(odf.size() + 1, 0.0);
    for (std::size_t i = 0; i < odf.size(); ++i)
        prefix[i + 1] = prefix[i] + odf[i];
    std::vector<double> out(odf.size());
    for (int i = 0; i < frames; ++i)
    {
        const int a = std::max(0, i - half);
        const int b = std::min(frames, i + half + 1);
        const double mean = (prefix[static_cast<std::size_t>(b)] - prefix[static_cast<std::size_t>(a)]) / (b - a);
        out[static_cast<std::size_t>(i)] = std::max(0.0, odf[static_cast<std::size_t>(i)] - mean);
    }
    return out;
}

/** Spectral flux of log-compressed magnitudes, one value per hop: of the whole spectrum (onsets of
    everything), and of the band below 300 Hz (the kick and the body of the snare, which mark the
    beat rather than its subdivisions). */
struct OnsetStrength
{
    std::vector<double> full;
    std::vector<double> low;
};

OnsetStrength onsetStrength(const std::vector<float>& x)
{
    const int frames = x.size() >= static_cast<std::size_t>(kFftSize) ? static_cast<int>((x.size() - kFftSize) / kHop) + 1 : 0;
    std::vector<double> odf(static_cast<std::size_t>(std::max(0, frames)), 0.0);
    std::vector<double> low(odf.size(), 0.0);
    if (frames < 2)
        return {odf, low};
    const int lowBins = static_cast<int>(300.0 / (kAnalysisRate / kFftSize));

    juce::dsp::FFT fft(kFftOrder);
    std::vector<float> window(kFftSize);
    for (int i = 0; i < kFftSize; ++i)
        window[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * juce::MathConstants<double>::pi * i / kFftSize));
    std::vector<float> buffer(static_cast<std::size_t>(2 * kFftSize));
    std::vector<double> previous(kFftSize / 2 + 1, 0.0);

    for (int f = 0; f < frames; ++f)
    {
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        const auto* frame = x.data() + static_cast<std::ptrdiff_t>(f) * kHop;
        for (int i = 0; i < kFftSize; ++i)
            buffer[static_cast<std::size_t>(i)] = frame[i] * window[static_cast<std::size_t>(i)];
        fft.performFrequencyOnlyForwardTransform(buffer.data(), true);
        double flux = 0.0;
        double lowFlux = 0.0;
        for (int b = 1; b <= kFftSize / 2; ++b)
        {
            const double y = std::log1p(100.0 * buffer[static_cast<std::size_t>(b)]);
            const double rise = std::max(0.0, y - previous[static_cast<std::size_t>(b)]);
            flux += rise;
            if (b <= lowBins)
                lowFlux += rise;
            previous[static_cast<std::size_t>(b)] = y;
        }
        odf[static_cast<std::size_t>(f)] = f == 0 ? 0.0 : flux;
        low[static_cast<std::size_t>(f)] = f == 0 ? 0.0 : lowFlux;
    }
    return {rectifyAroundMean(odf), rectifyAroundMean(low)};
}

double secondsPerHop() { return kHop / kAnalysisRate; }

/** Value of `r` at a fractional index (linear). */
double at(const std::vector<double>& r, double index)
{
    if (index < 0.0 || index >= static_cast<double>(r.size() - 1))
        return 0.0;
    const auto i = static_cast<std::size_t>(index);
    const double frac = index - static_cast<double>(i);
    return r[i] + (r[i + 1] - r[i]) * frac;
}

/** The autocorrelation of an onset strength up to `maxLag` hops, normalised to 1 at lag 0 (empty if
    it is silent). */
std::vector<double> autocorrelation(const std::vector<double>& odf, int maxLag)
{
    std::vector<double> r(static_cast<std::size_t>(std::max(0, maxLag + 1)), 0.0);
    const auto n = static_cast<int>(odf.size());
    for (int lag = 0; lag <= maxLag && lag < n; ++lag)
    {
        double sum = 0.0;
        for (int i = 0; i + lag < n; ++i)
            sum += odf[static_cast<std::size_t>(i)] * odf[static_cast<std::size_t>(i + lag)];
        r[static_cast<std::size_t>(lag)] = sum / (n - lag);
    }
    if (r.empty() || r[0] <= 1.0e-12)
        return {};
    const double zero = r[0];
    for (auto& v : r)
        v /= zero;
    return r;
}

/** The beat tempo the onsets repeat at most: a comb over 4 multiples of each candidate period in the
    autocorrelations of the whole spectrum and of the low band (when it has onsets), weighted by
    the tempo prior. */
double coarseTempo(const OnsetStrength& onsets, double& strength)
{
    const double hop = secondsPerHop();
    const double slowest = 40.0;
    const int maxLag = std::min(static_cast<int>(onsets.full.size()) - 2, static_cast<int>(std::ceil(4.0 * 60.0 / slowest / hop)) + 2);
    const auto full = autocorrelation(onsets.full, maxLag);
    const auto low = autocorrelation(onsets.low, maxLag);
    if (full.empty())
    {
        strength = 0.0;
        return 0.0;
    }

    double best = 0.0;
    double bestScore = -1.0;
    double bestRaw = 0.0;
    for (double bpm = 40.0; bpm <= 250.0; bpm += 0.05)
    {
        const double lag = 60.0 / bpm / hop;
        double raw = 0.0;
        for (int k = 1; k <= 4; ++k)
            raw += at(full, k * lag) + (low.empty() ? 0.0 : at(low, k * lag));
        raw /= low.empty() ? 4.0 : 8.0;
        const double octaves = std::log2(bpm / 120.0);
        const double score = raw * std::exp(-0.5 * octaves * octaves); // (prior: an octave either side of 120)
        if (score > bestScore)
        {
            bestScore = score;
            best = bpm;
            bestRaw = raw;
        }
    }
    strength = bestRaw;
    return best;
}

/** Onset candidates: local maxima of the onset strength above an adaptive threshold (frames). */
std::vector<int> pickPeaks(const std::vector<double>& odf)
{
    if (odf.empty())
        return {};
    const double mean = std::accumulate(odf.begin(), odf.end(), 0.0) / static_cast<double>(odf.size());
    double variance = 0.0;
    for (const double v : odf)
        variance += (v - mean) * (v - mean);
    const double deviation = std::sqrt(variance / static_cast<double>(odf.size()));
    const double threshold = mean + 0.5 * deviation;
    const int radius = std::max(1, static_cast<int>(0.03 / secondsPerHop())); // 30 ms

    std::vector<int> peaks;
    for (int i = 1; i + 1 < static_cast<int>(odf.size()); ++i)
    {
        const double v = odf[static_cast<std::size_t>(i)];
        if (v <= threshold)
            continue;
        bool isMax = true;
        for (int j = std::max(0, i - radius); j <= std::min(static_cast<int>(odf.size()) - 1, i + radius) && isMax; ++j)
            isMax = j == i || odf[static_cast<std::size_t>(j)] < v || (j > i && odf[static_cast<std::size_t>(j)] <= v);
        if (isMax)
            peaks.push_back(i);
    }
    return peaks;
}

struct Onset
{
    double seconds = 0.0;
    double strength = 0.0;
};

/** Each peak's attack, to the sample: where the energy envelope first passes 30 % of the way from
    the level before the hit to its peak. */
std::vector<Onset> refineOnsets(const std::vector<float>& x, const std::vector<double>& odf, const std::vector<int>& peaks)
{
    // Energy envelope: a 10 ms moving average of the square (longer than a cycle of most bass notes,
    // so it follows each note's envelope, not its waveform).
    const int smooth = std::max(1, static_cast<int>(0.010 * kAnalysisRate));
    std::vector<double> energy(x.size(), 0.0);
    double sum = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        sum += static_cast<double>(x[i]) * x[i];
        if (i >= static_cast<std::size_t>(smooth))
            sum -= static_cast<double>(x[i - static_cast<std::size_t>(smooth)]) * x[i - static_cast<std::size_t>(smooth)];
        energy[i] = std::max(0.0, sum / smooth);
    }

    std::vector<Onset> onsets;
    const auto n = static_cast<int>(energy.size());
    for (const int p : peaks)
    {
        // The frame's window (and a little before): the attack is somewhere in it.
        const int from = std::max(0, p * kHop - static_cast<int>(0.005 * kAnalysisRate));
        const int to = std::min(n - 1, p * kHop + kFftSize);
        if (to <= from + 2)
            continue;
        int top = from;
        for (int i = from; i <= to; ++i)
            if (energy[static_cast<std::size_t>(i)] > energy[static_cast<std::size_t>(top)])
                top = i;
        const int lookBack = std::max(0, top - static_cast<int>(0.04 * kAnalysisRate));
        double base = energy[static_cast<std::size_t>(top)];
        for (int i = lookBack; i <= top; ++i)
            base = std::min(base, energy[static_cast<std::size_t>(i)]);
        const double peakValue = energy[static_cast<std::size_t>(top)];
        if (peakValue <= base * 1.5 + 1.0e-12)
            continue; // no attack in it
        const double threshold = base + 0.3 * (peakValue - base);
        int j = top;
        while (j > lookBack && energy[static_cast<std::size_t>(j - 1)] >= threshold)
            --j;
        double seconds = j;
        if (j > 0)
        {
            const double a = energy[static_cast<std::size_t>(j - 1)];
            const double b = energy[static_cast<std::size_t>(j)];
            seconds = (j - 1) + (b > a ? (threshold - a) / (b - a) : 1.0);
        }
        onsets.push_back({seconds / kAnalysisRate, odf[static_cast<std::size_t>(p)]});
    }

    // In time order; one hit found twice keeps the stronger.
    std::sort(onsets.begin(), onsets.end(), [](const Onset& a, const Onset& b) { return a.seconds < b.seconds; });
    std::vector<Onset> unique;
    for (const auto& o : onsets)
    {
        if (!unique.empty() && o.seconds - unique.back().seconds < 0.03)
        {
            if (o.strength > unique.back().strength)
                unique.back() = o;
            continue;
        }
        unique.push_back(o);
    }
    return unique;
}

struct GridFit
{
    double unit = 0.0;   ///< seconds per grid step
    double origin = 0.0; ///< time of grid step 0
    int fitted = 0;
    double rmsResidual = 1.0; ///< in grid steps
};

/** Weighted least squares (weights: onset strength, so the hits outweigh their tails) on (grid
    number, time) of the onsets within `tolerance` grid steps of the grid; then again without those
    further off than three times the residual of the first fit. */
bool fitGrid(const std::vector<Onset>& onsets, double origin, double unit, double windowEnd, GridFit& fit)
{
    const auto fitOnce = [&](double fromOrigin, double fromUnit, double tolerance, GridFit& out)
    {
        double w = 0.0, sn = 0.0, st = 0.0, snn = 0.0, snt = 0.0;
        int count = 0;
        for (const auto& o : onsets)
        {
            if (o.seconds > windowEnd)
                break;
            const double position = (o.seconds - fromOrigin) / fromUnit;
            const double number = std::round(position);
            if (std::abs(position - number) > tolerance)
                continue; // off the grid (a flam, a triplet, a tail, noise)
            const double weight = std::max(1.0e-9, o.strength);
            w += weight;
            sn += weight * number;
            st += weight * o.seconds;
            snn += weight * number * number;
            snt += weight * number * o.seconds;
            ++count;
        }
        const double denominator = w * snn - sn * sn;
        if (count < 4 || std::abs(denominator) < 1.0e-12 * w * w)
            return false;
        out.unit = (w * snt - sn * st) / denominator;
        out.origin = (st - out.unit * sn) / w;
        out.fitted = count;
        if (out.unit <= 0.0)
            return false;

        double squares = 0.0;
        double weights = 0.0;
        for (const auto& o : onsets)
        {
            if (o.seconds > windowEnd)
                break;
            const double position = (o.seconds - out.origin) / out.unit;
            const double residual = position - std::round(position);
            if (std::abs(residual) <= tolerance)
            {
                squares += o.strength * residual * residual;
                weights += o.strength;
            }
        }
        out.rmsResidual = weights > 0.0 ? std::sqrt(squares / weights) : 1.0;
        return true;
    };

    GridFit first;
    if (!fitOnce(origin, unit, 0.2, first))
        return false;
    fit = first;
    GridFit second;
    if (fitOnce(first.origin, first.unit, std::clamp(3.0 * first.rmsResidual, 0.05, 0.2), second))
        fit = second;
    return true;
}

double fold(double bpm, double minBpm, double maxBpm)
{
    while (bpm > maxBpm && bpm / 2.0 >= minBpm * 0.999)
        bpm /= 2.0;
    while (bpm < minBpm && bpm * 2.0 <= maxBpm * 1.001)
        bpm *= 2.0;
    return bpm;
}
} // namespace

Estimate estimate(const float* samples, int numSamples, double sampleRate, double minBpm, double maxBpm)
{
    Estimate result;
    const auto x = toAnalysisRate(samples, numSamples, sampleRate);
    if (x.size() < static_cast<std::size_t>(3.0 * kAnalysisRate))
    {
        result.problem = "Too short (at least 3 seconds are needed)";
        return result;
    }

    const auto strengths = onsetStrength(x);
    const auto& odf = strengths.full;
    double strength = 0.0;
    const double coarse = coarseTempo(strengths, strength);
    if (coarse <= 0.0 || strength < 0.02)
    {
        result.problem = "No clear beat";
        return result;
    }

    // Number the onsets on the sixteenth grid, from a strong early one, over a growing window.
    const auto onsets = refineOnsets(x, odf, pickPeaks(odf));
    GridFit fit;
    bool fitted = false;
    if (onsets.size() >= 4)
    {
        const double total = onsets.back().seconds;
        std::size_t anchor = 0;
        for (std::size_t i = 0; i < onsets.size() && onsets[i].seconds < onsets.front().seconds + 4.0; ++i)
            if (onsets[i].strength > onsets[anchor].strength)
                anchor = i;
        double unit = 60.0 / coarse / kGridDivisions;
        double origin = onsets[anchor].seconds;
        double windowEnd = origin + std::max(4.0, 8.0 * 60.0 / coarse);
        for (int pass = 0; pass < 40; ++pass)
        {
            GridFit next;
            if (!fitGrid(onsets, origin, unit, windowEnd, next))
                break;
            fit = next;
            fitted = true;
            unit = fit.unit;
            origin = fit.origin;
            if (windowEnd >= total)
            {
                // A last pass with the final grid over everything.
                if (fitGrid(onsets, origin, unit, total + 1.0, next))
                    fit = next;
                break;
            }
            windowEnd = std::min(total + 1.0, origin + (windowEnd - origin) * 1.6);
        }
    }

    const int candidates = static_cast<int>(onsets.size());
    // (five hits two seconds apart already give an exact line)
    const double span = onsets.empty() ? 0.0 : onsets.back().seconds - onsets.front().seconds;
    if (fitted && fit.fitted >= 5 && span >= 2.0)
    {
        result.bpm = fold(60.0 / (fit.unit * kGridDivisions), minBpm, maxBpm);
        result.onsets = fit.fitted;
        const double share = static_cast<double>(fit.fitted) / std::max(1, candidates);
        result.confidence = juce::jlimit(0.0, 1.0, share * (1.0 - std::min(1.0, fit.rmsResidual / 0.2)));
    }
    else
    {
        // Too few clear onsets for the grid: the autocorrelation's tempo.
        result.bpm = fold(coarse, minBpm, maxBpm);
        result.onsets = candidates;
        result.confidence = juce::jlimit(0.0, 0.3, strength);
    }
    result.valid = result.bpm > 0.0;
    return result;
}

TrackMaterial takeMaterial(const Session&, const juce::ValueTree& track)
{
    TrackMaterial material;
    material.trackId = track::getId(track);
    material.name = track::getName(track);
    const auto kind = track::getKind(track);
    if (holdsMidi(kind))
    {
        material.problem = "MIDI follows the project tempo";
        return material;
    }
    if (!holdsAudio(kind))
    {
        material.problem = "Not an audio track";
        return material;
    }
    for (const auto& c : track)
    {
        if (!c.hasType(ids::clip))
            continue;
        ClipRegion region;
        region.file = clip::getFile(c);
        region.start = clip::getStart(c);
        region.length = clip::getLength(c);
        region.offset = clip::getOffset(c);
        region.stretch = std::max(kMinStretch, clip::getStretch(c));
        region.loopLength = clip::isLooped(c) ? clip::getLoopLength(c) : 0.0;
        region.sourceChannel = clip::getSourceChannel(c);
        material.clips.push_back(region);
    }
    if (material.clips.empty())
        material.problem = "No audio clips";
    return material;
}

std::vector<float> mixMaterial(const TrackMaterial& material, juce::AudioFormatManager& formats, double sampleRate, double maxSeconds,
                               juce::String& error)
{
    if (material.clips.empty())
        return {};
    double first = 1.0e12;
    double last = 0.0;
    for (const auto& c : material.clips)
    {
        first = std::min(first, c.start);
        last = std::max(last, c.start + c.length);
    }
    last = std::min(last, first + maxSeconds);
    const auto total = static_cast<std::size_t>(std::max(0.0, (last - first) * sampleRate));
    std::vector<float> out(total, 0.0f);

    for (const auto& c : material.clips)
    {
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(c.file));
        if (reader == nullptr || reader->sampleRate <= 0.0)
        {
            error = c.file.existsAsFile() ? "Cannot read " + c.file.getFileName() : c.file.getFileName() + " is missing";
            continue;
        }
        const double clipEnd = std::min(c.start + c.length, last);
        if (clipEnd <= c.start)
            continue;
        // The part of the file the clip plays (one loop of a looped clip).
        const double played = c.loopLength > 0.0 ? c.loopLength : clipEnd - c.start;
        const auto firstFrame = static_cast<juce::int64>(std::floor(c.offset * reader->sampleRate));
        const auto frames = static_cast<int>(std::min<juce::int64>(reader->lengthInSamples - firstFrame,
                                                                   static_cast<juce::int64>(std::ceil(played / c.stretch * reader->sampleRate)) + 2));
        if (frames <= 1)
            continue;
        const int channels = static_cast<int>(reader->numChannels);
        juce::AudioBuffer<float> source(channels, frames);
        reader->read(&source, 0, frames, firstFrame, true, true);
        std::vector<float> mono(static_cast<std::size_t>(frames), 0.0f);
        for (int ch = 0; ch < channels; ++ch)
        {
            if (c.sourceChannel >= 0 && ch != c.sourceChannel)
                continue;
            const float weight = c.sourceChannel >= 0 ? 1.0f : 1.0f / static_cast<float>(channels);
            const float* data = source.getReadPointer(ch);
            for (int i = 0; i < frames; ++i)
                mono[static_cast<std::size_t>(i)] += weight * data[i];
        }

        // Onto the timeline at the analysis rate.
        const auto from = static_cast<std::size_t>(std::max(0.0, (c.start - first) * sampleRate));
        const auto to = std::min(total, static_cast<std::size_t>((clipEnd - first) * sampleRate));
        for (std::size_t k = from; k < to; ++k)
        {
            double local = static_cast<double>(k) / sampleRate + first - c.start;
            if (c.loopLength > 0.0)
                local = std::fmod(local, c.loopLength);
            const double position = local / c.stretch * reader->sampleRate;
            const auto i = static_cast<int>(position);
            if (i < 0 || i + 1 >= frames)
                continue;
            const double frac = position - i;
            out[k] += static_cast<float>(mono[static_cast<std::size_t>(i)] + (mono[static_cast<std::size_t>(i + 1)] - mono[static_cast<std::size_t>(i)]) * frac);
        }
    }
    return out;
}

Combined combine(std::vector<TrackResult> results)
{
    Combined combined;
    std::vector<double> values;
    for (const auto& r : results)
        if (r.estimate.valid)
            values.push_back(r.estimate.bpm);
    if (!values.empty())
    {
        auto sorted = values;
        std::sort(sorted.begin(), sorted.end());
        const double median = sorted.size() % 2 == 1 ? sorted[sorted.size() / 2]
                                                     : 0.5 * (sorted[sorted.size() / 2 - 1] + sorted[sorted.size() / 2]);
        double sum = 0.0;
        for (auto& r : results)
        {
            if (!r.estimate.valid)
                continue;
            // An octave apart from the others (half- or double-time): moved to agree with them.
            double bpm = r.estimate.bpm;
            while (bpm / median > std::sqrt(2.0))
                bpm /= 2.0;
            while (median / bpm > std::sqrt(2.0))
                bpm *= 2.0;
            r.folded = bpm;
            sum += bpm;
            ++combined.counted;
        }
        combined.average = sum / combined.counted;
        combined.valid = true;
    }
    combined.tracks = std::move(results);
    return combined;
}

Combined analyse(const std::vector<TrackMaterial>& tracks, double minBpm, double maxBpm, const std::atomic<bool>* cancel)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::vector<TrackResult> results;
    for (const auto& material : tracks)
    {
        if (cancel != nullptr && cancel->load())
            break;
        TrackResult result;
        result.trackId = material.trackId;
        result.name = material.name;
        if (material.problem.isNotEmpty())
            result.estimate.problem = material.problem;
        else
        {
            juce::String error;
            const auto audio = mixMaterial(material, formats, kAnalysisRate, 120.0, error);
            result.estimate = estimate(audio.data(), static_cast<int>(audio.size()), kAnalysisRate, minBpm, maxBpm);
            if (!result.estimate.valid && error.isNotEmpty())
                result.estimate.problem = error;
        }
        result.folded = result.estimate.bpm;
        results.push_back(result);
    }
    return combine(std::move(results));
}

} // namespace dojo::session::tempo
