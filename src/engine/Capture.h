#pragma once

#include "engine/RealtimeHandoff.h"
#include "engine/RenderGraph.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace dojo::engine
{

/** A MIDI message captured while recording, stamped with its timeline time. */
struct CapturedMidiEvent
{
    double seconds = 0.0;
    std::uint8_t data[3]{};
    std::uint8_t size = 0;
};

/**
    What the audio thread should capture while a recording or real-time bounce runs.

    Built on the message thread, handed over with command::StartCapture, released after
    command::StopCapture has been acknowledged (TelemetrySnapshot::captureActive == false). The
    audio thread only writes into the pre-allocated streams/arrays.
*/
struct CaptureSession
{
    struct InputTarget
    {
        int firstChannel = 0;
        int numChannels = 2;
        std::shared_ptr<CaptureStream> stream;
    };

    std::vector<InputTarget> inputs;                ///< device inputs (recording)
    std::array<CaptureStream*, static_cast<std::size_t>(kMaxTracks)> trackOutputs{}; ///< by slot (stems)
    std::vector<std::shared_ptr<CaptureStream>> ownedTrackStreams;
    std::shared_ptr<CaptureStream> master;          ///< master output (real-time bounce)

    bool captureMidi = false;
    std::vector<CapturedMidiEvent> midiEvents;      ///< capacity reserved up front (size == capacity)
    std::atomic<int> midiCount{0};

    bool onlyWhilePlaying = true;
    bool suspendLoop = false; ///< ignore the loop range while capturing (recording)
};

} // namespace dojo::engine
