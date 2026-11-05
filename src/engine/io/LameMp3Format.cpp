#include "engine/io/LameMp3Format.h"

#include <lame.h>

#include <vector>

namespace dojo::engine
{
namespace
{
class LameWriter final : public juce::AudioFormatWriter
{
public:
    LameWriter(juce::OutputStream* stream, double rate, int channels, int kbps, lame_t lame)
        : juce::AudioFormatWriter(stream, "MP3 (LAME)", rate, static_cast<unsigned int>(channels), 16)
        , lame_(lame)
    {
        usesFloatingPointData = true;
        startPosition_ = stream->getPosition();
        juce::ignoreUnused(kbps);
    }

    ~LameWriter() override
    {
        flushEncoder();

        // Replace the placeholder first frame with the LAME/Xing tag (duration, gapless info).
        std::vector<unsigned char> tag(4096);
        const auto tagSize = lame_get_lametag_frame(lame_, tag.data(), tag.size());
        if (tagSize > 0 && tagSize <= tag.size() && output != nullptr)
        {
            const auto end = output->getPosition();
            if (output->setPosition(startPosition_))
            {
                output->write(tag.data(), tagSize);
                output->setPosition(end);
            }
        }

        if (output != nullptr)
            output->flush();
        lame_close(lame_);
    }

    bool write(const int** samplesToWrite, int numSamples) override
    {
        if (numSamples <= 0)
            return true;

        // usesFloatingPointData: the "int" pointers are really float data.
        const auto* left = reinterpret_cast<const float*>(samplesToWrite[0]);
        const auto* right = numChannels > 1 && samplesToWrite[1] != nullptr ? reinterpret_cast<const float*>(samplesToWrite[1]) : left;

        buffer_.resize(static_cast<std::size_t>(numSamples) * 5 / 4 + 7200);
        const int bytes = lame_encode_buffer_ieee_float(lame_, left, right, numSamples, buffer_.data(),
                                                        static_cast<int>(buffer_.size()));
        if (bytes < 0)
            return false;
        return bytes == 0 || output->write(buffer_.data(), static_cast<std::size_t>(bytes));
    }

private:
    void flushEncoder()
    {
        if (flushed_)
            return;
        flushed_ = true;
        buffer_.resize(7200 * 2);
        const int bytes = lame_encode_flush(lame_, buffer_.data(), static_cast<int>(buffer_.size()));
        if (bytes > 0 && output != nullptr)
            output->write(buffer_.data(), static_cast<std::size_t>(bytes));
    }

    lame_t lame_;
    std::vector<unsigned char> buffer_;
    juce::int64 startPosition_ = 0;
    bool flushed_ = false;
};
} // namespace

LameMp3Format::LameMp3Format()
    : juce::AudioFormat("MP3 (LAME)", ".mp3")
{
}

juce::Array<int> LameMp3Format::getPossibleSampleRates()
{
    return {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
}

const juce::Array<int>& LameMp3Format::getBitRates()
{
    static const juce::Array<int> rates{128, 160, 192, 224, 256, 320};
    return rates;
}

juce::StringArray LameMp3Format::getQualityOptions()
{
    juce::StringArray options;
    for (const int rate : getBitRates())
        options.add(juce::String(rate) + " kbps");
    return options;
}

std::unique_ptr<juce::AudioFormatWriter> LameMp3Format::createWriterFor(std::unique_ptr<juce::OutputStream>& streamToWriteTo,
                                                                        const juce::AudioFormatWriterOptions& options)
{
    if (streamToWriteTo == nullptr)
        return nullptr;

    const int numChannels = options.getNumChannels();
    const auto sampleRate = static_cast<int>(options.getSampleRate());
    if (numChannels < 1 || numChannels > 2 || !getPossibleSampleRates().contains(sampleRate))
        return nullptr;

    const auto& rates = getBitRates();
    const int kbps = rates[juce::jlimit(0, rates.size() - 1, options.getQualityOptionIndex() < 0 ? 4 : options.getQualityOptionIndex())];

    lame_t lame = lame_init();
    if (lame == nullptr)
        return nullptr;

    lame_set_in_samplerate(lame, sampleRate);
    lame_set_out_samplerate(lame, sampleRate);
    lame_set_num_channels(lame, numChannels);
    lame_set_mode(lame, numChannels > 1 ? JOINT_STEREO : MONO);
    lame_set_brate(lame, kbps);
    lame_set_VBR(lame, vbr_off);
    lame_set_quality(lame, 2); // high quality, still fast
    lame_set_bWriteVbrTag(lame, 1);

    if (lame_init_params(lame) < 0)
    {
        lame_close(lame);
        return nullptr;
    }

    auto writer = std::make_unique<LameWriter>(streamToWriteTo.release(), options.getSampleRate(), numChannels, kbps, lame);
    return writer;
}

} // namespace dojo::engine
