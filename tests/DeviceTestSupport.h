#pragma once

// Helpers for testing native devices: run a device (through engine::ProcessorNode, exactly as the
// engine does) over a stereo signal with optional sidechain and MIDI.

#include "devices/BuiltinProcessor.h"
#include "engine/ProcessorNode.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <utility>
#include <vector>

namespace dojo::test::devices
{
using dojo::devices::BuiltinProcessor;
using dojo::engine::ProcessorNode;

inline constexpr double kRate = 48'000.0;
inline constexpr int kBlock = 256;

struct Stereo
{
    explicit Stereo(std::size_t n = 0)
        : left(n, 0.0)
        , right(n, 0.0)
    {
    }
    std::vector<double> left, right;
};

inline Stereo fromMono(const std::vector<float>& mono)
{
    Stereo s(mono.size());
    for (std::size_t i = 0; i < mono.size(); ++i)
        s.left[i] = s.right[i] = mono[i];
    return s;
}

struct TimedMidi
{
    int sample;
    juce::MidiMessage message;
};

/** Runs `node` over `input` in blocks, with optional sidechain and MIDI. */
inline Stereo run(ProcessorNode& node, const Stereo& input, const Stereo* sidechain = nullptr, std::vector<TimedMidi> midiEvents = {})
{
    const auto total = input.left.size();
    Stereo out(total);
    juce::AudioBuffer<double> buffer(2, kBlock);
    juce::AudioBuffer<double> side(2, kBlock);
    juce::MidiBuffer midi;

    for (std::size_t offset = 0; offset < total; offset += kBlock)
    {
        const int n = static_cast<int>(std::min<std::size_t>(kBlock, total - offset));
        for (int i = 0; i < n; ++i)
        {
            buffer.setSample(0, i, input.left[offset + static_cast<std::size_t>(i)]);
            buffer.setSample(1, i, input.right[offset + static_cast<std::size_t>(i)]);
            if (sidechain != nullptr)
            {
                side.setSample(0, i, sidechain->left[offset + static_cast<std::size_t>(i)]);
                side.setSample(1, i, sidechain->right[offset + static_cast<std::size_t>(i)]);
            }
        }

        midi.clear();
        for (const auto& e : midiEvents)
            if (e.sample >= static_cast<int>(offset) && e.sample < static_cast<int>(offset) + n)
                midi.addEvent(e.message, e.sample - static_cast<int>(offset));

        node.process(buffer, n, midi, sidechain != nullptr ? &side : nullptr);

        for (int i = 0; i < n; ++i)
        {
            out.left[offset + static_cast<std::size_t>(i)] = buffer.getSample(0, i);
            out.right[offset + static_cast<std::size_t>(i)] = buffer.getSample(1, i);
        }
    }
    return out;
}

inline void setParameter(BuiltinProcessor& processor, const juce::String& id, float value)
{
    auto* parameter = processor.getParameterById(id);
    REQUIRE(parameter != nullptr);
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

template <typename T>
std::pair<std::unique_ptr<ProcessorNode>, T*> makeNode()
{
    auto processor = std::make_unique<T>();
    auto* raw = processor.get();
    auto node = std::make_unique<ProcessorNode>(std::move(processor));
    node->prepare(kRate, kBlock, nullptr);
    return {std::move(node), raw};
}
} // namespace dojo::test::devices
