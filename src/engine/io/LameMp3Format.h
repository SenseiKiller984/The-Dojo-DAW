#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

namespace dojo::engine
{

/**
    MP3 export through the LAME encoder (LGPL, built from source in third_party/lame).

    Write-only (JUCE's own decoder reads MP3s). Constant bit rate chosen with the quality option
    index (see getQualityOptions()); the LAME/Xing tag is written at the start of the file when the
    writer is closed, so players show the correct duration.
*/
class LameMp3Format final : public juce::AudioFormat
{
public:
    LameMp3Format();

    juce::Array<int> getPossibleSampleRates() override;
    juce::Array<int> getPossibleBitDepths() override { return {16}; }
    bool canDoStereo() override { return true; }
    bool canDoMono() override { return true; }
    bool isCompressed() override { return true; }
    juce::StringArray getQualityOptions() override;

    /** Bit rates (kbps) matching getQualityOptions(). */
    static const juce::Array<int>& getBitRates();

    juce::AudioFormatReader* createReaderFor(juce::InputStream*, bool) override { return nullptr; }

    std::unique_ptr<juce::AudioFormatWriter> createWriterFor(std::unique_ptr<juce::OutputStream>& streamToWriteTo,
                                                             const juce::AudioFormatWriterOptions& options) override;
};

} // namespace dojo::engine
