#include "ui/score/Notation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

namespace dojo::score
{
namespace
{
constexpr double kEpsilon = 1.0e-6;
constexpr std::array<int, 7> kStepSemitones{0, 2, 4, 5, 7, 9, 11};
constexpr std::array<int, 7> kSharpOrder{3, 0, 4, 1, 5, 2, 6}; // F C G D A E B
constexpr std::array<int, 7> kFlatOrder{6, 2, 5, 1, 4, 0, 3};  // B E A D G C F

struct Name
{
    int step;
    int alter;
};
// Pitch classes 0..11 spelled in C (sharps but E flat and B flat), in sharp keys, in flat keys.
constexpr std::array<Name, 12> kNatural{{{0, 0}, {0, 1}, {1, 0}, {2, -1}, {2, 0}, {3, 0}, {3, 1}, {4, 0}, {4, 1}, {5, 0}, {6, -1}, {6, 0}}};
constexpr std::array<Name, 12> kSharps{{{0, 0}, {0, 1}, {1, 0}, {1, 1}, {2, 0}, {3, 0}, {3, 1}, {4, 0}, {4, 1}, {5, 0}, {5, 1}, {6, 0}}};
constexpr std::array<Name, 12> kFlats{{{0, 0}, {1, -1}, {1, 0}, {2, -1}, {2, 0}, {3, 0}, {4, -1}, {4, 0}, {5, -1}, {5, 0}, {6, -1}, {6, 0}}};

struct Value
{
    double length;
    int type;
    int dots;
};
constexpr std::array<Value, 12> kValues{{{4.0, 0, 0},
                                         {3.0, 1, 1},
                                         {2.0, 1, 0},
                                         {1.5, 2, 1},
                                         {1.0, 2, 0},
                                         {0.75, 3, 1},
                                         {0.5, 3, 0},
                                         {0.375, 4, 1},
                                         {0.25, 4, 0},
                                         {0.1875, 5, 1},
                                         {0.125, 5, 0},
                                         {0.0625, 6, 0}}};

bool isMultiple(double x, double unit) noexcept
{
    const double ratio = x / unit;
    return std::abs(ratio - std::round(ratio)) < 1.0e-4;
}

bool isCompound(time::TimeSignature s) noexcept { return s.denominator >= 8 && s.numerator % 3 == 0; }

/** The span notes are aligned to and beamed in: a beat (a dotted quarter in compound meters; at
    most a quarter otherwise). */
double beatGroup(time::TimeSignature s) noexcept
{
    const double unit = 4.0 / std::max(1, s.denominator);
    if (isCompound(s))
        return 3.0 * unit;
    return s.denominator <= 4 ? std::min(unit, 1.0) : 1.0;
}

struct Piece
{
    double length;
    int type;
    int dots;
};

/** `length` beats from `offset` into a bar as notatable values. Each is the longest that fits and
    sits well: a note's plain value on a multiple of itself (of the beat group, when longer), a dotted
    one on a beat group or a multiple of twice its plain value; off the beat, nothing crosses the next
    beat group (an eighth then a quarter, not a dotted figure across the beat). Rests show the beats:
    a plain rest only on a multiple of itself, a dotted one only on a beat group (in compound meters)
    or a multiple of twice its plain value. */
std::vector<Piece> decompose(double offset, double length, double barLength, time::TimeSignature signature, bool rest)
{
    const double group = beatGroup(signature);
    const bool compound = isCompound(signature);
    std::vector<Piece> pieces;
    double p = offset;
    double remaining = length;
    while (remaining > kEpsilon)
    {
        const bool onGroup = isMultiple(p, group);
        const double limit = onGroup ? barLength : std::min(barLength, (std::floor(p / group + kEpsilon) + 1.0) * group);
        const Value* chosen = &kValues.back();
        for (const auto& v : kValues)
        {
            if (v.length > remaining + kEpsilon || p + v.length > limit + kEpsilon)
                continue;
            const double plain = v.dots > 0 ? v.length / 1.5 : v.length;
            bool fits = false;
            if (rest)
                fits = v.dots > 0 ? onGroup && (compound || isMultiple(p, 2.0 * plain)) : isMultiple(p, plain);
            else
                fits = v.dots > 0 ? onGroup || isMultiple(p, std::min(2.0 * plain, group)) : isMultiple(p, std::min(plain, group));
            if (fits)
            {
                chosen = &v;
                break;
            }
        }
        const double step = std::min(chosen->length, remaining);
        pieces.push_back({step, chosen->type, chosen->dots});
        p += step;
        remaining -= step;
    }
    return pieces;
}

constexpr std::array<double, 12> kMajorProfile{6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
constexpr std::array<double, 12> kMinorProfile{6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};
constexpr std::array<int, 12> kFifthsOfMajor{0, -5, 2, -3, 4, -1, 6, 1, -4, 3, -2, 5}; // by tonic pitch class

double correlation(const std::array<double, 12>& a, const std::array<double, 12>& profile, int tonic) noexcept
{
    double ma = 0.0, mp = 0.0;
    for (int i = 0; i < 12; ++i)
    {
        ma += a[static_cast<std::size_t>(i)];
        mp += profile[static_cast<std::size_t>(i)];
    }
    ma /= 12.0;
    mp /= 12.0;
    double num = 0.0, da = 0.0, dp = 0.0;
    for (int i = 0; i < 12; ++i)
    {
        const double x = a[static_cast<std::size_t>((i + tonic) % 12)] - ma;
        const double y = profile[static_cast<std::size_t>(i)] - mp;
        num += x * y;
        da += x * x;
        dp += y * y;
    }
    return da > 0.0 && dp > 0.0 ? num / std::sqrt(da * dp) : 0.0;
}
} // namespace

Spelling spell(int midi, int keyFifths) noexcept
{
    const int pc = ((midi % 12) + 12) % 12;
    int octave = (midi - pc) / 12 - 1;
    Name name = keyFifths > 0 ? kSharps[static_cast<std::size_t>(pc)] : keyFifths < 0 ? kFlats[static_cast<std::size_t>(pc)]
                                                                                     : kNatural[static_cast<std::size_t>(pc)];
    // The keys with six or seven accidentals name E#, B#, Cb and Fb.
    if (keyFifths >= 6 && pc == 5)
        name = {2, 1};
    if (keyFifths >= 7 && pc == 0)
    {
        name = {6, 1};
        --octave;
    }
    if (keyFifths <= -6 && pc == 11)
    {
        name = {0, -1};
        ++octave;
    }
    if (keyFifths <= -7 && pc == 4)
        name = {3, -1};
    return {name.step, octave, name.alter};
}

int keyAlteration(int step, int keyFifths) noexcept
{
    const int count = std::min(7, std::abs(keyFifths));
    const auto& order = keyFifths > 0 ? kSharpOrder : kFlatOrder;
    for (int i = 0; i < count; ++i)
        if (order[static_cast<std::size_t>(i)] == step)
            return keyFifths > 0 ? 1 : -1;
    return 0;
}

int midiOf(const Spelling& s) noexcept { return (s.octave + 1) * 12 + kStepSemitones[static_cast<std::size_t>(std::clamp(s.step, 0, 6))] + s.alter; }

int detectKey(const std::vector<SourceNote>& notes) noexcept
{
    std::array<double, 12> weight{};
    double total = 0.0;
    for (const auto& n : notes)
    {
        const double d = std::max(0.0625, n.end - n.start);
        weight[static_cast<std::size_t>(((n.midi % 12) + 12) % 12)] += d;
        total += d;
    }
    // Too little to tell (fewer than six notes or four pitch classes): C, rather than a key that
    // changes with each note written.
    const auto classes = std::count_if(weight.begin(), weight.end(), [](double w) { return w > 0.0; });
    if (total <= 0.0 || notes.size() < 6 || classes < 4)
        return 0;
    double best = -2.0;
    int fifths = 0;
    for (int tonic = 0; tonic < 12; ++tonic)
    {
        if (const double r = correlation(weight, kMajorProfile, tonic); r > best + 1.0e-12)
        {
            best = r;
            fifths = kFifthsOfMajor[static_cast<std::size_t>(tonic)];
        }
        if (const double r = correlation(weight, kMinorProfile, tonic); r > best + 1.0e-12)
        {
            best = r;
            fifths = kFifthsOfMajor[static_cast<std::size_t>((tonic + 3) % 12)];
        }
    }
    return fifths;
}

const char* keyName(int keyFifths) noexcept
{
    static constexpr std::array<const char*, 15> names{
        "Cb major / Ab minor", "Gb major / Eb minor", "Db major / Bb minor", "Ab major / F minor", "Eb major / C minor",
        "Bb major / G minor",  "F major / D minor",   "C major / A minor",   "G major / E minor",  "D major / B minor",
        "A major / F# minor",  "E major / C# minor",  "B major / G# minor",  "F# major / D# minor", "C# major / A# minor"};
    return names[static_cast<std::size_t>(std::clamp(keyFifths, -7, 7) + 7)];
}

double valueLength(int type, int dots) noexcept
{
    const double base = 4.0 / std::exp2(static_cast<double>(std::clamp(type, 0, 6)));
    return base * (2.0 - 1.0 / std::exp2(static_cast<double>(std::max(0, dots))));
}

Score layout(const std::vector<SourceNote>& notes, double from, double to, const time::TempoMap& map, const Options& options)
{
    Score score;

    // The bars shown: whole bars around [from, to).
    const auto firstBar = map.barAt(from + kEpsilon);
    const auto lastBar = std::max(firstBar, map.barAt(std::max(from, to - kEpsilon)));
    for (auto bar = firstBar; bar <= lastBar; ++bar)
    {
        Bar b{bar, map.beatOfBar(bar), map.beatOfBar(bar + 1), map.signatureOfBar(bar), false};
        b.showSignature = score.bars.empty() || b.signature.numerator != score.bars.back().signature.numerator
                          || b.signature.denominator != score.bars.back().signature.denominator;
        score.bars.push_back(b);
    }
    const double begin = score.bars.front().start;
    const double finish = score.bars.back().end;

    std::vector<SourceNote> inRange;
    for (const auto& n : notes)
        if (n.end > begin && n.start < finish && n.end > n.start)
            inRange.push_back(n);
    score.keyFifths = options.key.has_value() ? std::clamp(*options.key, -7, 7) : detectKey(inRange);

    // The staves.
    auto mode = options.clef;
    if (mode == ClefMode::automatic)
    {
        int lo = 127, hi = 0;
        for (const auto& n : inRange)
        {
            lo = std::min(lo, n.midi);
            hi = std::max(hi, n.midi);
        }
        mode = inRange.empty() || lo >= 55 ? ClefMode::treble : hi <= 64 ? ClefMode::bass : ClefMode::grand;
    }
    std::vector<std::pair<Clef, std::vector<SourceNote>>> staves;
    if (mode == ClefMode::grand)
    {
        staves.push_back({Clef::treble, {}});
        staves.push_back({Clef::bass, {}});
        for (const auto& n : inRange)
            staves[n.midi >= 60 ? std::size_t{0} : std::size_t{1}].second.push_back(n);
    }
    else
    {
        staves.push_back({mode == ClefMode::bass ? Clef::bass : Clef::treble, inRange});
    }

    const double grid = std::max(0.0625, options.grid);
    const auto quantize = [grid](double x) { return std::round(x / grid) * grid; };

    for (auto& [clef, staffNotes] : staves)
    {
        Staff staff;
        staff.clef = clef;

        // Onsets (chords) with their ends: the longest of their notes, until the next onset.
        struct Onset
        {
            double at = 0.0;
            double end = 0.0;
            std::vector<const SourceNote*> notes;
        };
        std::map<long long, Onset> byTick;
        for (const auto& n : staffNotes)
        {
            const double s = std::clamp(quantize(n.start), begin, finish);
            if (s >= finish - kEpsilon)
                continue;
            const double e = std::min(finish, std::max(s + grid, quantize(n.end)));
            auto& onset = byTick[std::llround(s / grid)];
            onset.at = s;
            onset.end = std::max(onset.end, e);
            onset.notes.push_back(&n);
        }
        std::vector<Onset> onsets;
        for (auto& [tick, onset] : byTick)
            onsets.push_back(std::move(onset));
        for (std::size_t i = 0; i + 1 < onsets.size(); ++i)
            onsets[i].end = std::min(onsets[i].end, onsets[i + 1].at);

        // Spans: chords, and rests between them.
        struct Span
        {
            double start, end;
            const Onset* chord;
        };
        std::vector<Span> spans;
        double cursor = begin;
        for (const auto& onset : onsets)
        {
            if (onset.at > cursor + kEpsilon)
                spans.push_back({cursor, onset.at, nullptr});
            spans.push_back({onset.at, onset.end, &onset});
            cursor = onset.end;
        }
        if (cursor < finish - kEpsilon)
            spans.push_back({cursor, finish, nullptr});

        // Cut at bar lines, written as notatable values, tied across.
        for (const auto& span : spans)
        {
            std::vector<Event> pieces;
            for (std::size_t b = 0; b < score.bars.size(); ++b)
            {
                const auto& bar = score.bars[b];
                const double s = std::max(span.start, bar.start);
                const double e = std::min(span.end, bar.end);
                if (e <= s + kEpsilon)
                    continue;
                const double barLength = bar.end - bar.start;
                if (span.chord == nullptr && s <= bar.start + kEpsilon && e >= bar.end - kEpsilon)
                {
                    Event rest;
                    rest.beat = bar.start;
                    rest.length = barLength;
                    rest.type = 0;
                    rest.rest = true;
                    rest.wholeBarRest = true;
                    rest.bar = static_cast<int>(b);
                    pieces.push_back(rest);
                    continue;
                }
                double at = s;
                for (const auto& piece : decompose(s - bar.start, e - s, barLength, bar.signature, span.chord == nullptr))
                {
                    Event ev;
                    ev.beat = at;
                    ev.length = piece.length;
                    ev.type = piece.type;
                    ev.dots = piece.dots;
                    ev.rest = span.chord == nullptr;
                    ev.bar = static_cast<int>(b);
                    pieces.push_back(ev);
                    at += piece.length;
                }
            }
            if (span.chord != nullptr)
            {
                std::vector<const SourceNote*> sorted = span.chord->notes;
                std::sort(sorted.begin(), sorted.end(), [](const SourceNote* a, const SourceNote* b) { return a->midi < b->midi; });
                for (std::size_t k = 0; k < pieces.size(); ++k)
                    for (const auto* n : sorted)
                    {
                        if (!pieces[k].heads.empty() && pieces[k].heads.back().midi == n->midi)
                            continue; // (a doubled note)
                        Head head;
                        head.midi = n->midi;
                        head.spelling = spell(n->midi, score.keyFifths);
                        head.tieStart = k + 1 < pieces.size();
                        head.tieEnd = k > 0;
                        head.id = n->id;
                        pieces[k].heads.push_back(head);
                    }
            }
            for (auto& p : pieces)
                staff.events.push_back(std::move(p));
        }

        // Accidentals: against the key signature and earlier notes on the line in the bar.
        std::map<int, int> alterAt;
        int currentBar = -1;
        for (auto& ev : staff.events)
        {
            if (ev.bar != currentBar)
            {
                alterAt.clear();
                currentBar = ev.bar;
            }
            for (auto& head : ev.heads)
            {
                const int line = diatonic(head.spelling);
                if (head.tieEnd)
                {
                    alterAt[line] = head.spelling.alter;
                    continue;
                }
                const auto known = alterAt.find(line);
                const int expected = known != alterAt.end() ? known->second : keyAlteration(head.spelling.step, score.keyFifths);
                if (head.spelling.alter != expected)
                    head.accidental = head.spelling.alter == 0    ? Accidental::natural
                                      : head.spelling.alter == 1  ? Accidental::sharp
                                      : head.spelling.alter == -1 ? Accidental::flat
                                      : head.spelling.alter >= 2  ? Accidental::doubleSharp
                                                                  : Accidental::doubleFlat;
                alterAt[line] = head.spelling.alter;
            }
        }

        // Stems: away from the middle line; beams: eighths and shorter within a beat group.
        const int middle = middleLine(staff.clef);
        for (auto& ev : staff.events)
            if (!ev.rest && !ev.heads.empty())
                ev.stemUp = (diatonic(ev.heads.front().spelling) + diatonic(ev.heads.back().spelling)) < 2 * middle;
        int beamCount = 0;
        for (std::size_t i = 0; i < staff.events.size();)
        {
            const auto& first = staff.events[i];
            const auto& bar = score.bars[static_cast<std::size_t>(first.bar)];
            const double group = beatGroup(bar.signature);
            const auto groupOf = [&](const Event& e) { return std::llround(std::floor((e.beat - bar.start) / group + kEpsilon)); };
            std::size_t j = i;
            if (!first.rest && first.type >= 3)
                while (j + 1 < staff.events.size())
                {
                    const auto& next = staff.events[j + 1];
                    if (next.rest || next.type < 3 || next.bar != first.bar || groupOf(next) != groupOf(first))
                        break;
                    ++j;
                }
            if (j > i)
            {
                int sum = 0, count = 0;
                for (std::size_t k = i; k <= j; ++k)
                {
                    sum += diatonic(staff.events[k].heads.front().spelling) + diatonic(staff.events[k].heads.back().spelling);
                    count += 2;
                }
                const bool up = sum < middle * count;
                for (std::size_t k = i; k <= j; ++k)
                {
                    staff.events[k].beam = beamCount;
                    staff.events[k].stemUp = up;
                }
                ++beamCount;
            }
            i = j + 1;
        }
        score.staves.push_back(std::move(staff));
    }
    return score;
}

} // namespace dojo::score
