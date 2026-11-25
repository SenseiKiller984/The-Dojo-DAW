#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>

namespace dojo::engine
{

/**
    The license's say over what the sound card plays (licensing/Features.h), applied by the device
    callback after the engine, to every output channel the device plays: the mix, direct outputs,
    HW Insert sends and renderer outputs alike, so a loopback or a virtual cable gets the same.

        mute    silence (no license; the free trial while recording software runs or the output is a
                virtual audio device)
        clean   as the engine made it

    Starts muted. setPolicy() from any thread; process() on the audio thread (real-time safe).
*/
class OutputGuard
{
public:
    enum class Policy
    {
        mute,
        clean,
    };

    void setPolicy(Policy policy) noexcept { policy_.store(static_cast<int>(policy), std::memory_order_relaxed); }
    [[nodiscard]] Policy getPolicy() const noexcept { return static_cast<Policy>(policy_.load(std::memory_order_relaxed)); }

    void process(float* const* out, int numChannels, int numSamples) noexcept
    {
        if (getPolicy() == Policy::clean)
            return;
        for (int c = 0; c < numChannels; ++c)
            if (out[c] != nullptr)
                juce::FloatVectorOperations::clear(out[c], numSamples);
    }

private:
    std::atomic<int> policy_{static_cast<int>(Policy::mute)};
};

} // namespace dojo::engine
