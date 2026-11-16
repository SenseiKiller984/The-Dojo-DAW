#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace dojo
{

/**
    A device with an insert loop (an accessory or effects loop): a point inside its signal path where
    other devices are inserted, like the send and return jacks of a hardware unit.

    The engine runs such a device in two halves around the devices of its loop: processSend() up to
    the loop's send (the buffer then holds what the loop receives), the loop's devices in order on
    that buffer, then processReturn() from the loop's return to the device's output. With nothing in
    the loop, or the loop disengaged, the engine runs the device in one piece as usual.

    Both halves are real-time safe and run on the audio thread with stereo, 64-bit buffers of the
    block's length; they are only ever called in pairs (send, then return), never with the device's
    ordinary processBlock() in between.
*/
class InsertLoopHost
{
public:
    virtual ~InsertLoopHost() = default;

    /** Whether the loop is in the signal path (when not, its devices are skipped). */
    [[nodiscard]] virtual bool isLoopEngaged() const noexcept = 0;
    /** Runs up to the loop's send; afterwards `buffer` holds the send. */
    virtual void processSend(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept = 0;
    /** Runs from the loop's return (`buffer`: what came back from the loop) to the output, in place. */
    virtual void processReturn(juce::AudioBuffer<double>& buffer) noexcept = 0;

    /** The loop's name as the device's panel labels it ("Accessory Loop"). */
    [[nodiscard]] virtual juce::String getLoopName() const { return "Insert Loop"; }
    /** What the return feeds, for display ("vibrato, power amps, speakers"); may be empty. */
    [[nodiscard]] virtual juce::String getReturnDescription() const { return {}; }
};

} // namespace dojo
