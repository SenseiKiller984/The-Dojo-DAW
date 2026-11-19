#pragma once

namespace dojo
{

/**
    A device that moves other parameters (the LFO device). The engine asks it for its value once per
    block, before any track renders, and applies that value to the parameters, volumes, pans and send
    levels mapped to it. Implemented by devices, used by the engine; neither knows the other.
*/
class ModulationSource
{
public:
    virtual ~ModulationSource() = default;

    /** Audio thread: advances by `numSamples` and returns the output, 0..1 (where each target's
        range begins and ends). `ppq` and `playing` are the transport at the start of the block. */
    virtual double advanceModulation(int numSamples, double sampleRate, double bpm, double ppq, bool playing) noexcept = 0;
};

} // namespace dojo
