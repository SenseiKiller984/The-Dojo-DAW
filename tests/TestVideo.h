#pragma once

#include "video/MjpegAvi.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_graphics/juce_graphics.h>

#include <vector>

namespace dojo::test
{

/** Frame k of the test videos: its number in binary as five black or white stripes (left = bit 0),
    which survive any decoder's colour conversion. */
inline constexpr int kStripeWidth = 12;
inline juce::Image numberedFrame(int k, int width = 64, int height = 48)
{
    juce::Image image(juce::Image::RGB, width, height, false);
    juce::Graphics g(image);
    g.fillAll(juce::Colour(0xff406080));
    for (int bit = 0; bit < 5; ++bit)
    {
        g.setColour((k >> bit) & 1 ? juce::Colours::white : juce::Colours::black);
        g.fillRect(bit * kStripeWidth, 0, kStripeWidth, height);
    }
    return image;
}

/** Which frame an image is (from its stripes), or -1. */
inline int frameNumberOf(const juce::Image& image)
{
    if (!image.isValid())
        return -1;
    const float scale = static_cast<float>(image.getWidth()) / 64.0f; // (thumbnails are smaller)
    int number = 0;
    for (int bit = 0; bit < 5; ++bit)
    {
        const auto x = static_cast<int>((static_cast<float>(bit * kStripeWidth) + kStripeWidth / 2.0f) * scale);
        if (image.getPixelAt(x, image.getHeight() / 2).getBrightness() > 0.5f)
            number |= 1 << bit;
    }
    return number;
}

/** A Motion-JPEG AVI of numbered frames. */
inline juce::File writeTestVideo(const juce::File& file, int frames = 10, double frameRate = 10.0)
{
    std::vector<juce::Image> images;
    for (int k = 0; k < frames; ++k)
        images.push_back(numberedFrame(k));
    REQUIRE(video::writeMjpegAvi(file, images, frameRate, 0.95f));
    return file;
}

} // namespace dojo::test
