#include "devices/MidiEffects.h"

#include <algorithm>
#include <climits>
#include <cmath>

namespace dojo::devices
{
namespace
{
// Raw MIDI, read and written byte by byte: no juce::MidiMessage (a long one would allocate).
struct Raw
{
    const juce::uint8* data = nullptr;
    int size = 0;

    [[nodiscard]] int status() const noexcept { return size > 0 ? data[0] : 0; }
    [[nodiscard]] int type() const noexcept { return status() & 0xf0; }
    [[nodiscard]] int channel() const noexcept { return (status() & 0x0f) + 1; }
    [[nodiscard]] bool isChannelMessage() const noexcept { return status() >= 0x80 && status() < 0xf0; }
    [[nodiscard]] int byte(int index) const noexcept { return index < size ? data[index] : 0; }
    [[nodiscard]] bool isNoteOn() const noexcept { return type() == 0x90 && size >= 3 && data[2] > 0; }
    [[nodiscard]] bool isNoteOff() const noexcept { return size >= 3 && (type() == 0x80 || (type() == 0x90 && data[2] == 0)); }
    [[nodiscard]] bool isPolyAftertouch() const noexcept { return type() == 0xa0 && size >= 3; }
    [[nodiscard]] bool isController() const noexcept { return type() == 0xb0 && size >= 3; }
    /** All sound off, reset controllers, local control, all notes off, omni / mono / poly. */
    [[nodiscard]] bool isChannelMode() const noexcept { return isController() && data[1] >= 120; }
    [[nodiscard]] bool isPanic() const noexcept { return isController() && (data[1] == 120 || data[1] == 123); }
};

Raw rawOf(const juce::MidiMessageMetadata& meta) noexcept
{
    return {meta.data, meta.numBytes};
}

juce::uint8 clamp7(int value) noexcept
{
    return static_cast<juce::uint8>(std::clamp(value, 0, 127));
}

void addNoteOn(juce::MidiBuffer& out, int sample, int channel, int note, int velocity) noexcept
{
    const juce::uint8 bytes[3] = {static_cast<juce::uint8>(0x90 | ((channel - 1) & 0x0f)), clamp7(note), static_cast<juce::uint8>(std::clamp(velocity, 1, 127))};
    out.addEvent(bytes, 3, sample);
}

void addNoteOff(juce::MidiBuffer& out, int sample, int channel, int note, int velocity = 0) noexcept
{
    const juce::uint8 bytes[3] = {static_cast<juce::uint8>(0x80 | ((channel - 1) & 0x0f)), clamp7(note), clamp7(velocity)};
    out.addEvent(bytes, 3, sample);
}

void addRechannelled(juce::MidiBuffer& out, int sample, const Raw& raw, int channel) noexcept
{
    if (!raw.isChannelMessage() || raw.size > 3)
    {
        out.addEvent(raw.data, raw.size, sample);
        return;
    }
    juce::uint8 bytes[3] = {static_cast<juce::uint8>((raw.status() & 0xf0) | ((channel - 1) & 0x0f)), 0, 0};
    for (int i = 1; i < raw.size; ++i)
        bytes[i] = raw.data[i];
    out.addEvent(bytes, raw.size, sample);
}

int floorDiv(int a, int b) noexcept
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

int positiveModulo(int a, int b) noexcept
{
    return ((a % b) + b) % b;
}

juce::StringArray numbered(const juce::String& first, int count)
{
    juce::StringArray names{first};
    for (int i = 1; i <= count; ++i)
        names.add(juce::String(i));
    return names;
}
} // namespace

//==================================================================================================
// Scales

namespace scales
{
namespace
{
std::uint16_t maskOf(std::initializer_list<int> steps)
{
    std::uint16_t mask = 0;
    for (const int s : steps)
        mask = static_cast<std::uint16_t>(mask | (1u << s));
    return mask;
}

bool inScale(int note, int root, std::uint16_t mask) noexcept
{
    return ((static_cast<unsigned>(mask) >> static_cast<unsigned>(positiveModulo(note - root, 12))) & 1u) != 0;
}
} // namespace

const std::vector<Scale>& all()
{
    static const std::vector<Scale> scales{
        {"Major", maskOf({0, 2, 4, 5, 7, 9, 11})},
        {"Minor", maskOf({0, 2, 3, 5, 7, 8, 10})},
        {"Harmonic Minor", maskOf({0, 2, 3, 5, 7, 8, 11})},
        {"Melodic Minor", maskOf({0, 2, 3, 5, 7, 9, 11})},
        {"Dorian", maskOf({0, 2, 3, 5, 7, 9, 10})},
        {"Phrygian", maskOf({0, 1, 3, 5, 7, 8, 10})},
        {"Lydian", maskOf({0, 2, 4, 6, 7, 9, 11})},
        {"Mixolydian", maskOf({0, 2, 4, 5, 7, 9, 10})},
        {"Locrian", maskOf({0, 1, 3, 5, 6, 8, 10})},
        {"Major Pentatonic", maskOf({0, 2, 4, 7, 9})},
        {"Minor Pentatonic", maskOf({0, 3, 5, 7, 10})},
        {"Blues", maskOf({0, 3, 5, 6, 7, 10})},
        {"Whole Tone", maskOf({0, 2, 4, 6, 8, 10})},
        {"Diminished", maskOf({0, 2, 3, 5, 6, 8, 9, 11})},
        {"Phrygian Dominant", maskOf({0, 1, 4, 5, 7, 8, 10})},
        {"Hungarian Minor", maskOf({0, 2, 3, 6, 7, 8, 11})},
        {"Hirajoshi", maskOf({0, 2, 3, 7, 8})},
        {"In Sen", maskOf({0, 1, 5, 7, 10})},
        {"Chromatic", 0x0fff},
    };
    return scales;
}

juce::StringArray names()
{
    juce::StringArray result;
    for (const auto& scale : all())
        result.add(scale.name);
    return result;
}

juce::StringArray keyNames()
{
    return {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
}

int stepsOf(std::uint16_t mask, std::array<int, 12>& steps) noexcept
{
    int count = 0;
    for (int s = 0; s < 12; ++s)
        if (((static_cast<unsigned>(mask) >> static_cast<unsigned>(s)) & 1u) != 0)
            steps[static_cast<std::size_t>(count++)] = s;
    return count;
}

int snap(int note, int root, std::uint16_t mask, Snap direction) noexcept
{
    if ((mask & 0x0fff) == 0)
        return -1;
    if (note >= 0 && note <= 127 && inScale(note, root, mask))
        return note;
    for (int d = 1; d <= 12; ++d)
    {
        if (direction != up && note - d >= 0 && note - d <= 127 && inScale(note - d, root, mask))
            return note - d;
        if (direction != down && note + d >= 0 && note + d <= 127 && inScale(note + d, root, mask))
            return note + d;
    }
    return -1;
}

int shiftDegrees(int note, int root, std::uint16_t mask, int degrees) noexcept
{
    const int base = snap(note, root, mask, nearest);
    if (base < 0)
        return -1;
    if (degrees == 0)
        return base;
    std::array<int, 12> steps{};
    const int count = stepsOf(mask, steps);
    const int relative = base - root;
    const int octave = floorDiv(relative, 12);
    const int pc = relative - octave * 12;
    int index = 0;
    while (index < count && steps[static_cast<std::size_t>(index)] != pc)
        ++index;
    const int target = index + degrees;
    const int result = root + (octave + floorDiv(target, count)) * 12 + steps[static_cast<std::size_t>(positiveModulo(target, count))];
    return result >= 0 && result <= 127 ? result : -1;
}

int whiteKey(int note, int root, std::uint16_t mask) noexcept
{
    static constexpr std::array<int, 12> whiteIndex{0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6};
    if (note < 0 || note > 127)
        return -1;
    const int white = whiteIndex[static_cast<std::size_t>(note % 12)];
    std::array<int, 12> steps{};
    const int count = stepsOf(mask, steps);
    if (white < 0 || count == 0)
        return -1;
    const int degree = (note / 12) * 7 + white - 35; // (middle C, 60, is degree 0)
    const int result = 60 + root + 12 * floorDiv(degree, count) + steps[static_cast<std::size_t>(positiveModulo(degree, count))];
    return result >= 0 && result <= 127 ? result : -1;
}
} // namespace scales

//==================================================================================================
// The base

MidiEffect::MidiEffect(juce::String typeId, juce::String name)
    : BuiltinProcessor(std::move(typeId), std::move(name),
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
}

void MidiEffect::prepareDsp(double, int)
{
    out_.ensureSize(16384);
    out_.clear();
    prepareMidi();
}

void MidiEffect::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept
{
    run(midi, buffer.getNumSamples());
}

void MidiEffect::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    run(midi, buffer.getNumSamples()); // (the audio passes through untouched)
}

void MidiEffect::processBlock(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi)
{
    run(midi, buffer.getNumSamples());
}

void MidiEffect::run(juce::MidiBuffer& midi, int numSamples) noexcept
{
    out_.clear();
    processMidi(midi, out_, std::max(0, numSamples));
    midi.clear();
    midi.addEvents(out_, 0, -1, 0); // (copied into the engine's own storage)
}

MidiEffect::Transport MidiEffect::transport() const noexcept
{
    Transport t;
    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
        {
            if (const auto bpm = position->getBpm())
                t.bpm = std::max(1.0, *bpm);
            if (const auto ppq = position->getPpqPosition())
                t.ppq = *ppq;
            t.playing = position->getIsPlaying();
        }
    return t;
}

//==================================================================================================
// Arpeggiator

Arpeggiator::Arpeggiator()
    : MidiEffect(kTypeId, "Arpeggiator")
{
    style_ = addChoice("style", "Style", styleNames(), up);
    rate_ = addChoice("rate", "Rate", rateNames(), 9);
    sync_ = addBool("sync", "Sync", true);
    freeRate_ = addFloat("freerate", "Free Rate", skewedRange(0.5f, 40.0f, 6.0f, 0.01f), 8.0f, "Hz");
    gate_ = addFloat("gate", "Gate", juce::NormalisableRange<float>(5.0f, 200.0f, 1.0f), 50.0f, "%", true, 0);
    octaves_ = addChoice("octaves", "Octaves", {"1", "2", "3", "4"}, 0);
    swing_ = addFloat("swing", "Swing", juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 0.0f, "%", true, 0);
    repeats_ = addChoice("repeats", "Repeats", {"1", "2", "3", "4", "5", "6", "7", "8"}, 0);
    velocityMode_ = addChoice("velmode", "Velocity", {"As Played", "Fixed"}, 0);
    velocity_ = addFloat("velocity", "Fixed Velocity", juce::NormalisableRange<float>(1.0f, 127.0f, 1.0f), 100.0f, {}, true, 0);
    hold_ = addBool("hold", "Hold", false);
    retrigger_ = addBool("retrigger", "Retrigger", false);
}

juce::StringArray Arpeggiator::styleNames()
{
    return {"Up", "Down", "Up/Down", "Down/Up", "Converge", "Diverge", "As Played", "Random", "Chord"};
}

juce::StringArray Arpeggiator::rateNames()
{
    return {"1/1", "1/2", "1/2T", "1/4", "1/4T", "1/8D", "1/8", "1/8T", "1/16D", "1/16", "1/16T", "1/32", "1/32T"};
}

double Arpeggiator::rateBeats(int index) noexcept
{
    static constexpr std::array<double, 13> beats{4.0, 2.0, 4.0 / 3.0, 1.0, 2.0 / 3.0, 0.75, 0.5, 1.0 / 3.0, 0.375, 0.25, 1.0 / 6.0, 0.125, 1.0 / 12.0};
    return beats[static_cast<std::size_t>(std::clamp(index, 0, 12))];
}

int Arpeggiator::buildSequence(int style, const Held* held, int count, int octaves, std::array<Held, kMaxSequence>& expanded, int& expandedCount,
                               std::array<int, kMaxSequence>& steps) noexcept
{
    expandedCount = 0;
    count = std::clamp(count, 0, kMaxHeld);
    octaves = std::clamp(octaves, 1, 4);
    if (count == 0)
        return 0;

    // The held notes low to high (or in the order played), then again an octave up, and so on.
    std::array<Held, kMaxHeld> base{};
    std::copy_n(held, count, base.begin());
    if (style != asPlayed) // (an insertion sort: stable, and no buffer to allocate as std::stable_sort would)
        for (int i = 1; i < count; ++i)
            for (int j = i; j > 0 && base[static_cast<std::size_t>(j)].note < base[static_cast<std::size_t>(j - 1)].note; --j)
                std::swap(base[static_cast<std::size_t>(j)], base[static_cast<std::size_t>(j - 1)]);
    for (int o = 0; o < octaves; ++o)
        for (int i = 0; i < count; ++i)
            if (const int note = base[static_cast<std::size_t>(i)].note + 12 * o; note <= 127 && expandedCount < kMaxSequence)
            {
                auto entry = base[static_cast<std::size_t>(i)];
                entry.note = note;
                entry.octave = o;
                expanded[static_cast<std::size_t>(expandedCount++)] = entry;
            }

    const int n = expandedCount;
    int length = 0;
    const auto push = [&](int index)
    {
        if (length < kMaxSequence)
            steps[static_cast<std::size_t>(length++)] = index;
    };
    switch (style)
    {
    case chord:
        for (int o = 0; o < octaves; ++o)
            push(o);
        break;
    case down:
        for (int i = n - 1; i >= 0; --i)
            push(i);
        break;
    case upDown:
        for (int i = 0; i < n; ++i)
            push(i);
        for (int i = n - 2; i >= 1; --i)
            push(i);
        break;
    case downUp:
        for (int i = n - 1; i >= 0; --i)
            push(i);
        for (int i = 1; i <= n - 2; ++i)
            push(i);
        break;
    case converge:
    case diverge:
    {
        std::array<int, kMaxSequence> order{};
        int count2 = 0;
        for (int lo = 0, hi = n - 1; lo <= hi;)
        {
            order[static_cast<std::size_t>(count2++)] = lo++;
            if (lo <= hi)
                order[static_cast<std::size_t>(count2++)] = hi--;
        }
        for (int i = 0; i < count2; ++i)
            push(order[static_cast<std::size_t>(style == converge ? i : count2 - 1 - i)]);
        break;
    }
    default: // up, as played, random (which picks among them)
        for (int i = 0; i < n; ++i)
            push(i);
        break;
    }
    return length;
}

int Arpeggiator::getShownNote(int step) const noexcept
{
    return step >= 0 && step < kShownSteps ? shownNotes_[static_cast<std::size_t>(step)].load(std::memory_order_relaxed) : -1;
}

void Arpeggiator::prepareMidi()
{
    heldCount_ = keysDown_ = soundingCount_ = 0;
    sequenceLength_ = expandedCount_ = 0;
    stepCount_ = 0;
    untilNext_ = 0.0;
    lastFired_ = -1.0e300;
    wasLocked_ = false;
    startNow_ = false;
    dirty_ = true;
    shownLength_.store(0, std::memory_order_relaxed);
    shownStep_.store(-1, std::memory_order_relaxed);
}

double Arpeggiator::stepStart(long long grid, double stepBeats) const noexcept
{
    const double swing = static_cast<double>(swing_->get()) / 100.0;
    return static_cast<double>(grid) * stepBeats + ((grid & 1) != 0 ? swing * 0.5 * stepBeats : 0.0);
}

void Arpeggiator::rebuild() noexcept
{
    builtStyle_ = style_->getIndex();
    builtOctaves_ = octaves_->getIndex() + 1;
    sequenceLength_ = buildSequence(builtStyle_, held_.data(), heldCount_, builtOctaves_, expanded_, expandedCount_, sequence_);
    dirty_ = false;
    const int shown = std::min(sequenceLength_, kShownSteps);
    for (int i = 0; i < shown; ++i)
    {
        int note = -1;
        if (builtStyle_ == chord)
        {
            for (int e = 0; e < expandedCount_; ++e)
                if (expanded_[static_cast<std::size_t>(e)].octave == sequence_[static_cast<std::size_t>(i)])
                {
                    note = expanded_[static_cast<std::size_t>(e)].note;
                    break;
                }
        }
        else
            note = expanded_[static_cast<std::size_t>(sequence_[static_cast<std::size_t>(i)])].note;
        shownNotes_[static_cast<std::size_t>(i)].store(note, std::memory_order_relaxed);
    }
    shownLength_.store(shown, std::memory_order_relaxed);
}

void Arpeggiator::noteOn(int note, int velocity, int channel, bool locked) noexcept
{
    const bool wasEmpty = heldCount_ == 0;
    if (hold_->get() && keysDown_ == 0)
        heldCount_ = 0; // a new chord replaces the one held
    keysDown_ = std::min(keysDown_ + 1, kMaxHeld);
    for (int i = 0; i < heldCount_; ++i)
        if (held_[static_cast<std::size_t>(i)].note == note)
        {
            std::move(held_.begin() + i + 1, held_.begin() + heldCount_, held_.begin() + i);
            --heldCount_;
            break;
        }
    if (heldCount_ < kMaxHeld)
        held_[static_cast<std::size_t>(heldCount_++)] = {note, velocity, channel, 0};
    if (wasEmpty || heldCount_ == 1 || retrigger_->get())
    {
        stepCount_ = 0;
        if (!locked)
            startNow_ = true;
    }
    dirty_ = true;
}

void Arpeggiator::noteOff(int note) noexcept
{
    keysDown_ = std::max(0, keysDown_ - 1);
    if (hold_->get())
        return;
    for (int i = 0; i < heldCount_; ++i)
        if (held_[static_cast<std::size_t>(i)].note == note)
        {
            std::move(held_.begin() + i + 1, held_.begin() + heldCount_, held_.begin() + i);
            --heldCount_;
            dirty_ = true;
            break;
        }
}

void Arpeggiator::releaseAll(juce::MidiBuffer& out, int sample) noexcept
{
    for (int i = 0; i < soundingCount_; ++i)
        addNoteOff(out, sample, sounding_[static_cast<std::size_t>(i)].channel, sounding_[static_cast<std::size_t>(i)].note);
    soundingCount_ = 0;
}

void Arpeggiator::startNote(juce::MidiBuffer& out, int sample, const Held& note, double gateSamples) noexcept
{
    // The same note still sounding (a long gate): end it first.
    for (int i = 0; i < soundingCount_; ++i)
        if (sounding_[static_cast<std::size_t>(i)].note == note.note && sounding_[static_cast<std::size_t>(i)].channel == note.channel)
        {
            addNoteOff(out, sample, note.channel, note.note);
            sounding_[static_cast<std::size_t>(i)] = sounding_[static_cast<std::size_t>(--soundingCount_)];
            break;
        }
    if (soundingCount_ == kMaxSounding) // (full: the oldest goes)
    {
        addNoteOff(out, sample, sounding_[0].channel, sounding_[0].note);
        std::move(sounding_.begin() + 1, sounding_.begin() + soundingCount_, sounding_.begin());
        --soundingCount_;
    }
    const int velocity = velocityMode_->getIndex() == 1 ? juce::roundToInt(velocity_->get()) : note.velocity;
    addNoteOn(out, sample, note.channel, note.note, velocity);
    sounding_[static_cast<std::size_t>(soundingCount_++)] = {note.note, note.channel, gateSamples};
}

void Arpeggiator::step(juce::MidiBuffer& out, int sample, double stepSamples) noexcept
{
    if (heldCount_ == 0)
        return;
    if (dirty_)
        rebuild();
    if (sequenceLength_ == 0)
        return;

    const int repeats = repeats_->getIndex() + 1;
    int index = 0;
    if (builtStyle_ == random)
    {
        if (stepCount_ % repeats == 0 || lastRandom_ < 0 || lastRandom_ >= sequenceLength_)
        {
            randomState_ = randomState_ * 1664525u + 1013904223u;
            int pick = static_cast<int>((randomState_ >> 8) % static_cast<std::uint32_t>(sequenceLength_));
            if (sequenceLength_ > 1 && pick == lastRandom_)
                pick = (pick + 1) % sequenceLength_;
            lastRandom_ = pick;
        }
        index = lastRandom_;
    }
    else
        index = static_cast<int>((stepCount_ / repeats) % sequenceLength_);

    const double gateSamples = std::max(1.0, stepSamples * static_cast<double>(gate_->get()) / 100.0);
    if (builtStyle_ == chord)
    {
        for (int e = 0; e < expandedCount_; ++e)
            if (expanded_[static_cast<std::size_t>(e)].octave == sequence_[static_cast<std::size_t>(index)])
                startNote(out, sample, expanded_[static_cast<std::size_t>(e)], gateSamples);
    }
    else
        startNote(out, sample, expanded_[static_cast<std::size_t>(sequence_[static_cast<std::size_t>(index)])], gateSamples);
    shownStep_.store(index < kShownSteps ? index : -1, std::memory_order_relaxed);
    ++stepCount_;
}

void Arpeggiator::processMidi(const juce::MidiBuffer& in, juce::MidiBuffer& out, int numSamples) noexcept
{
    const auto t = transport();
    const double samplesPerBeat = sampleRate_ * 60.0 / t.bpm;
    const bool synced = sync_->get();
    const double stepBeats = synced ? rateBeats(rate_->getIndex()) : t.bpm / 60.0 / std::max(0.01, static_cast<double>(freeRate_->get()));
    const double stepSamples = std::max(1.0, stepBeats * samplesPerBeat);
    const bool locked = synced && t.playing;
    const double swing = static_cast<double>(swing_->get()) / 100.0;

    if (!hold_->get() && keysDown_ == 0 && heldCount_ > 0) // (Hold was switched off)
    {
        heldCount_ = 0;
        dirty_ = true;
    }
    if (style_->getIndex() != builtStyle_ || octaves_->getIndex() + 1 != builtOctaves_)
        dirty_ = true;

    // While the song plays, the steps sit on its grid. Find the next one again when the transport
    // starts or jumps, or the rate, swing or tempo changes; otherwise the clock just runs on.
    if (locked)
    {
        const double sampleBeats = 1.0 / samplesPerBeat;
        const bool jumped = !wasLocked_ || std::abs(t.ppq - expectedPpq_) > 2.0 * sampleBeats;
        if (jumped)
            lastFired_ = -1.0e300;
        if (jumped || std::abs(stepBeats - lockedBeats_) > 1.0e-12 || std::abs(swing - lockedSwing_) > 1.0e-12 ||
            std::abs(t.bpm - lockedBpm_) > 1.0e-9)
        {
            auto grid = static_cast<long long>(std::floor(t.ppq / stepBeats)) - 1;
            while (stepStart(grid, stepBeats) < t.ppq - sampleBeats || stepStart(grid, stepBeats) <= lastFired_ + sampleBeats * 0.5)
                ++grid;
            nextGrid_ = grid;
            untilNext_ = std::max(0.0, (stepStart(grid, stepBeats) - t.ppq) * samplesPerBeat);
            lockedBeats_ = stepBeats;
            lockedSwing_ = swing;
            lockedBpm_ = t.bpm;
        }
        expectedPpq_ = t.ppq + static_cast<double>(numSamples) * sampleBeats;
    }
    wasLocked_ = locked;

    auto event = in.cbegin();
    const auto end = in.cend();
    for (int s = 0; s < numSamples; ++s)
    {
        // This sample's events: the notes go into the pattern, everything else straight through.
        for (; event != end && (*event).samplePosition <= s; ++event)
        {
            const auto raw = rawOf(*event);
            if (raw.isNoteOn())
                noteOn(raw.byte(1), raw.byte(2), raw.channel(), locked);
            else if (raw.isNoteOff())
                noteOff(raw.byte(1));
            else
            {
                if (raw.isPanic())
                {
                    releaseAll(out, s);
                    heldCount_ = keysDown_ = 0;
                    dirty_ = true;
                }
                out.addEvent(raw.data, raw.size, s);
            }
        }

        // The notes playing end with their gate.
        for (int i = 0; i < soundingCount_;)
        {
            auto& sounding = sounding_[static_cast<std::size_t>(i)];
            sounding.samplesLeft -= 1.0;
            if (sounding.samplesLeft <= 0.0)
            {
                addNoteOff(out, s, sounding.channel, sounding.note);
                sounding = sounding_[static_cast<std::size_t>(--soundingCount_)];
            }
            else
                ++i;
        }

        // The clock.
        if (startNow_)
        {
            untilNext_ = static_cast<double>(s);
            freeParity_ = 0;
            startNow_ = false;
        }
        if (static_cast<double>(s) >= untilNext_ - 1.0e-9)
        {
            step(out, s, stepSamples);
            if (locked)
            {
                lastFired_ = stepStart(nextGrid_, stepBeats);
                ++nextGrid_;
                untilNext_ = (stepStart(nextGrid_, stepBeats) - t.ppq) * samplesPerBeat;
            }
            else
            {
                // Free-running: every second step later by the swing.
                untilNext_ += stepSamples * (freeParity_ == 0 ? 1.0 + swing * 0.5 : 1.0 - swing * 0.5);
                freeParity_ ^= 1;
            }
        }
        if (heldCount_ == 0)
            shownStep_.store(-1, std::memory_order_relaxed);
    }
    untilNext_ -= static_cast<double>(numSamples);
    if (dirty_ && heldCount_ == 0)
        rebuild(); // (the display empties)
}

//==================================================================================================
// Chord Generator

namespace
{
struct Shape
{
    std::array<int, 6> intervals;
    int count;
};

const std::array<Shape, 20>& shapes()
{
    static const std::array<Shape, 20> table{{
        {{0}, 1},                   // Custom (the shifts)
        {{0, 4, 7}, 3},             // Major
        {{0, 3, 7}, 3},             // Minor
        {{0, 3, 6}, 3},             // Dim
        {{0, 4, 8}, 3},             // Aug
        {{0, 2, 7}, 3},             // Sus2
        {{0, 5, 7}, 3},             // Sus4
        {{0, 4, 7, 9}, 4},          // 6
        {{0, 3, 7, 9}, 4},          // m6
        {{0, 4, 7, 10}, 4},         // 7
        {{0, 4, 7, 11}, 4},         // Maj7
        {{0, 3, 7, 10}, 4},         // m7
        {{0, 3, 6, 10}, 4},         // m7b5
        {{0, 3, 6, 9}, 4},          // Dim7
        {{0, 4, 7, 10, 14}, 5},     // 9
        {{0, 4, 7, 11, 14}, 5},     // Maj9
        {{0, 3, 7, 10, 14}, 5},     // m9
        {{0, 4, 7, 14}, 4},         // Add9
        {{0, 7, 12}, 3},            // Power
        {{0, 12}, 2},               // Octave
    }};
    return table;
}
} // namespace

ChordGenerator::ChordGenerator()
    : MidiEffect(kTypeId, "Chord Generator")
{
    mode_ = addChoice("mode", "Mode", {"Shape", "Diatonic"}, shapeMode);
    shape_ = addChoice("shape", "Shape", shapeNames(), 1);
    for (int i = 0; i < kShifts; ++i)
        shift_[static_cast<std::size_t>(i)] = addFloat("shift" + juce::String(i + 1), "Shift " + juce::String(i + 1),
                                                       juce::NormalisableRange<float>(-24.0f, 24.0f, 1.0f), 0.0f, "st", true, 0);
    key_ = addChoice("key", "Key", scales::keyNames(), 0);
    scale_ = addChoice("scale", "Scale", scales::names(), 0);
    diatonic_ = addChoice("type", "Chord", diatonicNames(), 0);
    inversion_ = addChoice("inversion", "Inversion", {"Root", "1st", "2nd", "3rd"}, 0);
    voicing_ = addChoice("voicing", "Voicing", {"Close", "Drop 2", "Spread"}, 0);
    added_ = addFloat("velocity", "Velocity", juce::NormalisableRange<float>(0.0f, 200.0f, 1.0f), 100.0f, "%", true, 0);
    strum_ = addFloat("strum", "Strum", juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 0.0f, "ms", true, 1);
    strumDirection_ = addChoice("strumdir", "Strum Dir", {"Up", "Down"}, 0);
}

juce::StringArray ChordGenerator::shapeNames()
{
    return {"Custom", "Major", "Minor", "Dim", "Aug", "Sus2", "Sus4", "6", "m6", "7", "Maj7", "m7", "m7b5", "Dim7", "9", "Maj9", "m9", "Add9",
            "Power", "Octave"};
}

juce::StringArray ChordGenerator::diatonicNames()
{
    return {"Triad", "Seventh", "Ninth", "Sus2", "Sus4", "Power"};
}

int ChordGenerator::chordFor(int note, std::array<int, kMaxChord>& notes) const noexcept
{
    int count = 0;
    const auto add = [&](int n)
    {
        if (n < 0 || n > 127 || count >= kMaxChord)
            return;
        for (int i = 0; i < count; ++i)
            if (notes[static_cast<std::size_t>(i)] == n)
                return;
        notes[static_cast<std::size_t>(count++)] = n;
    };
    const auto sortUp = [&] { std::sort(notes.begin(), notes.begin() + count); };

    if (mode_->getIndex() == diatonicMode)
    {
        // The chord the note's scale degree has in the key.
        const int root = key_->getIndex();
        const auto mask = scales::all()[static_cast<std::size_t>(scale_->getIndex())].mask;
        static constexpr std::array<std::array<int, 5>, 6> degrees{{{0, 2, 4, -1, -1}, {0, 2, 4, 6, -1}, {0, 2, 4, 6, 8},
                                                                    {0, 1, 4, -1, -1}, {0, 3, 4, -1, -1}, {0, 4, 7, -1, -1}}};
        const int base = scales::snap(note, root, mask, scales::nearest);
        if (base < 0)
            add(note);
        else
            for (const int d : degrees[static_cast<std::size_t>(diatonic_->getIndex())])
                if (d >= 0)
                    add(scales::shiftDegrees(base, root, mask, d));
    }
    else if (shape_->getIndex() == 0)
    {
        add(note);
        for (const auto* shift : shift_)
            if (const int s = juce::roundToInt(shift->get()); s != 0)
                add(note + s);
    }
    else
    {
        const auto& shape = shapes()[static_cast<std::size_t>(shape_->getIndex())];
        for (int i = 0; i < shape.count; ++i)
            add(note + shape.intervals[static_cast<std::size_t>(i)]);
    }
    sortUp();

    // Inversion: the lowest notes up an octave.
    for (int i = 0; i < std::min(inversion_->getIndex(), count - 1); ++i)
    {
        const int lowest = notes[0];
        std::move(notes.begin() + 1, notes.begin() + count, notes.begin());
        --count;
        add(lowest + 12 <= 127 ? lowest + 12 : lowest);
        sortUp();
    }
    // Voicing: Drop 2 (the second-highest note down an octave), Spread (every other note up one).
    if (count >= 3 && voicing_->getIndex() == 1 && notes[static_cast<std::size_t>(count - 2)] >= 12)
    {
        notes[static_cast<std::size_t>(count - 2)] -= 12;
        sortUp();
    }
    else if (count >= 3 && voicing_->getIndex() == 2)
    {
        for (int i = 1; i < count; i += 2)
            if (notes[static_cast<std::size_t>(i)] + 12 <= 127)
                notes[static_cast<std::size_t>(i)] += 12;
        sortUp();
        count = static_cast<int>(std::unique(notes.begin(), notes.begin() + count) - notes.begin());
    }
    if (strumDirection_->getIndex() == 1)
        std::reverse(notes.begin(), notes.begin() + count);
    return count;
}

int ChordGenerator::getShownNote(int index) const noexcept
{
    return index >= 0 && index < kMaxChord ? shownNotes_[static_cast<std::size_t>(index)].load(std::memory_order_relaxed) : -1;
}

void ChordGenerator::prepareMidi()
{
    for (auto& channel : chords_)
        for (auto& chord : channel)
            chord.count = 0;
    for (auto& channel : sounding_)
        channel.fill(0);
    pendingCount_ = 0;
}

void ChordGenerator::noteOnOut(juce::MidiBuffer& out, int sample, int channel, int note, int velocity) noexcept
{
    auto& count = sounding_[static_cast<std::size_t>(channel - 1)][static_cast<std::size_t>(note)];
    count = static_cast<std::uint8_t>(std::min(255, count + 1));
    addNoteOn(out, sample, channel, note, velocity);
}

void ChordGenerator::noteOffOut(juce::MidiBuffer& out, int sample, int channel, int note) noexcept
{
    auto& count = sounding_[static_cast<std::size_t>(channel - 1)][static_cast<std::size_t>(note)];
    if (count == 0)
        return;
    if (--count == 0)
        addNoteOff(out, sample, channel, note);
}

void ChordGenerator::play(juce::MidiBuffer& out, int sample, int channel, int note, int velocity) noexcept
{
    auto& chord = chords_[static_cast<std::size_t>(channel - 1)][static_cast<std::size_t>(note)];
    if (chord.count > 0)
        release(out, sample, channel, note); // (the same key again)

    std::array<int, kMaxChord> notes{};
    const int count = chordFor(note, notes);
    const double strumSamples = static_cast<double>(strum_->get()) * sampleRate_ / 1000.0;
    const int added = std::clamp(juce::roundToInt(static_cast<float>(velocity) * added_->get() / 100.0f), 1, 127);
    // (the chord's own root keeps the played velocity)
    const int root = mode_->getIndex() == diatonicMode ? scales::snap(note, key_->getIndex(),
                                                                     scales::all()[static_cast<std::size_t>(scale_->getIndex())].mask, scales::nearest)
                                                       : note;
    chord.count = 0;
    for (int k = 0; k < count; ++k)
    {
        const int n = notes[static_cast<std::size_t>(k)];
        const int v = n == root ? velocity : added;
        chord.notes[static_cast<std::size_t>(chord.count++)] = static_cast<std::int8_t>(n);
        const double delay = static_cast<double>(k) * strumSamples;
        if (delay < 1.0 || pendingCount_ >= kMaxPending)
            noteOnOut(out, sample, channel, n, v);
        else
            pending_[static_cast<std::size_t>(pendingCount_++)] = {n, channel, v, note, static_cast<double>(sample) + delay};
    }
    shownCount_.store(count, std::memory_order_relaxed);
    for (int k = 0; k < kMaxChord; ++k)
        shownNotes_[static_cast<std::size_t>(k)].store(k < count ? notes[static_cast<std::size_t>(k)] : -1, std::memory_order_relaxed);
}

void ChordGenerator::release(juce::MidiBuffer& out, int sample, int channel, int note) noexcept
{
    auto& chord = chords_[static_cast<std::size_t>(channel - 1)][static_cast<std::size_t>(note)];
    for (int k = 0; k < chord.count; ++k)
    {
        const int n = chord.notes[static_cast<std::size_t>(k)];
        bool wasPending = false;
        for (int p = 0; p < pendingCount_; ++p)
            if (const auto& pending = pending_[static_cast<std::size_t>(p)];
                pending.source == note && pending.channel == channel && pending.note == n)
            {
                pending_[static_cast<std::size_t>(p)] = pending_[static_cast<std::size_t>(--pendingCount_)]; // (never played: no release)
                wasPending = true;
                break;
            }
        if (!wasPending)
            noteOffOut(out, sample, channel, n);
    }
    chord.count = 0;
}

void ChordGenerator::releaseAll(juce::MidiBuffer& out, int sample) noexcept
{
    for (int c = 0; c < 16; ++c)
        for (int n = 0; n < 128; ++n)
        {
            chords_[static_cast<std::size_t>(c)][static_cast<std::size_t>(n)].count = 0;
            if (auto& count = sounding_[static_cast<std::size_t>(c)][static_cast<std::size_t>(n)]; count > 0)
            {
                count = 0;
                addNoteOff(out, sample, c + 1, n);
            }
        }
    pendingCount_ = 0;
}

void ChordGenerator::processMidi(const juce::MidiBuffer& in, juce::MidiBuffer& out, int numSamples) noexcept
{
    // Strummed notes due by `sample` (pending ones count from this block's start).
    const auto flush = [&](double until)
    {
        for (int p = 0; p < pendingCount_;)
        {
            const auto pending = pending_[static_cast<std::size_t>(p)];
            if (pending.samplesLeft <= until)
            {
                noteOnOut(out, std::clamp(static_cast<int>(std::ceil(pending.samplesLeft)), 0, std::max(0, numSamples - 1)), pending.channel,
                          pending.note, pending.velocity);
                pending_[static_cast<std::size_t>(p)] = pending_[static_cast<std::size_t>(--pendingCount_)];
            }
            else
                ++p;
        }
    };

    for (const auto meta : in)
    {
        const auto raw = rawOf(meta);
        const int s = meta.samplePosition;
        flush(static_cast<double>(s));
        if (raw.isNoteOn())
            play(out, s, raw.channel(), raw.byte(1), raw.byte(2));
        else if (raw.isNoteOff())
            release(out, s, raw.channel(), raw.byte(1));
        else if (raw.isPolyAftertouch())
        {
            const auto& chord = chords_[static_cast<std::size_t>(raw.channel() - 1)][static_cast<std::size_t>(raw.byte(1))];
            for (int k = 0; k < chord.count; ++k)
            {
                const juce::uint8 bytes[3] = {static_cast<juce::uint8>(raw.status()), static_cast<juce::uint8>(chord.notes[static_cast<std::size_t>(k)]),
                                              static_cast<juce::uint8>(raw.byte(2))};
                out.addEvent(bytes, 3, s);
            }
        }
        else
        {
            if (raw.isPanic())
                releaseAll(out, s);
            out.addEvent(raw.data, raw.size, s);
        }
    }
    flush(static_cast<double>(numSamples) - 1.0e-9);
    for (int p = 0; p < pendingCount_; ++p)
        pending_[static_cast<std::size_t>(p)].samplesLeft -= static_cast<double>(numSamples);
}

//==================================================================================================
// Scale

ScaleDevice::ScaleDevice()
    : MidiEffect(kTypeId, "Scale")
{
    root_ = addChoice("root", "Root", scales::keyNames(), 0);
    scale_ = addChoice("scale", "Scale", scales::names(), 0);
    mode_ = addChoice("mode", "Mode", modeNames(), nearestMode);
    transpose_ = addFloat("transpose", "Transpose", juce::NormalisableRange<float>(-24.0f, 24.0f, 1.0f), 0.0f, "st", true, 0);
    shift_ = addFloat("shift", "Shift", juce::NormalisableRange<float>(-7.0f, 7.0f, 1.0f), 0.0f, "deg", true, 0);
    octave_ = addFloat("octave", "Octave", juce::NormalisableRange<float>(-3.0f, 3.0f, 1.0f), 0.0f, "oct", true, 0);
    prepareMidi();
}

juce::StringArray ScaleDevice::modeNames()
{
    return {"Nearest", "Up", "Down", "Remove", "White Keys"};
}

std::uint16_t ScaleDevice::getMask() const noexcept
{
    return scales::all()[static_cast<std::size_t>(scale_->getIndex())].mask;
}

int ScaleDevice::map(int note) const noexcept
{
    const int root = root_->getIndex();
    const auto mask = getMask();
    const int x = note + juce::roundToInt(transpose_->get());
    if (x < 0 || x > 127)
        return -1;
    int y = -1;
    switch (mode_->getIndex())
    {
    case upMode:
        y = scales::snap(x, root, mask, scales::up);
        break;
    case downMode:
        y = scales::snap(x, root, mask, scales::down);
        break;
    case removeMode:
        y = scales::snap(x, root, mask, scales::nearest) == x ? x : -1;
        break;
    case whiteKeysMode:
        y = scales::whiteKey(x, root, mask);
        break;
    default:
        y = scales::snap(x, root, mask, scales::nearest);
        break;
    }
    if (y < 0)
        return -1;
    if (const int degrees = juce::roundToInt(shift_->get()); degrees != 0)
        y = scales::shiftDegrees(y, root, mask, degrees);
    if (y < 0)
        return -1;
    y += 12 * juce::roundToInt(octave_->get());
    return y >= 0 && y <= 127 ? y : -1;
}

void ScaleDevice::prepareMidi()
{
    for (auto& channel : mapped_)
        channel.fill(-1);
    for (auto& channel : sounding_)
        channel.fill(0);
}

void ScaleDevice::processMidi(const juce::MidiBuffer& in, juce::MidiBuffer& out, int) noexcept
{
    for (const auto meta : in)
    {
        const auto raw = rawOf(meta);
        const int s = meta.samplePosition;
        if (raw.isNoteOn() || raw.isNoteOff() || raw.isPolyAftertouch())
        {
            const auto ch = static_cast<std::size_t>(raw.channel() - 1);
            const auto note = static_cast<std::size_t>(raw.byte(1));
            if (raw.isNoteOn())
            {
                if (mapped_[ch][note] >= 0) // (pressed again before its release: let the old one go)
                {
                    const auto old = static_cast<std::size_t>(mapped_[ch][note]);
                    if (sounding_[ch][old] > 0 && --sounding_[ch][old] == 0)
                        addNoteOff(out, s, raw.channel(), static_cast<int>(old));
                }
                const int to = map(raw.byte(1));
                mapped_[ch][note] = static_cast<std::int16_t>(to);
                lastIn_.store(raw.byte(1), std::memory_order_relaxed);
                lastOut_.store(to, std::memory_order_relaxed);
                if (to >= 0)
                {
                    auto& count = sounding_[ch][static_cast<std::size_t>(to)];
                    count = static_cast<std::uint8_t>(std::min(255, count + 1));
                    addNoteOn(out, s, raw.channel(), to, raw.byte(2));
                }
            }
            else if (const int to = mapped_[ch][note]; to >= 0)
            {
                if (raw.isPolyAftertouch())
                {
                    const juce::uint8 bytes[3] = {static_cast<juce::uint8>(raw.status()), static_cast<juce::uint8>(to), static_cast<juce::uint8>(raw.byte(2))};
                    out.addEvent(bytes, 3, s);
                    continue;
                }
                mapped_[ch][note] = -1;
                auto& count = sounding_[ch][static_cast<std::size_t>(to)];
                if (count > 0 && --count == 0)
                    addNoteOff(out, s, raw.channel(), to, raw.byte(2));
            }
            continue;
        }
        if (raw.isPanic())
            for (int c = 0; c < 16; ++c)
                for (int n = 0; n < 128; ++n)
                {
                    mapped_[static_cast<std::size_t>(c)][static_cast<std::size_t>(n)] = -1;
                    if (auto& count = sounding_[static_cast<std::size_t>(c)][static_cast<std::size_t>(n)]; count > 0)
                    {
                        count = 0;
                        addNoteOff(out, s, c + 1, n);
                    }
                }
        out.addEvent(raw.data, raw.size, s);
    }
}

//==================================================================================================
// MIDI Filter

namespace
{
juce::String noteText(float value)
{
    return juce::MidiMessage::getMidiNoteName(juce::roundToInt(value), true, true, 3);
}

float noteFromText(const juce::String& text)
{
    const auto t = text.trim().toUpperCase();
    if (t.isEmpty())
        return 0.0f;
    static const juce::String letters = "C D EF G A B";
    const int letter = letters.indexOfChar(t[0]);
    if (letter < 0)
        return static_cast<float>(std::clamp(t.getIntValue(), 0, 127));
    int pc = letter;
    int at = 1;
    if (t.length() > 1 && (t[1] == '#' || t[1] == 'B'))
    {
        pc += t[1] == '#' ? 1 : -1;
        at = 2;
    }
    const int octave = t.substring(at).getIntValue();
    return static_cast<float>(std::clamp((octave + 2) * 12 + pc, 0, 127));
}
} // namespace

MidiFilter::MidiFilter()
    : MidiEffect(kTypeId, "MIDI Filter")
{
    lowNote_ = addFloatWithText("low", "Lowest", juce::NormalisableRange<float>(0.0f, 127.0f, 1.0f), 0.0f, {}, noteText, noteFromText);
    highNote_ = addFloatWithText("high", "Highest", juce::NormalisableRange<float>(0.0f, 127.0f, 1.0f), 127.0f, {}, noteText, noteFromText);
    outside_ = addBool("outside", "Outside", false);
    velocityLow_ = addFloat("vellow", "Vel Min", juce::NormalisableRange<float>(1.0f, 127.0f, 1.0f), 1.0f, {}, true, 0);
    velocityHigh_ = addFloat("velhigh", "Vel Max", juce::NormalisableRange<float>(1.0f, 127.0f, 1.0f), 127.0f, {}, true, 0);
    velocityScale_ = addFloat("velscale", "Vel Scale", juce::NormalisableRange<float>(0.0f, 200.0f, 1.0f), 100.0f, "%", true, 0);
    velocityOffset_ = addFloat("veloffset", "Vel Offset", juce::NormalisableRange<float>(-64.0f, 64.0f, 1.0f), 0.0f, {}, true, 0);
    channel_ = addChoice("channel", "Channel", numbered("All", 16), 0);
    outChannel_ = addChoice("outchannel", "Out Channel", numbered("Keep", 16), 0);
    allow_[noteKind] = addBool("notes", "Notes", true);
    allow_[controlKind] = addBool("cc", "Controllers", true);
    allow_[pitchBendKind] = addBool("pitchbend", "Pitch Bend", true);
    allow_[aftertouchKind] = addBool("aftertouch", "Aftertouch", true);
    allow_[programKind] = addBool("program", "Program", true);
    allow_[otherKind] = addBool("other", "Other", true);
}

void MidiFilter::prepareMidi()
{
    for (auto& channel : held_)
        channel.fill(0);
}

void MidiFilter::processMidi(const juce::MidiBuffer& in, juce::MidiBuffer& out, int) noexcept
{
    const int low = std::min(juce::roundToInt(lowNote_->get()), juce::roundToInt(highNote_->get()));
    const int high = std::max(juce::roundToInt(lowNote_->get()), juce::roundToInt(highNote_->get()));
    const int velocityLow = std::min(juce::roundToInt(velocityLow_->get()), juce::roundToInt(velocityHigh_->get()));
    const int velocityHigh = std::max(juce::roundToInt(velocityLow_->get()), juce::roundToInt(velocityHigh_->get()));
    const int onlyChannel = channel_->getIndex();  // 0: all
    const int outChannel = outChannel_->getIndex(); // 0: keep
    const auto count = [this](int kind, bool passed)
    {
        seen_[static_cast<std::size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
        if (passed)
            passed_[static_cast<std::size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
    };

    for (const auto meta : in)
    {
        const auto raw = rawOf(meta);
        const int s = meta.samplePosition;
        if (!raw.isChannelMessage())
        {
            const bool pass = allow_[otherKind]->get();
            count(otherKind, pass);
            if (pass)
                out.addEvent(raw.data, raw.size, s);
            continue;
        }
        const int channel = raw.channel();
        const auto ch = static_cast<std::size_t>(channel - 1);
        const bool channelOk = onlyChannel == 0 || onlyChannel == channel;
        const int to = outChannel == 0 ? channel : outChannel;

        if (raw.isNoteOn())
        {
            const int note = raw.byte(1), velocity = raw.byte(2);
            const bool inRange = note >= low && note <= high;
            const bool pass = allow_[noteKind]->get() && channelOk && (inRange != outside_->get()) && velocity >= velocityLow &&
                              velocity <= velocityHigh;
            count(noteKind, pass);
            if (pass)
            {
                const int shaped = juce::roundToInt(static_cast<float>(velocity) * velocityScale_->get() / 100.0f + velocityOffset_->get());
                held_[ch][static_cast<std::size_t>(note)] = static_cast<std::int8_t>(to);
                addNoteOn(out, s, to, note, std::clamp(shaped, 1, 127));
            }
        }
        else if (raw.isNoteOff())
        {
            // A note let through always gets its release (whatever changed since).
            auto& went = held_[ch][static_cast<std::size_t>(raw.byte(1))];
            count(noteKind, went != 0);
            if (went != 0)
            {
                addNoteOff(out, s, went, raw.byte(1), raw.byte(2));
                went = 0;
            }
        }
        else if (raw.isChannelMode())
        {
            // All notes off and the like always pass, and end what this channel let through.
            count(controlKind, true);
            for (int n = 0; n < 128; ++n)
                if (auto& went = held_[ch][static_cast<std::size_t>(n)]; went != 0)
                {
                    if (went != channel)
                        addNoteOff(out, s, went, n);
                    went = 0;
                }
            out.addEvent(raw.data, raw.size, s);
        }
        else
        {
            const int kind = raw.type() == 0xb0   ? controlKind
                             : raw.type() == 0xe0 ? pitchBendKind
                             : raw.type() == 0xa0 || raw.type() == 0xd0 ? aftertouchKind
                             : raw.type() == 0xc0 ? programKind
                                                  : otherKind;
            bool pass = allow_[static_cast<std::size_t>(kind)]->get() && channelOk;
            int outTo = to;
            if (raw.isPolyAftertouch()) // (follows its note)
            {
                const int went = held_[ch][static_cast<std::size_t>(raw.byte(1))];
                pass = pass && went != 0;
                outTo = went != 0 ? went : to;
            }
            count(kind, pass);
            if (pass)
                addRechannelled(out, s, raw, outTo);
        }
    }
}

} // namespace dojo::devices
