#pragma once

#include "core/Types.h"

namespace dojo
{

/**
    The sound card's channels during one stretch of the audio callback (a sub-block), as the engine
    hands them to a device that talks to them directly (see HardwareIoDevice).

    `inputs` are the sub-block's input samples (64-bit, from the start of the sub-block), one
    pointer per active input channel in the engine's order; outputs are reached through
    HardwareIoDevice::addSends(). While the engine renders offline there is no sound card:
    `live` is false and there are no inputs.
*/
struct HardwarePorts
{
    const Sample* const* inputs = nullptr;
    int numInputs = 0;
    int numOutputs = 0;
    int bufferSize = 0;      ///< the audio device's buffer, in samples
    int reportedLatency = 0; ///< the driver's input + output latency in samples (0: not reported)
    bool live = false;
};

/**
    A device that sends audio out of the computer through the sound card's outputs, and takes it
    back in through its inputs: a hardware insert (outboard gear patched in like a plug-in).

    Before each sub-block the graph runs, the engine gives every such device in the graph the
    sound card's channels (setHardwarePorts); the device reads its return from them while it
    processes, and keeps what it sends. After the graph ran, the engine has the device add what it
    sent to the sound card's outputs (addSends), on top of the main mix. Both run on the audio
    thread and must be real-time safe.

    service() is the device's message-thread housekeeping, about ten times a second (from the
    session's device manager): e.g. analysing a latency measurement and reporting the new latency.
*/
class HardwareIoDevice
{
public:
    virtual ~HardwareIoDevice() = default;

    virtual void setHardwarePorts(const HardwarePorts& ports) noexcept = 0;
    /** Adds what the device sent during the last sub-block it processed to `outputs`, from
        `offset`, and forgets it (a sub-block it did not process sends nothing). */
    virtual void addSends(float* const* outputs, int numOutputs, int offset, int numSamples) noexcept = 0;

    virtual void service() {}
};

} // namespace dojo
