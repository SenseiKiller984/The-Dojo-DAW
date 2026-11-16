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

    A loop can be in series (the Rhodes' accessory loop: everything passes through it) or in
    parallel (a console channel's echo send: the loop's return is mixed with the channel's own
    signal). A parallel host delays its own signal by the loop's latency, which processReturn()
    is given, so the two meet in time; the engine counts that latency in the host's.
*/
class InsertLoopHost
{
public:
    virtual ~InsertLoopHost() = default;

    /** Whether the loop is in the signal path (when not, its devices are skipped). */
    [[nodiscard]] virtual bool isLoopEngaged() const noexcept = 0;
    /** Whether the device offers its loop at all in its current mode (a console channel's echo
        send, but not its monitor section). The Devices panel shows the loop while it is offered
        (or holds devices). */
    [[nodiscard]] virtual bool isLoopAvailable() const noexcept { return true; }
    /** In series (an empty loop passes the signal straight through: normalled) or in parallel (an
        empty loop returns nothing). */
    [[nodiscard]] virtual bool isLoopInSeries() const noexcept { return true; }
    /** Runs up to the loop's send; afterwards `buffer` holds the send. */
    virtual void processSend(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept = 0;
    /** Runs from the loop's return (`buffer`: what came back from the loop) to the output, in place.
        `loopLatency`: the samples the loop's devices delayed the send by. */
    virtual void processReturn(juce::AudioBuffer<double>& buffer, int loopLatency) noexcept = 0;

    /** The loop's name as the device's panel labels it ("Accessory Loop"). */
    [[nodiscard]] virtual juce::String getLoopName() const { return "Insert Loop"; }
    /** What the return feeds, for display ("vibrato, power amps, speakers"); may be empty. */
    [[nodiscard]] virtual juce::String getReturnDescription() const { return {}; }
};

} // namespace dojo
