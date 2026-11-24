#include "session/DrumKitDemo.h"

#include "devices/Instruments.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace dojo::session::presets
{
namespace
{
constexpr double kRingOutSeconds = 1.2;
constexpr double kFadeSeconds = 0.3;
constexpr float kCeiling = 0.891f; // -1 dBFS
constexpr int kBlock = 512;

struct Hit
{
    juce::int64 at = 0;
    int pad = 0;
    float velocity = 0.0f;
};
} // namespace

double demoSeconds(const DemoBeat& beat)
{
    return static_cast<double>(beat.bars) * 4.0 * 60.0 / std::max(1.0, beat.tempo);
}

juce::AudioBuffer<float> renderDemo(const Preset& kit, double sampleRate)
{
    const auto* settings = kit.findFor(devices::DrumRack::kTypeId);
    if (!kit.isDrumKit() || settings == nullptr || sampleRate <= 0.0)
        return {};

    devices::DrumRack rack;
    restore(*settings, rack);
    bool anySample = false;
    for (int pad = 0; pad < devices::DrumRack::kPads; ++pad)
        anySample = anySample || rack.getPadSample(pad) != nullptr;
    if (!anySample)
        return {};

    // The hits, in time: sixteenths, every second one late by the swing.
    const auto beat = kit.demo.isEmpty() ? DemoBeat::standard() : kit.demo;
    const double sixteenth = 60.0 / std::max(1.0, beat.tempo) / 4.0;
    const int steps = beat.bars * 16;
    std::vector<Hit> hits;
    for (const auto& lane : beat.lanes)
        for (int step = 0; step < std::min(steps, lane.steps.length()); ++step)
            if (const float velocity = DemoBeat::velocityOf(lane.steps[step]); velocity > 0.0f)
            {
                const double seconds = (static_cast<double>(step) + (step % 2 == 1 ? beat.swing : 0.0)) * sixteenth;
                hits.push_back({static_cast<juce::int64>(std::llround(seconds * sampleRate)), lane.pad, velocity});
            }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.at < b.at; });

    const auto total = static_cast<int>(std::ceil((demoSeconds(beat) + kRingOutSeconds) * sampleRate));
    juce::AudioBuffer<float> out(2, total);
    out.clear();

    const int channels = std::max(2, rack.getTotalNumOutputChannels());
    rack.setRateAndBufferSizeDetails(sampleRate, kBlock);
    rack.prepareToPlay(sampleRate, kBlock);
    juce::AudioBuffer<float> block(channels, kBlock);
    juce::MidiBuffer midi;
    std::size_t next = 0;
    for (int done = 0; done < total; done += kBlock)
    {
        const int n = std::min(kBlock, total - done);
        midi.clear();
        while (next < hits.size() && hits[next].at < done + n)
        {
            const auto& hit = hits[next++];
            midi.addEvent(juce::MidiMessage::noteOn(1, devices::DrumRack::kFirstNote + hit.pad, hit.velocity),
                          static_cast<int>(std::max<juce::int64>(0, hit.at - done)));
        }
        block.setSize(channels, n, false, false, true);
        block.clear();
        rack.processBlock(block, midi);
        for (int c = 0; c < 2; ++c)
            out.copyFrom(c, done, block, c, 0, n);
    }
    rack.releaseResources();

    // The ring-out faded, the peak under the ceiling.
    const int fade = std::min(total, static_cast<int>(kFadeSeconds * sampleRate));
    for (int c = 0; c < 2; ++c)
        out.applyGainRamp(c, total - fade, fade, 1.0f, 0.0f);
    if (const float peak = out.getMagnitude(0, total); peak > kCeiling)
        out.applyGain(kCeiling / peak);
    return out;
}

} // namespace dojo::session::presets
