#include "devices/DeviceRegistry.h"
#include "devices/RhodesSuitcase.h"
#include "session/Project.h"
#include "EffectTestHelpers.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace dojo;
using namespace dojo::test::devices;
using Catch::Matchers::WithinAbs;
using devices::RhodesSuitcase;
using devices::SuitcaseStage;

namespace
{
double level(double db)
{
    return std::pow(10.0, db / 20.0);
}

using Setup = std::function<void(RhodesSuitcase&)>;

/** Plays notes (note, velocity, start s, end s or < 0 to hold) through a fresh Rhodes. */
struct Note
{
    int note, velocity;
    double start, end = -1.0;
};
Stereo playNotes(std::vector<Note> notes, double seconds, const Setup& setup = {}, std::vector<TimedMidi> extra = {})
{
    auto [node, rhodes] = makeNode<RhodesSuitcase>();
    setParameter(*rhodes, "intensity", 0.0f);
    setParameter(*rhodes, "out", 1.0f); // (line out: the harp and preamp as they are)
    if (setup)
        setup(*rhodes);
    std::vector<TimedMidi> events = std::move(extra);
    for (const auto& n : notes)
    {
        events.push_back({static_cast<int>(at(n.start)), juce::MidiMessage::noteOn(1, n.note, static_cast<juce::uint8>(n.velocity))});
        if (n.end >= 0.0)
            events.push_back({static_cast<int>(at(n.end)), juce::MidiMessage::noteOff(1, n.note)});
    }
    return run(*node, Stereo(at(seconds)), nullptr, events);
}

double harmonicDb(const Stereo& out, double frequency, int harmonic, double from, double to)
{
    return toDb(amplitudeAt(out.left, frequency * harmonic, at(from), at(to)) / amplitudeAt(out.left, frequency, at(from), at(to)));
}

/** Energy (dB) above `hz` between two times (a 4th-order high-pass). */
double energyAboveDb(const std::vector<double>& x, double hz, double from, double to)
{
    using Shape = devices::dsp::BiquadCoefficients::Shape;
    const auto hp = devices::dsp::BiquadCoefficients::make(Shape::lowCut, kRate, hz, 0.7071, 0.0);
    devices::dsp::Biquad a, b;
    double energy = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        const double y = b.process(a.process(x[i], hp), hp);
        if (i >= at(from) && i < at(to))
            energy += y * y;
    }
    return 10.0 * std::log10(energy + 1e-30);
}

/** Runs a Rhodes in the engine's two halves, block by block: up to its accessory loop's send, then
    `inLoop` on the send (what the loop's devices do; it may replace it), then from the return on. */
using InLoop = std::function<void(juce::AudioBuffer<double>& buffer, std::size_t offset, int n)>;
Stereo runSplit(ProcessorNode& node, std::size_t total, const std::vector<TimedMidi>& events, const InLoop& inLoop)
{
    Stereo out(total);
    juce::AudioBuffer<double> buffer(2, kBlock);
    juce::MidiBuffer midi;
    for (std::size_t offset = 0; offset < total; offset += kBlock)
    {
        const int n = static_cast<int>(std::min<std::size_t>(kBlock, total - offset));
        buffer.clear();
        midi.clear();
        for (const auto& e : events)
            if (e.sample >= static_cast<int>(offset) && e.sample < static_cast<int>(offset) + n)
                midi.addEvent(e.message, e.sample - static_cast<int>(offset));
        node.processSend(buffer, n, midi);
        if (inLoop)
            inLoop(buffer, offset, n);
        node.processReturn(buffer, n, 0);
        for (int i = 0; i < n; ++i)
        {
            out.left[offset + static_cast<std::size_t>(i)] = buffer.getSample(0, i);
            out.right[offset + static_cast<std::size_t>(i)] = buffer.getSample(1, i);
        }
    }
    return out;
}
} // namespace

TEST_CASE("Rhodes: 73 keys from E1 to E7, each at its pitch", "[devices][rhodes]")
{
    CHECK(devices::findBuiltinDevice(RhodesSuitcase::kTypeId)->isInstrument);
    CHECK_THAT(RhodesSuitcase::frequencyOf(69), WithinAbs(440.0, 1e-9));
    for (const int note : {28, 57, 100})
    {
        INFO(note);
        const auto out = playNotes({{note, 90, 0.0}}, 1.2);
        const double f = RhodesSuitcase::frequencyOf(note);
        const double fundamental = amplitudeAt(out.left, f, at(0.1), at(1.1));
        CHECK(fundamental > level(-45.0));
        // The fundamental, not a neighbour.
        CHECK(fundamental > 4.0 * amplitudeAt(out.left, f * std::pow(2.0, 1.0 / 12.0), at(0.1), at(1.1)));
    }
    // Outside the 73 keys: nothing.
    for (const int note : {27, 101})
        CHECK(peakOf(playNotes({{note, 127, 0.0}}, 0.3).left) == 0.0);
    // Tuning moves every key.
    const auto sharp = playNotes({{69, 90, 0.0}}, 0.6, [](auto& r) { setParameter(r, "tuning", 50.0f); setParameter(r, "age", 0.0f); });
    CHECK(amplitudeAt(sharp.left, 440.0 * std::pow(2.0, 0.5 / 12.0), at(0.1), at(0.5)) > 4.0 * amplitudeAt(sharp.left, 440.0, at(0.1), at(0.5)));
}

TEST_CASE("Rhodes: the pickups bark when played hard; voicing, hammer and tine", "[devices][rhodes]")
{
    const double f = RhodesSuitcase::frequencyOf(57);
    // Louder with velocity, and the harmonics grow faster than the fundamental (the pickup's
    // non-linearity: the bark).
    const auto soft = playNotes({{57, 30, 0.0}}, 0.6), hard = playNotes({{57, 127, 0.0}}, 0.6);
    CHECK(rmsOf(hard.left, at(0.1), at(0.5)) > 4.0 * rmsOf(soft.left, at(0.1), at(0.5)));
    CHECK(harmonicDb(hard, f, 2, 0.1, 0.5) > harmonicDb(soft, f, 2, 0.1, 0.5) + 10.0);
    // Voicing: closer to the pickup's centre, more upper harmonics at the same touch.
    const auto barky = playNotes({{57, 127, 0.0}}, 0.6, [](auto& r) { setParameter(r, "voicing", 100.0f); });
    const auto mellow = playNotes({{57, 127, 0.0}}, 0.6, [](auto& r) { setParameter(r, "voicing", 0.0f); });
    CHECK(harmonicDb(barky, f, 3, 0.1, 0.5) > harmonicDb(mellow, f, 3, 0.1, 0.5) + 10.0);
    // Hammer and Tine: the bell at the attack.
    const auto bell = [&](float hammer, float tine)
    {
        const auto out = playNotes({{57, 100, 0.0}}, 0.3, [&](auto& r) { setParameter(r, "hammer", hammer); setParameter(r, "tine", tine); });
        return energyAboveDb(out.left, 1000.0, 0.0, 0.04) - energyAboveDb(out.left, 20.0, 0.0, 0.04);
    };
    CHECK(bell(100.0f, 50.0f) > bell(0.0f, 50.0f) + 3.0);
    CHECK(bell(50.0f, 100.0f) > bell(50.0f, 0.0f) + 6.0);
}

TEST_CASE("Rhodes: tines ring for seconds; dampers, sustain pedal and the undamped top keys", "[devices][rhodes]")
{
    const auto decayPerSecond = [](int note, const Setup& setup = {})
    {
        const auto out = playNotes({{note, 90, 0.0}}, 2.5, setup);
        const double f = RhodesSuitcase::frequencyOf(note);
        return toDb(amplitudeAt(out.left, f, at(1.2), at(1.5)) / amplitudeAt(out.left, f, at(0.2), at(0.5)));
    };
    const double bass = decayPerSecond(33), treble = decayPerSecond(84);
    CHECK(bass > -12.0); // (about 9 s to -60 dB at the bottom)
    CHECK(treble < bass - 10.0);
    CHECK(decayPerSecond(57, [](auto& r) { setParameter(r, "decay", 200.0f); }) > decayPerSecond(57) * 0.6);

    // Release: the felt damps the tine within a fraction of a second.
    const auto released = playNotes({{57, 90, 0.0, 0.3}}, 0.8);
    CHECK(toDb(rmsOf(released.left, at(0.45), at(0.5)) / rmsOf(released.left, at(0.2), at(0.3))) < -30.0);
    // ... unless the sustain pedal holds the dampers off, until it comes up.
    const auto pedal = playNotes({{57, 90, 0.0, 0.3}}, 1.2, {},
                                 {{0, juce::MidiMessage::controllerEvent(1, 64, 127)}, {static_cast<int>(at(0.7)), juce::MidiMessage::controllerEvent(1, 64, 0)}});
    CHECK(toDb(rmsOf(pedal.left, at(0.45), at(0.5)) / rmsOf(pedal.left, at(0.2), at(0.3))) > -6.0);
    CHECK(toDb(rmsOf(pedal.left, at(1.0), at(1.05)) / rmsOf(pedal.left, at(0.2), at(0.3))) < -35.0);
    // The top keys have no dampers: they die away by themselves.
    const int top = RhodesSuitcase::kLowestNote + RhodesSuitcase::kKeys - 1;
    const auto undamped = playNotes({{top, 90, 0.0, 0.1}}, 0.5);
    const auto held = playNotes({{top, 90, 0.0}}, 0.5);
    CHECK_THAT(rmsOf(undamped.left, at(0.2), at(0.3)), WithinAbs(rmsOf(held.left, at(0.2), at(0.3)), 1e-9));
    // A longer Release lets the tine ring longer after the key.
    const auto slowFelt = playNotes({{57, 90, 0.0, 0.3}}, 0.8, [](auto& r) { setParameter(r, "release", 800.0f); });
    CHECK(rmsOf(slowFelt.left, at(0.45), at(0.5)) > 10.0 * rmsOf(released.left, at(0.45), at(0.5)));

    // Striking a ringing key again adds to its swing (no restart, no click).
    const auto again = playNotes({{57, 90, 0.0}, {57, 90, 0.5}}, 0.8);
    double jump = 0.0, normal = 0.0;
    for (std::size_t i = at(0.45); i < at(0.55); ++i)
        (i < at(0.5) ? normal : jump) = std::max(i < at(0.5) ? normal : jump, std::abs(again.left[i] - again.left[i - 1]));
    CHECK(jump < normal * 6.0);
}

TEST_CASE("Rhodes: harp load, the Janus preamp's volume and EQ, op-amp headroom", "[devices][rhodes]")
{
    // Harp Load: into the Janus' high-impedance input every harmonic stays; into a mixer the top rolls off.
    const auto janus = playNotes({{69, 120, 0.0}}, 0.5);
    const auto mixer = playNotes({{69, 120, 0.0}}, 0.5, [](auto& r) { setParameter(r, "load", 2.0f); });
    CHECK(energyAboveDb(janus.left, 3000.0, 0.0, 0.5) > energyAboveDb(mixer.left, 3000.0, 0.0, 0.5) + 8.0);

    // Volume: an audio taper; 0 is silence.
    CHECK(peakOf(playNotes({{57, 100, 0.0}}, 0.3, [](auto& r) { setParameter(r, "volume", 0.0f); }).left) == 0.0);
    const double seven = rmsOf(playNotes({{57, 80, 0.0}}, 0.4).left, at(0.1), at(0.4));
    const double ten = rmsOf(playNotes({{57, 80, 0.0}}, 0.4, [](auto& r) { setParameter(r, "volume", 10.0f); }).left, at(0.1), at(0.4));
    CHECK_THAT(toDb(ten / seven), WithinAbs(20.0 * 2.2 * std::log10(10.0 / 7.0), 0.5));
    // About 0 dBu (-18 dBFS) for a mezzo-forte note at Volume 7.
    CHECK_THAT(toDb(amplitudeAt(playNotes({{57, 80, 0.0}}, 0.5).left, RhodesSuitcase::frequencyOf(57), at(0.1), at(0.3))), WithinAbs(-18.0, 4.0));

    // Bass +-15 dB (a shelf near 75 Hz), Treble +-12 dB (a shelf at 10 kHz).
    const auto lowNote = [](float bass)
    {
        const auto out = playNotes({{28, 60, 0.0}}, 0.5, [&](auto& r) { setParameter(r, "bass", bass); });
        return amplitudeAt(out.left, RhodesSuitcase::frequencyOf(28), at(0.1), at(0.4));
    };
    CHECK(toDb(lowNote(15.0f) / lowNote(0.0f)) > 10.0);
    CHECK(toDb(lowNote(-15.0f) / lowNote(0.0f)) < -10.0);
    const auto air = [](float treble)
    {
        const auto out = playNotes({{100, 110, 0.0}}, 0.4, [&](auto& r) { setParameter(r, "treble", treble); });
        return energyAboveDb(out.left, 9000.0, 0.0, 0.4);
    };
    CHECK(air(12.0f) > air(0.0f) + 5.0);
    CHECK(air(-12.0f) < air(0.0f) - 5.0);

    // The op-amps' headroom (about +21 dBu): a huge chord at full volume with the bass up rounds off there.
    std::vector<Note> chord;
    for (const int note : {28, 33, 40, 45, 52, 57, 64, 69})
        chord.push_back({note, 127, 0.0});
    const auto loud = playNotes(chord, 0.5, [](auto& r) { setParameter(r, "volume", 10.0f); setParameter(r, "bass", 15.0f); });
    CHECK(peakOf(loud.left) <= 0.1 * level(21.0) * SuitcaseStage::kUnitsToDbfs * 1.001);
    CHECK(peakOf(loud.left) > 0.1 * level(15.0) * SuitcaseStage::kUnitsToDbfs);
}

TEST_CASE("Rhodes: the vibrato swings between left and right, by optocoupler or JFET", "[devices][rhodes]")
{
    // Off: the two sides are one.
    {
        const auto out = playNotes({{57, 90, 0.0}}, 0.5);
        CHECK(out.left == out.right);
    }
    CHECK_THAT(SuitcaseStage::rateHz(0.0), WithinAbs(1.0, 1e-9));
    CHECK_THAT(SuitcaseStage::rateHz(10.0), WithinAbs(10.0, 1e-9));
    CHECK(SuitcaseStage::speedText(10.0f) == "10.00 Hz");
    CHECK_THAT(SuitcaseStage::speedFromText("5 Hz"), WithinAbs(10.0 * std::log10(5.0), 1e-4));

    /** The left / right level difference (dB) in 10 ms windows over a held note. */
    const auto swing = [](float modulator, float speed)
    {
        const auto out = playNotes({{57, 90, 0.0}}, 2.2,
                                   [&](auto& r)
                                   {
                                       setParameter(r, "intensity", 10.0f);
                                       setParameter(r, "modulator", modulator);
                                       setParameter(r, "speed", speed);
                                       setParameter(r, "decay", 400.0f);
                                   });
        std::vector<double> difference;
        for (double t = 0.2; t < 2.2; t += 0.01)
            difference.push_back(toDb(rmsOf(out.left, at(t), at(t + 0.01)) / rmsOf(out.right, at(t), at(t + 0.01))));
        return difference;
    };
    for (const float modulator : {0.0f, 1.0f})
    {
        INFO("modulator " << modulator);
        const auto d = swing(modulator, 10.0f * std::log10(3.0f)); // 3 Hz
        const auto [low, high] = std::minmax_element(d.begin(), d.end());
        CHECK(*high > 10.0);
        CHECK(*low < -10.0); // (180 degrees apart: each side has its turn)
        // About 3 swings a second: count the crossings of 0 dB.
        int crossings = 0;
        for (std::size_t i = 1; i < d.size(); ++i)
            crossings += (d[i - 1] < 0.0) != (d[i] < 0.0) ? 1 : 0;
        CHECK(crossings >= 10);
        CHECK(crossings <= 14);
    }
    // The photoresistor cannot follow a fast LFO fully: at 10 Hz the opto swing is narrower than the JFET's.
    const auto range = [&](float modulator)
    {
        const auto d = swing(modulator, 10.0f);
        const auto [low, high] = std::minmax_element(d.begin(), d.end());
        return *high - *low;
    };
    CHECK(range(0.0f) < range(1.0f) - 3.0);
}

TEST_CASE("Rhodes: power amps (40 W, under 0.5 % THD, then the rails), speakers and line out", "[devices][rhodes]")
{
    // A tone into the suitcase's loop return (as an effect in the accessory loop would send it):
    // the vibrato (off here), the power amps and the speakers, or the line out.
    const auto base = [](double levelDb, double frequency, float output, float cabinet, double seconds = 0.6)
    {
        auto [node, rhodes] = makeNode<RhodesSuitcase>();
        setParameter(*rhodes, "intensity", 0.0f);
        setParameter(*rhodes, "out", output);
        setParameter(*rhodes, "cabinet", cabinet);
        const auto input = tone(frequency, at(seconds), level(levelDb));
        return runSplit(*node, input.left.size(), {},
                        [&input](juce::AudioBuffer<double>& buffer, std::size_t offset, int n)
                        {
                            for (int i = 0; i < n; ++i)
                            {
                                buffer.setSample(0, i, input.left[offset + static_cast<std::size_t>(i)]);
                                buffer.setSample(1, i, input.right[offset + static_cast<std::size_t>(i)]);
                            }
                        });
    };
    const auto thd = [](const Stereo& out, double f)
    {
        const auto n = out.left.size();
        double harmonics = 0.0;
        for (int k = 2; k <= 7; ++k)
            harmonics += std::pow(amplitudeAt(out.left, f * k, n / 2, n), 2.0);
        return 100.0 * std::sqrt(harmonics) / amplitudeAt(out.left, f, n / 2, n);
    };
    // 0 dBu (-18 dBFS) drives the amps to their rated 40 W: under 0.5 % THD.
    CHECK(thd(base(-18.0, 220.0, 0.0f, 0.0f), 220.0) < 0.5);
    // Past the rails it clips.
    CHECK(thd(base(-10.0, 220.0, 0.0f, 0.0f), 220.0) > 3.0);
    // Line out: the umbilical's 0 dBu, no amps: clean and exact in level.
    const auto line = base(-6.0, 220.0, 1.0f, 0.0f);
    CHECK(thd(line, 220.0) < 0.01);
    CHECK_THAT(toDb(amplitudeAt(line.left, 220.0, at(0.3), at(0.6))), WithinAbs(-6.0, 0.05));
    // The rails sag on a long loud passage.
    {
        const auto out = base(-14.0, 110.0, 0.0f, 0.0f, 2.0);
        CHECK(toDb(rmsOf(out.left, at(1.5), at(2.0)) / rmsOf(out.left, at(0.01), at(0.06))) < -0.2);
    }
    // The speakers: about 30 Hz - 15 kHz; the 12-inch pair is darker than the four 10-inch ones.
    const auto gain = [&](double f, float cabinet)
    {
        const auto out = base(-30.0, f, 0.0f, cabinet);
        return toDb(amplitudeAt(out.left, f, at(0.3), at(0.6)) / level(-30.0));
    };
    CHECK(gain(30.0, 0.0f) < gain(1000.0, 0.0f) - 2.0);
    CHECK(gain(30.0, 0.0f) > gain(1000.0, 0.0f) - 12.0);
    CHECK(gain(15000.0, 0.0f) < gain(1000.0, 0.0f) - 2.0);
    CHECK(gain(15000.0, 0.0f) > gain(1000.0, 0.0f) - 15.0);
    CHECK(gain(10000.0, 1.0f) < gain(10000.0, 0.0f) - 3.0);
}

TEST_CASE("Rhodes: one device, its accessory loop inside it (send after the EQ, return into the vibrato)", "[devices][rhodes]")
{
    const std::vector<TimedMidi> events{{0, juce::MidiMessage::noteOn(1, 57, static_cast<juce::uint8>(100))},
                                        {0, juce::MidiMessage::noteOn(1, 64, static_cast<juce::uint8>(90))}};
    const auto setUp = [](devices::BuiltinProcessor& p)
    {
        setParameter(p, "intensity", 8.0f);
        setParameter(p, "speed", 5.0f);
        setParameter(p, "cabinet", 1.0f);
        setParameter(p, "treble", 6.0f);
    };
    // Whole: the Rhodes in one piece (nothing in its loop).
    auto [whole, rhodes] = makeNode<RhodesSuitcase>();
    setUp(*rhodes);
    CHECK(whole->hasInsertLoop());
    CHECK(whole->isLoopEngaged()); // (the switch is on by default: an empty loop is normalled)
    REQUIRE(whole->getInsertLoop() != nullptr);
    CHECK(whole->getInsertLoop()->getLoopName() == "Accessory Loop");
    CHECK(whole->getInsertLoop()->getReturnDescription().contains("speakers"));
    const auto reference = run(*whole, Stereo(at(1.0)), nullptr, events);

    // Split as the engine runs it around the loop's devices: the send is mono at 0 dBu = -18 dBFS
    // (after the EQ: the treble boost is in it), and with nothing changing it, the same sound.
    auto [split, looped] = makeNode<RhodesSuitcase>();
    setUp(*looped);
    double sendPeak = 0.0;
    bool sendMono = true;
    const auto out = runSplit(*split, at(1.0), events,
                              [&](juce::AudioBuffer<double>& buffer, std::size_t, int n)
                              {
                                  for (int i = 0; i < n; ++i)
                                  {
                                      sendMono = sendMono && buffer.getSample(0, i) == buffer.getSample(1, i);
                                      sendPeak = std::max(sendPeak, std::abs(buffer.getSample(0, i)));
                                  }
                              });
    CHECK(sendMono);
    CHECK(sendPeak > level(-30.0));
    CHECK(sendPeak < level(0.0));
    double error = 0.0;
    for (std::size_t i = 0; i < out.left.size(); ++i)
        error = std::max({error, std::abs(out.left[i] - reference.left[i]), std::abs(out.right[i] - reference.right[i])});
    CHECK(error < 1e-9);

    // An effect in the loop (here: 6 dB down) changes what the suitcase's amps get. On the line out,
    // without vibrato, that is exactly 6 dB less.
    const auto lineOut = [&](double loopGain)
    {
        auto [node, r] = makeNode<RhodesSuitcase>();
        setParameter(*r, "intensity", 0.0f);
        setParameter(*r, "out", 1.0f);
        return runSplit(*node, at(0.5), events,
                        [loopGain](juce::AudioBuffer<double>& buffer, std::size_t, int n) { buffer.applyGain(0, n, loopGain); });
    };
    const auto direct = lineOut(1.0);
    const auto padded = lineOut(level(-6.0));
    CHECK_THAT(toDb(rmsOf(padded.left, at(0.1), at(0.5)) / rmsOf(direct.left, at(0.1), at(0.5))), WithinAbs(-6.0, 0.01));

    // The Accessory Loop switch takes the loop out (the engine then runs the Rhodes in one piece).
    setParameter(*looped, "loop", 0.0f);
    CHECK_FALSE(split->isLoopEngaged());
    CHECK(split->hasInsertLoop());
}
