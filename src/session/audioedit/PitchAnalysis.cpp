#include "session/audioedit/PitchAnalysis.h"

#include "session/audioedit/Stft.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <numeric>
#include <tuple>

namespace dojo::audioedit::pitch
{
namespace
{
constexpr double kNoPitch = std::numeric_limits<double>::quiet_NaN();

bool isCancelled(const std::atomic<bool>* cancel) noexcept { return cancel != nullptr && cancel->load(std::memory_order_relaxed); }

double medianOf(std::vector<double> v)
{
    if (v.empty())
        return 0.0;
    const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
    std::nth_element(v.begin(), mid, v.end());
    return *mid;
}

double interpolate(const std::vector<float>& values, double hop, double t) noexcept
{
    if (values.empty())
        return 0.0;
    const double x = t / hop;
    if (x <= 0.0)
        return values.front();
    const auto i = static_cast<std::size_t>(x);
    if (i + 1 >= values.size())
        return values.back();
    const double f = x - static_cast<double>(i);
    return static_cast<double>(values[i]) + (static_cast<double>(values[i + 1]) - static_cast<double>(values[i])) * f;
}

template <typename T>
T& at(std::vector<T>& v, int i)
{
    return v[static_cast<std::size_t>(i)];
}
template <typename T>
const T& at(const std::vector<T>& v, int i)
{
    return v[static_cast<std::size_t>(i)];
}

/** The note of frames [first, last] of a pitch curve (MIDI, NaN where unknown: interpolated). */
Note makeNote(const std::vector<double>& midi, const std::vector<double>& level, int first, int last, double hop)
{
    Note note;
    note.hop = hop;
    note.start = static_cast<double>(first) * hop;
    note.end = static_cast<double>(last + 1) * hop;
    // The centre: the median of the middle 70 % (not the scoop in or the fall out).
    const int length = last - first + 1;
    const int trim = length * 15 / 100;
    std::vector<double> middle;
    for (int i = first + trim; i <= last - trim; ++i)
        if (!std::isnan(at(midi, i)))
            middle.push_back(at(midi, i));
    if (middle.empty())
        for (int i = first; i <= last; ++i)
            if (!std::isnan(at(midi, i)))
                middle.push_back(at(midi, i));
    note.pitch = medianOf(middle);
    double held = note.pitch;
    for (int i = first; i <= last; ++i)
    {
        if (!std::isnan(at(midi, i)))
            held = at(midi, i);
        note.drift.push_back(static_cast<float>(held - note.pitch));
        note.level.push_back(static_cast<float>(at(level, i)));
    }
    return note;
}
} // namespace

double Note::driftAt(double t) const noexcept { return interpolate(drift, hop, t - start); }
double Note::levelAt(double t) const noexcept { return interpolate(level, hop, t - start); }

// --- Monophonic: YIN ---------------------------------------------------------------------------------------

std::vector<Frame> trackPitch(const float* samples, int numSamples, double sampleRate, double hopSeconds, double minHz, double maxHz,
                              const std::atomic<bool>* cancel, const std::function<void(float)>& progress)
{
    std::vector<Frame> frames;
    if (samples == nullptr || numSamples <= 0 || sampleRate <= 0.0)
        return frames;
    const int tauMax = std::min(static_cast<int>(std::ceil(sampleRate / minHz)), 8192);
    const int tauMin = std::max(2, static_cast<int>(std::floor(sampleRate / maxHz)));
    const int window = std::max(tauMax, static_cast<int>(std::lround(sampleRate * 0.02)));
    const int span = window + tauMax + 2;
    RealFft fft(orderFor(span));
    const int size = fft.size();
    const double hop = hopSeconds * sampleRate;
    const int count = static_cast<int>(std::floor(static_cast<double>(numSamples - 1) / hop)) + 1;
    frames.resize(static_cast<std::size_t>(count));

    std::vector<float> a(static_cast<std::size_t>(size)), b(static_cast<std::size_t>(size)), corr(static_cast<std::size_t>(size));
    std::vector<std::complex<float>> fa(static_cast<std::size_t>(fft.bins())), fb(static_cast<std::size_t>(fft.bins()));
    std::vector<double> prefix(static_cast<std::size_t>(span + 1)), dn(static_cast<std::size_t>(tauMax + 2));
    for (int i = 0; i < count; ++i)
    {
        if ((i & 63) == 0)
        {
            if (isCancelled(cancel))
                return {};
            if (progress)
                progress(static_cast<float>(i) / static_cast<float>(count));
        }
        const auto centre = static_cast<long long>(std::llround(i * hop));
        const long long first = centre - window / 2;
        std::fill(a.begin(), a.end(), 0.0f);
        std::fill(b.begin(), b.end(), 0.0f);
        prefix[0] = 0.0;
        for (int j = 0; j < span; ++j)
        {
            const long long index = first + j;
            const float v = index >= 0 && index < numSamples ? samples[index] : 0.0f;
            at(b, j) = v;
            at(prefix, j + 1) = at(prefix, j) + static_cast<double>(v) * static_cast<double>(v);
        }
        std::copy(b.begin(), b.begin() + window, a.begin());
        fft.forward(a.data(), fa.data());
        fft.forward(b.data(), fb.data());
        for (std::size_t k = 0; k < fa.size(); ++k)
            fa[k] = std::conj(fa[k]) * fb[k];
        fft.inverse(fa.data(), corr.data());

        auto& frame = at(frames, i);
        const double e0 = at(prefix, window);
        frame.rms = static_cast<float>(std::sqrt(e0 / window));
        dn[0] = 1.0;
        double running = 0.0;
        for (int tau = 1; tau <= tauMax; ++tau)
        {
            const double e = at(prefix, tau + window) - at(prefix, tau);
            const double d = std::max(0.0, e0 + e - 2.0 * static_cast<double>(at(corr, tau)));
            running += d;
            at(dn, tau) = running > 0.0 ? d * tau / running : 1.0;
        }
        int best = -1;
        for (int tau = tauMin; tau < tauMax; ++tau)
            if (at(dn, tau) < 0.15)
            {
                while (tau + 1 < tauMax && at(dn, tau + 1) < at(dn, tau))
                    ++tau;
                best = tau;
                break;
            }
        if (best < 0)
        {
            best = tauMin;
            for (int tau = tauMin; tau < tauMax; ++tau)
                if (at(dn, tau) < at(dn, best))
                    best = tau;
        }
        double tau = best;
        if (best > 1 && best < tauMax)
        {
            const double s0 = at(dn, best - 1), s1 = at(dn, best), s2 = at(dn, best + 1);
            const double denominator = s0 - 2.0 * s1 + s2;
            if (denominator > 1.0e-12)
                tau = best + std::clamp(0.5 * (s0 - s2) / denominator, -1.0, 1.0);
        }
        frame.clarity = static_cast<float>(std::clamp(1.0 - at(dn, best), 0.0, 1.0));
        frame.f0 = at(dn, best) < 0.3 ? static_cast<float>(sampleRate / tau) : 0.0f;
    }

    // Silence (45 dB under the loudest frame) has no pitch.
    float loudest = 0.0f;
    for (const auto& f : frames)
        loudest = std::max(loudest, f.rms);
    for (auto& f : frames)
        if (f.rms < loudest * 0.0056f)
            f.f0 = 0.0f;

    // Octave errors: a frame an octave off the median of its voiced neighbours is folded back.
    std::vector<double> midi(frames.size(), kNoPitch);
    for (std::size_t i = 0; i < frames.size(); ++i)
        if (frames[i].f0 > 0.0f)
            midi[i] = hzToMidi(frames[i].f0);
    for (int i = 0; i < count; ++i)
    {
        if (std::isnan(at(midi, i)))
            continue;
        std::vector<double> around;
        for (int j = std::max(0, i - 8); j <= std::min(count - 1, i + 8); ++j)
            if (j != i && !std::isnan(at(midi, j)))
                around.push_back(at(midi, j));
        if (around.size() < 4)
            continue;
        const double m = medianOf(around);
        const double v = at(midi, i);
        if (std::abs(v - m) > 9.0)
            for (const double fold : {12.0, -12.0, 24.0, -24.0})
                if (std::abs(v + fold - m) < 3.0)
                {
                    at(frames, i).f0 = static_cast<float>(midiToHz(v + fold));
                    break;
                }
    }
    // Strays: voiced runs shorter than 3 frames.
    for (int i = 0; i < count;)
    {
        if (at(frames, i).f0 <= 0.0f)
        {
            ++i;
            continue;
        }
        int j = i;
        while (j < count && at(frames, j).f0 > 0.0f)
            ++j;
        if (j - i < 3)
            for (int k = i; k < j; ++k)
                at(frames, k).f0 = 0.0f;
        i = j;
    }
    if (progress)
        progress(1.0f);
    return frames;
}

std::vector<Note> segmentMelodic(const std::vector<Frame>& frames, double hop)
{
    const int count = static_cast<int>(frames.size());
    std::vector<double> midi(frames.size(), kNoPitch), level(frames.size(), 0.0), db(frames.size(), -120.0);
    float loudest = 1.0e-9f;
    for (const auto& f : frames)
        loudest = std::max(loudest, f.rms);
    for (int i = 0; i < count; ++i)
    {
        const auto& f = at(frames, i);
        if (f.f0 > 0.0f)
            at(midi, i) = hzToMidi(f.f0);
        at(level, i) = static_cast<double>(f.rms / loudest);
        at(db, i) = 20.0 * std::log10(std::max(1.0e-9, at(level, i)));
    }

    const auto frames_ = [hop](double seconds) { return std::max(1, static_cast<int>(std::lround(seconds / hop))); };
    const int smoothing = frames_(0.06);
    const int sustained = frames_(0.04);
    const int minimum = frames_(0.05);
    const int attackWindow = frames_(0.03);

    std::vector<std::pair<int, int>> segments;
    std::vector<bool> attacks; ///< whether each segment starts at a new attack
    for (int i = 0; i < count;)
    {
        if (std::isnan(at(midi, i)))
        {
            ++i;
            continue;
        }
        // A voiced run (gaps of up to two frames bridged).
        int last = i;
        for (int k = i + 1; k < count && k - last <= 3; ++k)
            if (!std::isnan(at(midi, k)))
                last = k;
        const int runStart = i;
        const int runEnd = last;
        std::vector<double> p;
        for (int k = runStart; k <= runEnd; ++k)
            p.push_back(at(midi, k));
        for (std::size_t k = 0; k < p.size(); ++k)
            if (std::isnan(p[k]))
            {
                std::size_t next = k;
                while (std::isnan(p[next]))
                    ++next;
                for (std::size_t g = k; g < next; ++g)
                    p[g] = p[k - 1] + (p[next] - p[k - 1]) * static_cast<double>(g - k + 1) / static_cast<double>(next - k + 1);
                k = next;
            }
        const int length = static_cast<int>(p.size());
        // The local pitch: a mean around the local median (vibrato averages out, stray frames do not count).
        std::vector<double> s(p.size());
        for (int k = 0; k < length; ++k)
        {
            std::vector<double> around;
            for (int j = std::max(0, k - smoothing); j <= std::min(length - 1, k + smoothing); ++j)
                around.push_back(at(p, j));
            const double m = medianOf(around);
            double sum = 0.0;
            int n = 0;
            for (const double v : around)
                if (std::abs(v - m) < 1.0)
                {
                    sum += v;
                    ++n;
                }
            at(s, k) = n > 0 ? sum / n : m;
        }

        // Cut where the averaged pitch leaves the note's centre for a while, and at new attacks.
        int segStart = 0;
        const auto centreOf = [&](int from, int to)
        {
            std::vector<double> v;
            for (int j = std::max(from, to - 30); j <= to; ++j)
                v.push_back(at(s, j));
            return medianOf(v);
        };
        double centre = centreOf(0, std::min(length - 1, minimum));
        int away = 0;
        bool attackStart = false; // (the segment being built began at an attack)
        for (int k = 1; k < length; ++k)
        {
            const int absolute = runStart + k;
            if (k - segStart >= 2 * minimum && k >= attackWindow)
            {
                int quietest = k - attackWindow;
                for (int j = k - attackWindow; j < k; ++j)
                    if (at(db, runStart + j) < at(db, runStart + quietest))
                        quietest = j;
                if (at(db, absolute) - at(db, runStart + quietest) >= 8.0 && at(db, absolute) > -40.0 && quietest > segStart + minimum)
                {
                    segments.emplace_back(runStart + segStart, runStart + quietest - 1);
                    attacks.push_back(segStart == 0 || attackStart);
                    attackStart = true;
                    segStart = quietest;
                    centre = at(s, k);
                    away = 0;
                    continue;
                }
            }
            if (std::abs(at(s, k) - centre) > 0.7)
                ++away;
            else
            {
                away = 0;
                centre = centreOf(segStart, k);
            }
            if (away >= sustained)
            {
                const double mid = 0.5 * (centre + at(s, k));
                int cut = k - sustained + 1;
                for (int q = std::max(segStart + 1, k - 3 * sustained); q <= k; ++q)
                    if ((at(p, q - 1) - mid) * (at(p, q) - mid) <= 0.0)
                    {
                        cut = q;
                        break;
                    }
                if (cut > segStart)
                {
                    segments.emplace_back(runStart + segStart, runStart + cut - 1);
                    attacks.push_back(segStart == 0 || attackStart);
                    attackStart = false;
                    segStart = cut;
                }
                centre = at(s, k);
                away = 0;
            }
        }
        segments.emplace_back(runStart + segStart, runEnd);
        attacks.push_back(segStart == 0 || attackStart);
        // (gap frames inside the run are part of the notes)
        for (int k = runStart; k <= runEnd; ++k)
            if (std::isnan(at(midi, k)))
                at(midi, k) = at(p, k - runStart);
        i = runEnd + 1;
    }

    // Neighbours that touch, on the same pitch, without a new attack between them: one note.
    {
        std::vector<std::pair<int, int>> joined;
        const auto centreOfSegment = [&](std::pair<int, int> seg)
        {
            std::vector<double> v;
            for (int k = seg.first; k <= seg.second; ++k)
                if (!std::isnan(at(midi, k)))
                    v.push_back(at(midi, k));
            return medianOf(v);
        };
        for (std::size_t k = 0; k < segments.size(); ++k)
        {
            if (!joined.empty() && joined.back().second + 1 == segments[k].first && !attacks[k]
                && std::abs(centreOfSegment(joined.back()) - centreOfSegment(segments[k])) < 0.4)
            {
                joined.back().second = segments[k].second;
                continue;
            }
            joined.push_back(segments[k]);
        }
        segments = std::move(joined);
    }

    // Too short: merged into the neighbour it touches (the previous one first), else dropped.
    std::vector<std::pair<int, int>> merged;
    for (std::size_t k = 0; k < segments.size(); ++k)
    {
        auto seg = segments[k];
        if (seg.second - seg.first + 1 >= minimum)
        {
            if (!merged.empty() && merged.back().second + 1 == seg.first && merged.back().second - merged.back().first + 1 < minimum)
            {
                seg.first = merged.back().first;
                merged.pop_back();
            }
            merged.push_back(seg);
        }
        else if (!merged.empty() && merged.back().second + 1 == seg.first)
            merged.back().second = seg.second;
        else if (k + 1 < segments.size() && segments[k + 1].first == seg.second + 1)
            segments[k + 1].first = seg.first;
    }

    std::vector<Note> notes;
    for (const auto& [first, last] : merged)
        if (last - first + 1 >= minimum)
            notes.push_back(makeNote(midi, level, first, last, hop));
    return notes;
}

// --- Polyphonic: iterative harmonic summation and cancellation ------------------------------------------------

std::vector<Note> detectPolyphonic(const float* samples, int numSamples, double sampleRate, int maxVoices, const std::atomic<bool>* cancel,
                                   const std::function<void(float)>& progress)
{
    std::vector<Note> notes;
    if (samples == nullptr || numSamples <= 0 || sampleRate <= 0.0)
        return notes;
    constexpr double kHop = 0.01;
    constexpr double kLowMidi = 33.0;
    constexpr double kHighMidi = 96.0;
    constexpr int kGrid = 10; // per semitone
    constexpr int kHarmonics = 16;

    int windowOrder = orderFor(sampleRate * 0.09);
    if ((1 << windowOrder) > sampleRate * 0.12)
        --windowOrder;
    const int windowLength = 1 << windowOrder;
    RealFft fft(windowOrder + 1);
    const int size = fft.size();
    const int bins = fft.bins();
    const auto window = hannWindow(windowLength);
    const double binHz = sampleRate / size;
    const double hop = kHop * sampleRate;
    const int count = static_cast<int>(std::floor(static_cast<double>(numSamples - 1) / hop)) + 1;

    // The log-frequency grid (a tenth of a semitone) that harmonics are read on.
    const double topMidi = std::min(kHighMidi + 12.0 * std::log2(static_cast<double>(kHarmonics)) + 1.0, hzToMidi(sampleRate * 0.47));
    const int gridSize = static_cast<int>((topMidi - kLowMidi) * kGrid) + 1;
    const int candidates = static_cast<int>((kHighMidi - kLowMidi) * kGrid) + 1;
    std::vector<int> gridLo(static_cast<std::size_t>(gridSize)), gridHi(static_cast<std::size_t>(gridSize));
    for (int q = 0; q < gridSize; ++q)
    {
        const double m = kLowMidi + static_cast<double>(q) / kGrid;
        const double centreBin = midiToHz(m) / binHz;
        at(gridLo, q) = std::clamp(static_cast<int>(std::floor(midiToHz(m - 0.3) / binHz)), 1, bins - 1);
        at(gridHi, q) = std::clamp(static_cast<int>(std::ceil(midiToHz(m + 0.3) / binHz)), 1, bins - 1);
        const int nearest = std::clamp(static_cast<int>(std::lround(centreBin)), 1, bins - 1);
        at(gridLo, q) = std::min(at(gridLo, q), nearest);
        at(gridHi, q) = std::max(at(gridHi, q), nearest);
    }
    std::array<int, kHarmonics + 1> harmonicOffset{};
    for (int h = 1; h <= kHarmonics; ++h)
        harmonicOffset[static_cast<std::size_t>(h)] = static_cast<int>(std::lround(12.0 * kGrid * std::log2(static_cast<double>(h))));

    std::vector<float> in(static_cast<std::size_t>(size));
    std::vector<std::complex<float>> spectrum(static_cast<std::size_t>(bins));
    std::vector<double> mag(static_cast<std::size_t>(bins)), prefix(static_cast<std::size_t>(bins + 1)), residual(static_cast<std::size_t>(bins)),
        logGrid(static_cast<std::size_t>(gridSize)), salience(static_cast<std::size_t>(candidates));
    struct Detection
    {
        double midi;
        double amplitude;
        double salience;
    };
    std::vector<std::vector<Detection>> perFrame(static_cast<std::size_t>(count));
    std::vector<double> frameEnergy(static_cast<std::size_t>(count), 0.0);

    // Frame energies first (the silence threshold is relative to the loudest frame).
    for (int i = 0; i < count; ++i)
    {
        const auto centre = static_cast<long long>(std::llround(i * hop));
        double e = 0.0;
        for (int j = 0; j < windowLength; j += 4)
        {
            const long long index = centre - windowLength / 2 + j;
            if (index >= 0 && index < numSamples)
                e += static_cast<double>(samples[index]) * static_cast<double>(samples[index]);
        }
        at(frameEnergy, i) = e;
    }
    const double loudestEnergy = *std::max_element(frameEnergy.begin(), frameEnergy.end());

    for (int i = 0; i < count; ++i)
    {
        if ((i & 15) == 0)
        {
            if (isCancelled(cancel))
                return {};
            if (progress)
                progress(0.9f * static_cast<float>(i) / static_cast<float>(count));
        }
        if (at(frameEnergy, i) < loudestEnergy * 3.2e-5 || at(frameEnergy, i) <= 0.0) // -45 dB
            continue;
        const auto centre = static_cast<long long>(std::llround(i * hop));
        std::fill(in.begin(), in.end(), 0.0f);
        for (int j = 0; j < windowLength; ++j)
        {
            const long long index = centre - windowLength / 2 + j;
            if (index >= 0 && index < numSamples)
                at(in, j) = samples[index] * at(window, j);
        }
        fft.forward(in.data(), spectrum.data());
        prefix[0] = 0.0;
        for (int k = 0; k < bins; ++k)
        {
            at(mag, k) = std::abs(at(spectrum, k));
            at(prefix, k + 1) = at(prefix, k) + at(mag, k);
        }
        // Whitening: divide out the spectral envelope (+-2 semitones around each bin) to the power 0.67.
        for (int k = 0; k < bins; ++k)
        {
            const int lo = std::max(1, static_cast<int>(k / 1.12));
            const int hi = std::min(bins - 1, static_cast<int>(k * 1.12) + 1);
            const double envelope = (at(prefix, hi + 1) - at(prefix, lo)) / (hi - lo + 1) + 1.0e-12;
            at(residual, k) = at(mag, k) * std::pow(envelope, -0.67);
        }

        double firstSalience = 0.0;
        auto& detections = at(perFrame, i);
        for (int voice = 0; voice < maxVoices; ++voice)
        {
            for (int q = 0; q < gridSize; ++q)
            {
                double best = 0.0;
                for (int k = at(gridLo, q); k <= at(gridHi, q); ++k)
                    best = std::max(best, at(residual, k));
                at(logGrid, q) = best;
            }
            int bestCandidate = -1;
            double bestSalience = 0.0;
            for (int c = 0; c < candidates; ++c)
            {
                const double f0 = midiToHz(kLowMidi + static_cast<double>(c) / kGrid);
                double s = 0.0;
                for (int h = 1; h <= kHarmonics; ++h)
                {
                    const int q = c + harmonicOffset[static_cast<std::size_t>(h)];
                    if (q >= gridSize)
                        break;
                    s += (f0 + 52.0) / (h * f0 + 320.0) * at(logGrid, q);
                }
                at(salience, c) = s;
                if (s > bestSalience)
                {
                    // Not a sub-octave ghost: the fundamental or the second harmonic is there.
                    const double strongest = std::max({at(logGrid, c), at(logGrid, std::min(gridSize - 1, c + harmonicOffset[2])),
                                                       at(logGrid, std::min(gridSize - 1, c + harmonicOffset[3]))});
                    if (std::max(at(logGrid, c), at(logGrid, std::min(gridSize - 1, c + harmonicOffset[2]))) < 0.25 * strongest)
                        continue;
                    // Not one already found, nor what is left of one's partials an octave, a twelfth or
                    // two octaves up (unless strong in its own right).
                    bool known = false;
                    const double m = kLowMidi + static_cast<double>(c) / kGrid;
                    for (const auto& d : detections)
                    {
                        known = known || std::abs(d.midi - m) < 0.6;
                        for (const double interval : {12.0, 19.02, 24.0})
                            known = known || (std::abs(d.midi + interval - m) < 0.4 && s < 0.6 * d.salience);
                    }
                    if (known)
                        continue;
                    bestSalience = s;
                    bestCandidate = c;
                }
            }
            if (bestCandidate < 0)
                break;
            if (voice == 0)
                firstSalience = bestSalience;
            else if (bestSalience < 0.25 * firstSalience)
                break;

            // The partials of the best candidate, then the pitch from their measured frequencies.
            double f0 = midiToHz(kLowMidi + static_cast<double>(bestCandidate) / kGrid);
            std::array<int, kHarmonics + 2> peakBin{};
            std::array<double, kHarmonics + 2> amplitude{};
            int harmonics = 0;
            double weighted = 0.0, weights = 0.0;
            for (int h = 1; h <= kHarmonics && h * f0 < sampleRate * 0.47; ++h)
            {
                const int lo = std::clamp(static_cast<int>(std::floor(h * f0 * std::exp2(-0.3 / 12.0) / binHz)), 1, bins - 2);
                const int hi = std::clamp(static_cast<int>(std::ceil(h * f0 * std::exp2(0.3 / 12.0) / binHz)), lo, bins - 2);
                int peak = lo;
                for (int k = lo; k <= hi; ++k)
                    if (at(residual, k) > at(residual, peak))
                        peak = k;
                peakBin[static_cast<std::size_t>(h)] = peak;
                amplitude[static_cast<std::size_t>(h)] = at(residual, peak);
                harmonics = h;
                // (a true peak of the spectrum: its frequency, interpolated on the log magnitude)
                const double m0 = at(mag, peak - 1), m1 = at(mag, peak), m2 = at(mag, peak + 1);
                if (h <= 8 && m1 > m0 && m1 >= m2 && m0 > 0.0 && m2 > 0.0)
                {
                    const double l0 = std::log(m0), l1 = std::log(m1), l2 = std::log(m2);
                    const double denominator = l0 - 2.0 * l1 + l2;
                    const double delta = denominator < -1.0e-12 ? std::clamp(0.5 * (l0 - l2) / denominator, -0.5, 0.5) : 0.0;
                    const double weight = at(residual, peak) / h;
                    weighted += weight * (peak + delta) * binHz / h;
                    weights += weight;
                }
            }
            double refined = kLowMidi + static_cast<double>(bestCandidate) / kGrid;
            if (weights > 0.0 && std::abs(hzToMidi(weighted / weights) - refined) < 0.5)
                refined = hzToMidi(weighted / weights);
            f0 = midiToHz(refined);
            double loudness = 0.0;
            for (int h = 1; h <= harmonics; ++h)
            {
                const auto hu = static_cast<std::size_t>(h);
                const double neighbours = (amplitude[hu - 1 > 0 ? hu - 1 : hu] + amplitude[hu] + amplitude[h < harmonics ? hu + 1 : hu]) / 3.0;
                const double cancel_ = std::min(amplitude[hu], neighbours);
                const double scale = amplitude[hu] > 0.0 ? 1.0 - cancel_ / amplitude[hu] : 1.0;
                const int peak = peakBin[hu];
                for (int k = std::max(1, peak - 4); k <= std::min(bins - 1, peak + 4); ++k)
                    at(residual, k) *= scale + (1.0 - scale) * (std::abs(k - peak) / 4.0) * (std::abs(k - peak) / 4.0);
                loudness += at(mag, peak);
            }
            detections.push_back({refined, loudness, bestSalience});
        }
    }

    // Linking frames into notes.
    struct Track
    {
        int first = 0;
        std::vector<double> midi;
        std::vector<double> amplitude;
        int missed = 0;
    };
    std::vector<Track> active, finished;
    for (int i = 0; i < count; ++i)
    {
        const auto& detections = at(perFrame, i);
        std::vector<bool> used(detections.size(), false);
        std::vector<bool> matched(active.size(), false);
        // Greedy: the closest pairs first.
        struct Pair
        {
            double distance;
            std::size_t track, detection;
        };
        std::vector<Pair> pairs;
        for (std::size_t t = 0; t < active.size(); ++t)
            for (std::size_t d = 0; d < detections.size(); ++d)
                if (const double distance = std::abs(active[t].midi.back() - detections[d].midi); distance < 0.6)
                    pairs.push_back({distance, t, d});
        std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.distance < b.distance; });
        for (const auto& pair : pairs)
        {
            if (matched[pair.track] || used[pair.detection])
                continue;
            matched[pair.track] = used[pair.detection] = true;
            auto& track = active[pair.track];
            for (int m = 0; m < track.missed; ++m)
            {
                track.midi.push_back(track.midi.back());
                track.amplitude.push_back(track.amplitude.back());
            }
            track.missed = 0;
            track.midi.push_back(detections[pair.detection].midi);
            track.amplitude.push_back(detections[pair.detection].amplitude);
        }
        for (std::size_t t = active.size(); t-- > 0;)
            if (!matched[t] && ++active[t].missed > 3)
            {
                finished.push_back(std::move(active[t]));
                active.erase(active.begin() + static_cast<std::ptrdiff_t>(t));
            }
        for (std::size_t d = 0; d < detections.size(); ++d)
            if (!used[d])
                active.push_back({i, {detections[d].midi}, {detections[d].amplitude}, 0});
    }
    for (auto& t : active)
        finished.push_back(std::move(t));

    double loudest = 1.0e-12;
    for (const auto& t : finished)
        for (const double a : t.amplitude)
            loudest = std::max(loudest, a);
    const int minimum = static_cast<int>(std::lround(0.1 / kHop));
    for (const auto& t : finished)
    {
        // A new attack on the same pitch (the level doubling after a dip) starts a new note.
        std::vector<int> cuts{0};
        for (int k = 5; k < static_cast<int>(t.midi.size()); ++k)
        {
            int quietest = k - 5;
            for (int j = k - 5; j < k; ++j)
                if (at(t.amplitude, j) < at(t.amplitude, quietest))
                    quietest = j;
            if (quietest - cuts.back() >= minimum && at(t.amplitude, k) >= 2.0 * at(t.amplitude, quietest))
                cuts.push_back(quietest + 1);
        }
        cuts.push_back(static_cast<int>(t.midi.size()));
        for (std::size_t c = 0; c + 1 < cuts.size(); ++c)
        {
            const int from = cuts[c];
            const int to = cuts[c + 1] - 1;
            if (to - from + 1 < minimum)
                continue;
            std::vector<double> midi(t.midi.begin() + from, t.midi.begin() + to + 1);
            std::vector<double> level;
            for (int k = from; k <= to; ++k)
                level.push_back(at(t.amplitude, k) / loudest);
            auto note = makeNote(midi, level, 0, to - from, kHop);
            note.start += static_cast<double>(t.first + from) * kHop;
            note.end += static_cast<double>(t.first + from) * kHop;
            notes.push_back(std::move(note));
        }
    }
    std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) { return std::tie(a.start, a.pitch) < std::tie(b.start, b.pitch); });
    if (progress)
        progress(1.0f);
    return notes;
}

std::vector<Note> analyse(const float* samples, int numSamples, double sampleRate, Mode mode, const std::atomic<bool>* cancel,
                          const std::function<void(float)>& progress)
{
    if (mode == Mode::polyphonic)
        return detectPolyphonic(samples, numSamples, sampleRate, 6, cancel, progress);
    constexpr double kHop = 0.005;
    const auto frames = trackPitch(samples, numSamples, sampleRate, kHop, 50.0, 1200.0, cancel, progress);
    if (isCancelled(cancel))
        return {};
    return segmentMelodic(frames, kHop);
}

} // namespace dojo::audioedit::pitch
