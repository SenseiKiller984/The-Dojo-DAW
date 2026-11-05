#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace dojo::test
{

/** A scratch folder that is deleted when the test ends. */
struct TempFolder
{
    TempFolder()
        : dir(juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("dojo-tests", "", false))
    {
        dir.createDirectory();
    }
    ~TempFolder() { dir.deleteRecursively(); }

    juce::File dir;
};

inline constexpr double kPi = 3.14159265358979323846;

inline std::vector<float> sine(double frequency, double sampleRate, int numSamples, float amplitude = 0.5f)
{
    std::vector<float> data(static_cast<std::size_t>(numSamples));
    for (int i = 0; i < numSamples; ++i)
        data[static_cast<std::size_t>(i)] = amplitude * static_cast<float>(std::sin(2.0 * kPi * frequency * i / sampleRate));
    return data;
}

/** Writes mono (or duplicated stereo) float samples as a 32-bit float WAV. */
inline juce::File writeWav(const juce::File& file, const std::vector<float>& samples, double sampleRate, int numChannels = 1)
{
    juce::AudioBuffer<float> buffer(numChannels, static_cast<int>(samples.size()));
    for (int ch = 0; ch < numChannels; ++ch)
        std::copy(samples.begin(), samples.end(), buffer.getWritePointer(ch));

    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                  .withSampleRate(sampleRate)
                                                  .withNumChannels(numChannels)
                                                  .withBitsPerSample(32)
                                                  .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    if (writer != nullptr)
        writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
    return file;
}

/** A WAV with a single impulse of `amplitude` at `frame`. */
inline juce::File writeImpulse(const juce::File& file, int frame, int length, double sampleRate, float amplitude = 0.5f)
{
    std::vector<float> data(static_cast<std::size_t>(length), 0.0f);
    data[static_cast<std::size_t>(frame)] = amplitude;
    return writeWav(file, data, sampleRate);
}

template <typename T>
double rms(const std::vector<T>& data, std::size_t begin, std::size_t end)
{
    end = std::min(end, data.size());
    if (end <= begin)
        return 0.0;
    double sum = 0.0;
    for (std::size_t i = begin; i < end; ++i)
        sum += static_cast<double>(data[i]) * static_cast<double>(data[i]);
    return std::sqrt(sum / static_cast<double>(end - begin));
}

template <typename T>
double peak(const std::vector<T>& data, std::size_t begin = 0, std::size_t end = static_cast<std::size_t>(-1))
{
    end = std::min(end, data.size());
    double result = 0.0;
    for (std::size_t i = begin; i < end; ++i)
        result = std::max(result, std::abs(static_cast<double>(data[i])));
    return result;
}

template <typename T>
std::size_t indexOfPeak(const std::vector<T>& data)
{
    std::size_t best = 0;
    for (std::size_t i = 0; i < data.size(); ++i)
        if (std::abs(static_cast<double>(data[i])) > std::abs(static_cast<double>(data[best])))
            best = i;
    return best;
}

/** Frequency estimate from rising zero crossings. */
template <typename T>
double estimateFrequency(const std::vector<T>& data, std::size_t begin, std::size_t end, double sampleRate)
{
    end = std::min(end, data.size());
    int crossings = 0;
    std::size_t first = 0;
    std::size_t last = 0;
    for (std::size_t i = begin + 1; i < end; ++i)
    {
        if (data[i - 1] < 0 && data[i] >= 0)
        {
            if (crossings == 0)
                first = i;
            last = i;
            ++crossings;
        }
    }
    if (crossings < 2)
        return 0.0;
    return (crossings - 1) * sampleRate / static_cast<double>(last - first);
}

} // namespace dojo::test
