#include "ui/DemoProject.h"

#include "devices/Dynamics.h"
#include "devices/Instruments.h"
#include "video/VideoSource.h"

#include <cmath>

namespace dojo::ui
{
namespace
{
constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

bool writeMono(const juce::File& file, const std::vector<float>& samples)
{
    juce::AudioBuffer<float> buffer(1, static_cast<int>(samples.size()));
    std::copy(samples.begin(), samples.end(), buffer.getWritePointer(0));
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(kRate).withNumChannels(1).withBitsPerSample(24));
    return writer != nullptr && writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

std::vector<float> kick()
{
    std::vector<float> s(static_cast<std::size_t>(0.45 * kRate));
    double phase = 0.0;
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        const double t = static_cast<double>(i) / kRate;
        const double frequency = 45.0 + 110.0 * std::exp(-t * 28.0);
        phase += 2.0 * kPi * frequency / kRate;
        s[i] = static_cast<float>(0.9 * std::sin(phase) * std::exp(-t * 7.0));
    }
    return s;
}

std::vector<float> noiseHit(double seconds, double decay, double tone, double toneFrequency, std::uint32_t seed)
{
    std::vector<float> s(static_cast<std::size_t>(seconds * kRate));
    juce::Random random(seed);
    double lowpass = 0.0;
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        const double t = static_cast<double>(i) / kRate;
        const double noise = random.nextDouble() * 2.0 - 1.0;
        lowpass += 0.5 * (noise - lowpass);
        const double body = tone * std::sin(2.0 * kPi * toneFrequency * t) * std::exp(-t * 18.0);
        s[i] = static_cast<float>((0.6 * (noise - lowpass) + body) * std::exp(-t * decay));
    }
    return s;
}

std::vector<float> pad()
{
    // A soft saw-ish tone at middle C (sum of decaying harmonics), 2 s long.
    std::vector<float> s(static_cast<std::size_t>(2.0 * kRate));
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        const double t = static_cast<double>(i) / kRate;
        double v = 0.0;
        for (int h = 1; h <= 8; ++h)
            v += std::sin(2.0 * kPi * 261.63 * h * t * (1.0 + 0.0008 * h)) / (h * 1.3);
        const double envelope = std::min(1.0, t * 8.0) * std::exp(-t * 0.6);
        s[i] = static_cast<float>(0.22 * v * envelope);
    }
    return s;
}

std::vector<float> bassLoop(double tempo)
{
    // Two bars of eighth notes: A1 A1 A2 A1 G1 G1 G2 E1 ...
    const double eighth = 30.0 / tempo;
    const std::array<double, 16> notes{55.0, 55.0, 110.0, 55.0, 49.0, 49.0, 98.0, 41.2, 55.0, 55.0, 110.0, 55.0, 65.4, 65.4, 61.7, 49.0};
    std::vector<float> s(static_cast<std::size_t>(16 * eighth * kRate));
    double phase = 0.0;
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        const double t = static_cast<double>(i) / kRate;
        const auto step = std::min<std::size_t>(15, static_cast<std::size_t>(t / eighth));
        const double inStep = t - static_cast<double>(step) * eighth;
        phase += 2.0 * kPi * notes[step] / kRate;
        const double saw = 2.0 * (phase / (2.0 * kPi) - std::floor(phase / (2.0 * kPi) + 0.5));
        const double envelope = std::min(1.0, inStep * 300.0) * std::exp(-inStep * 4.0);
        s[i] = static_cast<float>(0.45 * (0.6 * std::sin(phase) + 0.4 * saw) * envelope);
    }
    return s;
}
} // namespace

/** The demo's picture: a title card with a bar/beat counter and a progress bar, `bars` long. */
static bool writeDemoVideo(const juce::File& file, double tempo, int bars)
{
    constexpr double frameRate = 10.0;
    constexpr int width = 320;
    constexpr int height = 180;
    const double beat = 60.0 / tempo;
    const double length = bars * 4.0 * beat;
    std::vector<juce::Image> frames;
    for (int f = 0; f < static_cast<int>(std::ceil(length * frameRate)); ++f)
    {
        const double t = f / frameRate;
        const int beatIndex = static_cast<int>(t / beat);
        juce::Image image(juce::Image::RGB, width, height, true);
        juce::Graphics g(image);
        const float hue = static_cast<float>(std::fmod(0.02 + 0.08 * (beatIndex / 4), 1.0));
        g.setGradientFill(juce::ColourGradient(juce::Colour::fromHSV(hue, 0.55f, 0.32f, 1.0f), 0.0f, 0.0f,
                                               juce::Colour(0xff101014), static_cast<float>(width), static_cast<float>(height), false));
        g.fillAll();
        g.setColour(juce::Colour(0xffe0533d));
        g.fillEllipse(24.0f, 52.0f, 44.0f, 44.0f);
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(30.0f, juce::Font::bold));
        g.drawText("THE DOJO", juce::Rectangle<int>(80, 46, 220, 34), juce::Justification::centredLeft);
        g.setFont(juce::FontOptions(16.0f));
        g.drawText("Bar " + juce::String(beatIndex / 4 + 1) + "   Beat " + juce::String(beatIndex % 4 + 1),
                   juce::Rectangle<int>(80, 82, 220, 22), juce::Justification::centredLeft);
        // The beat flashes; a bar along the bottom shows where we are.
        const double phase = std::fmod(t, beat) / beat;
        g.setColour(juce::Colours::white.withAlpha(static_cast<float>(0.85 * (1.0 - phase))));
        g.fillEllipse(36.0f, 64.0f, 20.0f, 20.0f);
        g.setColour(juce::Colours::white.withAlpha(0.25f));
        g.fillRect(24, 150, width - 48, 4);
        g.setColour(juce::Colour(0xffe0533d));
        g.fillRect(24, 150, static_cast<int>((width - 48) * t / length), 4);
        frames.push_back(image);
    }
    return video::writeMjpegAvi(file, frames, frameRate, 0.85f);
}

juce::Result createDemoProject(session::Project& project, const juce::File& folder)
{
    if (!folder.createDirectory())
        return juce::Result::fail("Could not create " + folder.getFullPathName());

    auto& session = project.getSession();
    constexpr double tempo = 110.0;
    const double beat = 60.0 / tempo;
    const double bar = 4.0 * beat;

    const auto kickFile = folder.getChildFile("Kick.wav");
    const auto snareFile = folder.getChildFile("Snare.wav");
    const auto hatFile = folder.getChildFile("Hat.wav");
    const auto padFile = folder.getChildFile("Pad C4.wav");
    const auto bassFile = folder.getChildFile("Bass Loop.wav");
    if (!writeMono(kickFile, kick()) || !writeMono(snareFile, noiseHit(0.3, 14.0, 0.5, 190.0, 7)) ||
        !writeMono(hatFile, noiseHit(0.12, 45.0, 0.0, 0.0, 11)) || !writeMono(padFile, pad()) || !writeMono(bassFile, bassLoop(tempo)))
        return juce::Result::fail("Could not write the demo samples to " + folder.getFullPathName());

    session.beginTransaction("Demo Project");
    session.setTempo(tempo);
    auto* um = &session.getUndoManager();

    // Rhythm folder: drums + bass.
    auto drums = session.addTrack(session::TrackKind::instrument, "Drums");
    auto rack = session.addDevice(drums, devices::DrumRack::kTypeId, "Drum Rack");
    if (auto node = project.getDevices().getNode(rack))
        if (auto* drumRack = dynamic_cast<devices::DrumRack*>(&node->getProcessor()))
        {
            juce::String error;
            drumRack->loadPadSample(0, kickFile, error);
            drumRack->loadPadSample(1, snareFile, error);
            drumRack->loadPadSample(2, hatFile, error);
            drumRack->getPad(2).volume->setValueNotifyingHost(drumRack->getPad(2).volume->convertTo0to1(-8.0f));
            drumRack->getPad(2).pan->setValueNotifyingHost(drumRack->getPad(2).pan->convertTo0to1(0.3f));
        }

    auto beatClip = session.addMidiClip(drums, 0.0, 8.0 * bar, "Beat");
    for (int b = 0; b < 8; ++b)
    {
        const double barBeat = b * 4.0;
        session.addNote(beatClip, barBeat, 0.5, 36, 1.0f);
        session.addNote(beatClip, barBeat + 2.5, 0.5, 36, 0.8f);
        session.addNote(beatClip, barBeat + 1.0, 0.5, 37, 0.9f);
        session.addNote(beatClip, barBeat + 3.0, 0.5, 37, 0.95f);
        for (int e = 0; e < 8; ++e)
            session.addNote(beatClip, barBeat + e * 0.5, 0.25, 38, e % 2 == 0 ? 0.8f : 0.5f);
    }

    auto bass = session.addTrack(session::TrackKind::audio, "Bass");
    juce::String error;
    const auto bassAsset = project.getAssets().loadNow(bassFile, error);
    const double loopLength = bassAsset != nullptr ? bassAsset->getLengthSeconds() : 2.0 * bar;
    for (int i = 0; i < 4; ++i)
        session.addClip(bass, bassFile, i * loopLength, loopLength);
    session.addDevice(bass, "builtin:eq", "EQ");
    auto compressor = session.addDevice(bass, devices::Compressor::kTypeId, "Compressor");
    session::device::setSidechain(compressor, session::track::getId(drums), um);
    if (auto node = project.getDevices().getNode(compressor))
        if (auto* comp = dynamic_cast<devices::Compressor*>(&node->getProcessor()))
        {
            for (const auto& [id, value] : {std::pair{"sidechain", 1.0f}, {"threshold", -24.0f}, {"ratio", 6.0f}, {"release", 90.0f}})
                if (auto* p = comp->getParameterById(id))
                    p->setValueNotifyingHost(p->convertTo0to1(value));
        }
    session::track::setVolumeDb(bass, -3.0, um);

    auto rhythm = session.groupIntoFolder({drums, bass}, "Rhythm");
    session::track::setColour(rhythm, session::getTrackPalette()[5], um);

    // Keys: Sampler chords with a fade-in.
    auto keys = session.addTrack(session::TrackKind::instrument, "Keys");
    auto sampler = session.addDevice(keys, devices::Sampler::kTypeId, "Sampler");
    if (auto node = project.getDevices().getNode(sampler))
        if (auto* s = dynamic_cast<devices::Sampler*>(&node->getProcessor()))
        {
            s->loadSample(padFile, error);
            s->attack_->setValueNotifyingHost(s->attack_->convertTo0to1(120.0f));
            s->release_->setValueNotifyingHost(s->release_->convertTo0to1(600.0f));
        }
    session.addDevice(keys, "builtin:delay", "Delay");
    auto chords = session.addMidiClip(keys, 0.0, 8.0 * bar, "Chords");
    const std::array<std::array<int, 3>, 4> progression{{{57, 60, 64}, {53, 57, 60}, {55, 59, 62}, {52, 55, 59}}};
    for (int b = 0; b < 8; ++b)
        for (const int n : progression[static_cast<std::size_t>(b % 4)])
            session.addNote(chords, b * 4.0, 3.75, n, 0.7f);
    auto fade = session.getAutomation(keys, session::automation::volumeTarget(), true, 0.0);
    session.addAutomationPoint(fade, 2.0 * bar, engine::gainToAutomation(juce::Decibels::decibelsToGain(-4.0)));
    session::track::setShowingAutomation(keys, true, nullptr);
    session::track::setPan(keys, -0.15, um);

    // Reverb return.
    auto reverb = session.addTrack(session::TrackKind::bus, "Reverb");
    session.addDevice(reverb, "builtin:reverb", "Reverb");
    if (auto node = project.getDevices().findNode(session::device::getId(session::Session::getDevices(reverb).front())))
        if (auto* mix = dynamic_cast<devices::BuiltinProcessor*>(&node->getProcessor())->getParameterById("mix"))
            mix->setValueNotifyingHost(mix->convertTo0to1(100.0f));
    session.addSend(keys, reverb, -8.0);
    session.addSend(drums, reverb, -18.0);

    // Picture: a video track on top (View > Video shows it).
    const auto videoFile = folder.getChildFile("Demo Picture.avi");
    if (writeDemoVideo(videoFile, tempo, 8))
    {
        auto picture = session.addTrack(session::TrackKind::video, "Picture", 0);
        session.addVideoClip(picture, videoFile, 0.0, 8.0 * bar);
        session::track::setColour(picture, session::getTrackPalette()[6], um);
    }

    // Master.
    session.addDevice(session.getMaster(), devices::Limiter::kTypeId, "Limiter");
    session.setLoop(true, 0.0, 8.0 * bar);
    return juce::Result::ok();
}

} // namespace dojo::ui
