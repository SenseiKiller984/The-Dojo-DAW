#include "engine/AudioEngine.h"

#include "core/rt/AtomicUtils.h"
#include "core/rt/RealtimeGuard.h"
#include "engine/Capture.h"
#include "engine/ProcessorNode.h"
#include "engine/dsp/ClipRenderer.h"

#include <algorithm>
#include <cmath>
#include <thread>
#include <variant>

namespace dojo::engine
{
namespace
{
constexpr double kMasterGainRampSeconds = 0.050;
constexpr double kTrackGainRampSeconds = 0.030;
constexpr double kLoadSmoothingCoefficient = 0.1; // one-pole, ~10-block time constant
constexpr int kMidiBufferBytes = 16384;

void clearOutputs(float* const* outputs, int numOutputs, int offset, int numSamples) noexcept
{
    for (int ch = 0; ch < numOutputs; ++ch)
        if (outputs[ch] != nullptr)
            std::fill_n(outputs[ch] + offset, numSamples, 0.0f);
}

void addAllNotesOff(juce::MidiBuffer& midi) noexcept
{
    for (int channel = 1; channel <= 16; ++channel)
    {
        midi.addEvent(juce::MidiMessage::controllerEvent(channel, 64, 0), 0); // sustain off
        midi.addEvent(juce::MidiMessage::allNotesOff(channel), 0);
    }
}
} // namespace

AudioEngine::AudioEngine(int numWorkerThreads)
    : workers_(numWorkerThreads)
{
    slots_.resize(static_cast<std::size_t>(kMaxTracks));
}

AudioEngine::~AudioEngine()
{
    // By contract nothing is rendering any more, so this (message) thread may now act as the
    // consumer of the command queue and reclaim every graph the engine still owns.
    EngineCommand command;
    while (commands_.tryPop(command))
        if (const auto* swap = std::get_if<command::SwapGraph>(&command))
            delete swap->graph;

    releaseRetiredGraphs();

    for (std::size_t i = 0; i < retireBacklogSize_; ++i)
        delete retireBacklog_[i];

    delete graph_;
}

//==================================================================================================
// Lifecycle

void AudioEngine::prepare(double sampleRate, int maximumBlockSize, int numOutputChannels, int numInputChannels)
{
    jassert(sampleRate > 0.0 && maximumBlockSize > 0 && numOutputChannels >= 0);

    prepared_.store(false, std::memory_order_release);

    sampleRate_ = sampleRate;
    maxBlockSize_ = std::max(1, maximumBlockSize);
    numChannels_ = std::max(0, numOutputChannels);
    numInputs_ = std::clamp(numInputChannels, 0, kMaxInputChannels);

    // The only allocations in the engine's life cycle — done here, never on the audio thread.
    masterBus_.setSize(kMaxChannels, maxBlockSize_, false, true, false);
    masterScratch_.setSize(2, maxBlockSize_, false, true, false);
    inputBus_.setSize(std::max(1, numInputs_), maxBlockSize_, false, true, false);
    masterMidi_.ensureSize(kMidiBufferBytes);
    liveBlock_.ensureSize(kMidiBufferBytes);

    for (std::size_t index = 0; index < slots_.size(); ++index)
    {
        auto& slot = slots_[index];
        const bool wide = static_cast<int>(index) >= kFirstWideSlot;
        const int channels = wide ? kMaxChannels : 2;
        slot.main.setSize(channels, maxBlockSize_, false, true, false);
        slot.pre.setSize(channels, maxBlockSize_, false, true, false);
        slot.midi.ensureSize(kMidiBufferBytes);
        if (slot.delay.getNumSamples() != kMaxCompensationSamples || slot.delay.getNumChannels() != channels)
            slot.delay.setSize(channels, kMaxCompensationSamples, false, true, false);
        if (wide)
        {
            slot.out.setSize(kMaxChannels, maxBlockSize_, false, true, false);
            slot.groupScratch.setSize(2, maxBlockSize_, false, true, false);
        }
        slot.matrixValid = false;
        slot.delay.clear();
        slot.delayWrite = 0;
        slot.currentDelay = 0;
        slot.gainLeft.reset(sampleRate_, kTrackGainRampSeconds);
        slot.gainRight.reset(sampleRate_, kTrackGainRampSeconds);
        slot.postGain.reset(sampleRate_, kTrackGainRampSeconds);
        slot.postGain.setCurrentAndTargetValue(1.0);
        slot.trackId = 0;
    }

    masterGain_.reset(sampleRate_, kMasterGainRampSeconds);
    masterFaderGain_.reset(sampleRate_, kMasterGainRampSeconds);
    masterFaderGain_.setCurrentAndTargetValue(1.0);
    masterGain_.setCurrentAndTargetValue(masterGainTarget_);
    testTone_.prepare(sampleRate_);
    smoothedLoad_ = 0.0;
    updatePlayHead();

    publishedSampleRate_.store(sampleRate_, std::memory_order_relaxed);
    publishedBlockSize_.store(maxBlockSize_, std::memory_order_relaxed);

    if (onPrepare)
        onPrepare(sampleRate_, maxBlockSize_);

    prepared_.store(true, std::memory_order_release);
}

void AudioEngine::release() noexcept
{
    prepared_.store(false, std::memory_order_release);
    telemetry_.playing.store(false, std::memory_order_relaxed);
}

//==================================================================================================
// Message thread

bool AudioEngine::post(const EngineCommand& command) noexcept
{
    // Single-producer queue: every post must come from the same (message) thread.
    jassert(commandProducer_.isOwnedByCallingThread());
    return commands_.tryPush(command);
}

TelemetrySnapshot AudioEngine::collectTelemetry() noexcept
{
    TelemetrySnapshot snapshot;
    snapshot.masterPeakLeft = telemetry_.masterPeakLeft.exchange(0.0f, std::memory_order_acq_rel);
    snapshot.masterPeakRight = telemetry_.masterPeakRight.exchange(0.0f, std::memory_order_acq_rel);
    snapshot.masterChannels = telemetry_.masterChannels.load(std::memory_order_relaxed);
    for (std::size_t ch = 0; ch < snapshot.masterPeaks.size(); ++ch)
        snapshot.masterPeaks[ch] = telemetry_.masterPeaks[ch].exchange(0.0f, std::memory_order_acq_rel);
    snapshot.dspLoad = telemetry_.dspLoad.load(std::memory_order_relaxed);
    snapshot.dspLoadPeak = telemetry_.dspLoadPeak.exchange(0.0f, std::memory_order_acq_rel);
    snapshot.playheadSamples = telemetry_.playheadSamples.load(std::memory_order_relaxed);
    snapshot.playheadSeconds = telemetry_.playheadSeconds.load(std::memory_order_relaxed);
    snapshot.playing = telemetry_.playing.load(std::memory_order_relaxed);
    snapshot.stoppedAtEnd = telemetry_.stoppedAtEnd.load(std::memory_order_relaxed);
    snapshot.processedBlocks = telemetry_.processedBlocks.load(std::memory_order_relaxed);
    snapshot.clippedSamples = telemetry_.clippedSamples.load(std::memory_order_relaxed);
    snapshot.captureActive = telemetry_.captureActive.load(std::memory_order_acquire);
    return snapshot;
}

StereoPeak AudioEngine::collectTrackPeak(int slot) noexcept
{
    if (slot < 0 || slot >= kMaxTracks)
        return {};

    const auto index = static_cast<std::size_t>(slot) * kMaxChannels;
    return {telemetry_.trackPeaks[index].exchange(0.0f, std::memory_order_acq_rel),
            telemetry_.trackPeaks[index + 1].exchange(0.0f, std::memory_order_acq_rel)};
}

ChannelPeaks AudioEngine::collectTrackPeaks(int slot) noexcept
{
    ChannelPeaks peaks;
    if (slot < 0 || slot >= kMaxTracks)
        return peaks;

    const auto index = static_cast<std::size_t>(slot) * kMaxChannels;
    for (std::size_t ch = 0; ch < static_cast<std::size_t>(kMaxChannels); ++ch)
        peaks.peak[ch] = telemetry_.trackPeaks[index + ch].exchange(0.0f, std::memory_order_acq_rel);
    return peaks;
}

float AudioEngine::collectInputPeak(int channel) noexcept
{
    if (channel < 0 || channel >= kMaxInputChannels)
        return 0.0f;
    return telemetry_.inputPeaks[static_cast<std::size_t>(channel)].exchange(0.0f, std::memory_order_acq_rel);
}

void AudioEngine::releaseRetiredGraphs() noexcept
{
    RenderGraph* graph = nullptr;
    while (retiredGraphs_.tryPop(graph))
        delete graph;
}

//==================================================================================================
// Offline rendering

bool AudioEngine::beginOfflineRender(int timeoutMs)
{
    // Dekker-style hand-over (sequentially consistent): either a live callback that is starting now
    // sees the request and bails out, or we see it running and wait for it to finish.
    offlineRequested_.store(true);

    const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
    bool idle = !liveCallbackActive_.load();
    while (!idle && juce::Time::getMillisecondCounter() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        idle = !liveCallbackActive_.load();
    }

    if (!idle)
    {
        offlineRequested_.store(false);
        return false;
    }

    offlineActive_.store(true, std::memory_order_release);
    return true;
}

void AudioEngine::renderOffline(float* const* outputs, int numOutputs, int numSamples) noexcept
{
    const rt::ScopedRealtimeSection realtimeSection;
    const juce::ScopedNoDenormals noDenormals;

    if (numSamples <= 0)
        return;

    if (maxBlockSize_ <= 0 || sampleRate_ <= 0.0)
    {
        clearOutputs(outputs, numOutputs, 0, numSamples);
        return;
    }

    renderBlock(nullptr, 0, outputs, numOutputs, numSamples);
}

void AudioEngine::setOfflineTransport(double positionSeconds, bool playing, bool ignoreLoop) noexcept
{
    if (!offlineActive_.load(std::memory_order_acquire))
        return;
    positionSeconds_ = std::max(0.0, positionSeconds);
    playing_ = playing;
    stoppedAtEnd_ = false;
    ignoreLoop_ = ignoreLoop;
    panicAll();
    telemetry_.playheadSeconds.store(positionSeconds_, std::memory_order_relaxed);
    telemetry_.playing.store(playing_, std::memory_order_relaxed);
}

void AudioEngine::applyCommandsOffline() noexcept
{
    if (offlineActive_.load(std::memory_order_acquire))
        applyPendingCommands();
}

void AudioEngine::endOfflineRender()
{
    offlineActive_.store(false, std::memory_order_release);
    offlineRequested_.store(false, std::memory_order_release);
}

//==================================================================================================
// Audio thread — THE AUDIO CALLBACK ENTRY POINT.
//
// Everything reachable from here must be real-time safe: no allocation/free, no locks, no blocking
// I/O, no logging, no UI calls, no unbounded loops.

void AudioEngine::process(const float* const* inputs, int numInputs, float* const* outputs, int numOutputs,
                          int numSamples) noexcept
{
    const rt::ScopedRealtimeSection realtimeSection; // arms the allocation trap for this thread
    const juce::ScopedNoDenormals noDenormals;        // FTZ/DAZ: denormals can cost 100x CPU
    const auto callbackStart = Clock::now();

    if (numSamples <= 0)
        return;

    liveCallbackActive_.store(true);
    if (offlineRequested_.load())
    {
        // An export thread is rendering: stay silent and keep our hands off all engine state.
        liveCallbackActive_.store(false);
        clearOutputs(outputs, numOutputs, 0, numSamples);
        return;
    }

    if (!prepared_.load(std::memory_order_acquire))
    {
        liveCallbackActive_.store(false);
        clearOutputs(outputs, numOutputs, 0, numSamples);
        return;
    }

    renderBlock(inputs, numInputs, outputs, numOutputs, numSamples);
    preview_.addTo(outputs, numOutputs, numSamples); // after the mix: never metered, recorded or exported
    publishLoad(Clock::now() - callbackStart, numSamples);
    liveCallbackActive_.store(false);
}

void AudioEngine::renderBlock(const float* const* inputs, int numInputs, float* const* outputs, int numOutputs,
                              int numSamples) noexcept
{
    applyPendingCommands();

    // Hand back any retired graphs that did not fit in the return queue last time.
    while (retireBacklogSize_ > 0 && retiredGraphs_.tryPush(retireBacklog_[retireBacklogSize_ - 1]))
        --retireBacklogSize_;

    // Live MIDI: drain everything that arrived since the last callback (placed at the block start of
    // the first sub-block only).
    liveBlock_.clear();
    liveBlockPending_ = true;
    numTargetedMidi_ = 0;
    LiveMidiEvent event;
    while (liveMidi_.pop(event))
    {
        if (event.trackId != 0)
        {
            // A previewed note: for its track only, and never recorded.
            if (numTargetedMidi_ < kMaxTargetedMidi)
                targetedMidi_[static_cast<std::size_t>(numTargetedMidi_++)] = event;
            continue;
        }
        liveBlock_.addEvent(event.data, event.size, 0);

        if (capture_ != nullptr && capture_->captureMidi && playing_)
        {
            const int index = capture_->midiCount.load(std::memory_order_relaxed);
            if (index < static_cast<int>(capture_->midiEvents.size()))
            {
                auto& captured = capture_->midiEvents[static_cast<std::size_t>(index)];
                captured.seconds = positionSeconds_;
                std::copy_n(event.data, event.size, captured.data);
                captured.size = event.size;
                capture_->midiCount.store(index + 1, std::memory_order_release);
            }
        }
    }

    for (int offset = 0; offset < numSamples;)
    {
        int subBlock = std::min(numSamples - offset, maxBlockSize_);
        bool wraps = false;
        bool reachesEnd = false;

        const bool loopSuspended = ignoreLoop_ || (capture_ != nullptr && capture_->suspendLoop);
        const bool loopWillWrap =
            playing_ && loopEnabled_ && !loopSuspended && loopEnd_ > loopStart_ + 0.001 && positionSeconds_ < loopEnd_;
        if (loopWillWrap)
        {
            const double secondsPerSample = speed_ / sampleRate_;
            const double samplesToLoopEnd = (loopEnd_ - positionSeconds_) / secondsPerSample;
            if (samplesToLoopEnd <= static_cast<double>(subBlock))
            {
                subBlock = std::max(1, static_cast<int>(std::ceil(samplesToLoopEnd - 1.0e-9)));
                wraps = true;
            }
        }

        // Stop at the end of the arrangement: only when playback crosses it (starting beyond the
        // end plays on), never while a loop wraps, while capturing or in an offline render.
        const double end = graph_ != nullptr ? graph_->arrangementEnd : 0.0;
        if (playing_ && stopAtEnd_ && !loopWillWrap && capture_ == nullptr && !offlineActive_.load(std::memory_order_relaxed) &&
            end > 0.0 && positionSeconds_ < end)
        {
            const double secondsPerSample = speed_ / sampleRate_;
            const double samplesToEnd = (end - positionSeconds_) / secondsPerSample;
            if (samplesToEnd <= static_cast<double>(subBlock))
            {
                subBlock = std::max(1, static_cast<int>(std::ceil(samplesToEnd - 1.0e-9)));
                reachesEnd = true;
            }
        }

        // Device inputs for this sub-block, as 64-bit (monitoring + capture).
        for (int ch = 0; ch < numInputs_; ++ch)
        {
            Sample* dest = inputBus_.getWritePointer(ch);
            const float* source = (inputs != nullptr && ch < numInputs) ? inputs[ch] : nullptr;
            float peak = 0.0f;

            if (source == nullptr)
            {
                std::fill_n(dest, subBlock, 0.0);
                continue;
            }

            for (int i = 0; i < subBlock; ++i)
            {
                const float value = source[offset + i];
                dest[i] = static_cast<Sample>(value);
                peak = std::max(peak, std::abs(value));
            }

            rt::atomicStoreMax(telemetry_.inputPeaks[static_cast<std::size_t>(ch)], peak);
        }

        renderSubBlock(offset, outputs, numOutputs, offset, subBlock);

        if (wraps)
        {
            positionSeconds_ = loopStart_ + std::max(0.0, positionSeconds_ - loopEnd_);
            panicAll();
        }
        else if (reachesEnd)
        {
            playing_ = false;
            positionSeconds_ = end;
            stoppedAtEnd_ = true; // the next play starts from the beginning
            panicAll();
        }

        offset += subBlock;
    }

    telemetry_.playheadSeconds.store(positionSeconds_, std::memory_order_relaxed);
    telemetry_.playheadSamples.store(static_cast<SampleCount>(std::llround(positionSeconds_ * sampleRate_)),
                                     std::memory_order_relaxed);
    telemetry_.playing.store(playing_, std::memory_order_relaxed);
    telemetry_.stoppedAtEnd.store(stoppedAtEnd_, std::memory_order_relaxed);
    telemetry_.processedBlocks.fetch_add(1, std::memory_order_relaxed);
    epoch_.fetch_add(1, std::memory_order_acq_rel);
}

void AudioEngine::renderSubBlock(int /*inputOffset*/, float* const* outputs, int numOutputs, int offset,
                                 int numSamples) noexcept
{
    blockStartSeconds_ = positionSeconds_;
    secondsPerSample_ = speed_ / sampleRate_;
    currentNumSamples_ = numSamples;
    updatePlayHead();

    masterBus_.clear(0, numSamples);

    if (graph_ != nullptr)
    {
        for (const auto& level : graph_->levels)
        {
            currentLevel_ = &level;
            workers_.run(static_cast<int>(level.size()), &AudioEngine::processTrackTask, this);
        }

        renderMaster(numSamples);
    }

    if (graph_ != nullptr)
        testTone_.addTo(masterBus_, numSamples, graph_->masterLayout.indexOf(Speaker::left), graph_->masterLayout.indexOf(Speaker::right));
    else
        testTone_.addTo(masterBus_, numSamples);

    // Master gain (graph value or automation), smoothed.
    if (graph_ != nullptr && !graph_->masterVolumeAutomation.isEmpty())
        masterGain_.setTargetValue(automationToGain(graph_->masterVolumeAutomation.valueAt(blockStartSeconds_)));

    const int numMaster = masterChannels();
    if (!masterGain_.isSmoothing())
    {
        for (int ch = 0; ch < numMaster; ++ch)
            masterBus_.applyGain(ch, 0, numSamples, masterGain_.getTargetValue());
    }
    else
    {
        auto* const* channels = masterBus_.getArrayOfWritePointers();
        for (int i = 0; i < numSamples; ++i)
        {
            const Sample gain = masterGain_.getNextValue();
            for (int ch = 0; ch < numMaster; ++ch)
                channels[ch][i] *= gain;
        }
    }

    if (graph_ != nullptr && graph_->hasMasterFader)
        applyMasterFader(numSamples);

    if (capture_ != nullptr && (playing_ || !capture_->onlyWhilePlaying))
    {
        captureInputs(0, numSamples);

        if (capture_->master != nullptr)
        {
            if (capture_->master->getNumChannels() >= numMaster || graph_ == nullptr)
                capture_->master->write(masterBus_.getArrayOfReadPointers(), numMaster, numSamples);
            else
            {
                // A stereo file of a surround master (e.g. MP3): its fold-down.
                foldDown(masterBus_, numMaster, graph_->masterFoldDown, masterScratch_, numSamples);
                capture_->master->write(masterScratch_.getArrayOfReadPointers(), 2, numSamples);
            }
        }

        if (telemetry_.captureStartSeconds.load(std::memory_order_relaxed) < 0.0)
            telemetry_.captureStartSeconds.store(blockStartSeconds_, std::memory_order_release);
    }

    writeOutputsAndMeter(outputs, numOutputs, offset, numSamples);
    liveBlockPending_ = false;

    if (playing_)
        positionSeconds_ += static_cast<double>(numSamples) * secondsPerSample_;
}

//==================================================================================================
// Commands

void AudioEngine::applyPendingCommands() noexcept
{
    // Bounded: at most one queue's worth of commands per block.
    commands_.drain([this](const EngineCommand& command) noexcept
                    { std::visit([this](const auto& cmd) noexcept { handle(cmd); }, command); },
                    commands_.capacity());
}

void AudioEngine::handle(const command::SetPlaying& cmd) noexcept
{
    if (playing_ && !cmd.shouldPlay)
        panicAll();
    // After stopping at the end of the arrangement, playing again starts from the beginning (but a
    // recording starts where the playhead is: it may extend the song).
    if (cmd.shouldPlay && !playing_ && stoppedAtEnd_ && capture_ == nullptr)
        positionSeconds_ = 0.0;
    if (cmd.shouldPlay)
        stoppedAtEnd_ = false;
    playing_ = cmd.shouldPlay;
}

void AudioEngine::handle(const command::Locate& cmd) noexcept
{
    const double rate = sampleRate_ > 0.0 ? sampleRate_ : 48000.0;
    positionSeconds_ = std::max(0.0, static_cast<double>(cmd.positionSamples) / rate);
    stoppedAtEnd_ = false; // the user put the playhead somewhere: play from there
    panicAll();
}

void AudioEngine::handle(const command::SetMasterGain& cmd) noexcept
{
    masterGainTarget_ = std::clamp(cmd.linearGain, 0.0, 4.0); // hard ceiling: +12 dB
    masterGain_.setTargetValue(masterGainTarget_);
}

void AudioEngine::handle(const command::SetTestTone& cmd) noexcept
{
    testTone_.setParameters(cmd.enabled, cmd.frequencyHz, cmd.linearGain);
}

void AudioEngine::handle(const command::SetLoop& cmd) noexcept
{
    loopEnabled_ = cmd.enabled;
    loopStart_ = std::max(0.0, cmd.startSeconds);
    loopEnd_ = std::max(loopStart_, cmd.endSeconds);
}

void AudioEngine::handle(const command::SetSpeed& cmd) noexcept
{
    speed_ = std::clamp(cmd.rate, 0.25, 4.0);
}

void AudioEngine::handle(const command::SetStopAtEnd& cmd) noexcept
{
    stopAtEnd_ = cmd.enabled;
    if (!cmd.enabled)
        stoppedAtEnd_ = false;
}

void AudioEngine::handle(const command::SwapGraph& cmd) noexcept
{
    RenderGraph* const previous = graph_;
    graph_ = cmd.graph;

    if (graph_ != nullptr)
    {
        masterGainTarget_ = std::clamp(graph_->masterGain, 0.0, 4.0);
        masterGain_.setTargetValue(masterGainTarget_);
        telemetry_.appliedGraphSerial.store(graph_->serial, std::memory_order_release);

        if (graph_->hasTransportSettings)
        {
            speed_ = std::clamp(graph_->speed, 0.25, 4.0);
            loopEnabled_ = graph_->loopEnabled;
            loopStart_ = std::max(0.0, graph_->loopStart);
            loopEnd_ = std::max(loopStart_, graph_->loopEnd);
        }
    }

    retire(previous);
}

void AudioEngine::handle(const command::StartCapture& cmd) noexcept
{
    capture_ = cmd.session;
    telemetry_.captureStartSeconds.store(-1.0, std::memory_order_release);
    telemetry_.captureActive.store(capture_ != nullptr, std::memory_order_release);
}

void AudioEngine::handle(const command::StopCapture&) noexcept
{
    capture_ = nullptr;
    telemetry_.captureActive.store(false, std::memory_order_release);
    telemetry_.captureStops.fetch_add(1, std::memory_order_acq_rel);
}

void AudioEngine::retire(RenderGraph* graph) noexcept
{
    if (graph == nullptr)
        return;

    if (retiredGraphs_.tryPush(graph))
        return;

    if (retireBacklogSize_ < retireBacklog_.size())
        retireBacklog_[retireBacklogSize_++] = graph;
    // else: unreachable in practice (EngineController keeps at most one graph in flight). Leaking is
    // the only real-time-safe option left, and it is preferable to freeing memory on this thread.
}

void AudioEngine::panicAll() noexcept
{
    for (auto& slot : slots_)
        slot.panic = true;
}

//==================================================================================================
// Tracks (run on the audio thread and the worker threads; each task touches only its own slot,
// and reads slots of earlier levels)

void AudioEngine::processTrackTask(void* context, int taskIndex) noexcept
{
    auto* self = static_cast<AudioEngine*>(context);
    self->processTrack((*self->currentLevel_)[static_cast<std::size_t>(taskIndex)]);
}

void AudioEngine::processTrack(int graphIndex) noexcept
{
    const auto& track = graph_->tracks[static_cast<std::size_t>(graphIndex)];
    if (track.slot < 0 || track.slot >= kMaxTracks)
        return;

    auto& slot = slots_[static_cast<std::size_t>(track.slot)];
    const int numSamples = currentNumSamples_;

    if (slot.trackId != track.trackId)
    {
        // A different track now owns this slot: start clean.
        slot.trackId = track.trackId;
        slot.delay.clear();
        slot.delayWrite = 0;
        slot.currentDelay = track.compensationDelay;
        slot.gainLeft.setCurrentAndTargetValue(0.0);
        slot.gainRight.setCurrentAndTargetValue(0.0);
        slot.postGain.setCurrentAndTargetValue(track.hasPostFader && track.postAudible ? track.postVolume : 1.0);
        slot.matrixValid = false;
        slot.panic = true;
    }

    if (track.isWide() && track.slot < kFirstWideSlot)
        return; // (the compiler always gives multichannel tracks a wide slot)

    slot.main.clear(0, numSamples);
    slot.midi.clear();

    if (slot.panic)
    {
        addAllNotesOff(slot.midi);
        slot.panic = false;
    }

    applyParameterAutomation(track.parameterLanes);

    switch (track.kind)
    {
    case TrackKind::audio:
        renderAudioTrackSources(track, slot);
        break;

    case TrackKind::instrument:
        if (playing_)
            renderMidiClips(track, slot);
        if (track.receivesLiveMidi && liveBlockPending_)
            slot.midi.addEvents(liveBlock_, 0, numSamples, 0);
        addTargetedMidi(track.trackId, slot.midi);
        // MIDI tracks routed here (rendered in an earlier level), unless muted.
        for (const int source : track.midiSources)
        {
            const auto& sourceTrack = graph_->tracks[static_cast<std::size_t>(source)];
            if (sourceTrack.audible)
                slot.midi.addEvents(slots_[static_cast<std::size_t>(sourceTrack.slot)].midi, 0, numSamples, 0);
        }
        break;

    case TrackKind::midi:
        // MIDI only: its instrument track picks it up from this slot.
        if (playing_)
            renderMidiClips(track, slot);
        if (track.receivesLiveMidi && liveBlockPending_)
            slot.midi.addEvents(liveBlock_, 0, numSamples, 0);
        addTargetedMidi(track.trackId, slot.midi);
        break;

    case TrackKind::vca:
    case TrackKind::masterFader:
    case TrackKind::video:
        break; // control and video tracks: never in the render graph

    case TrackKind::bus:
        sumBusSources(track, slot);
        break;
    }

    if (track.mono)
    {
        // A mono track carries one signal: fold whatever its sources produced (e.g. a stereo clip).
        Sample* left = slot.main.getWritePointer(0);
        Sample* right = slot.main.getWritePointer(1);
        for (int i = 0; i < numSamples; ++i)
            left[i] = right[i] = 0.5 * (left[i] + right[i]);
    }

    runChain(track.devices, slot.main, track.layout.numChannels, track.groups, slot.midi, &slot.groupScratch);

    if (track.needsPreFaderCopy)
    {
        // Pre-fader sends ignore the fader, but not mute/solo (a muted track must not leak into a
        // reverb bus).
        for (int ch = 0; ch < track.layout.numChannels; ++ch)
        {
            if (track.audible)
                slot.pre.copyFrom(ch, 0, slot.main, ch, 0, numSamples);
            else
                slot.pre.clear(ch, 0, numSamples);
        }
    }

    applyFader(track, slot);
    if (track.hasPostFader)
        applyPostFader(track, slot);
    applyCompensationDelay(track, slot);

    if (capture_ != nullptr && (playing_ || !capture_->onlyWhilePlaying))
        if (auto* stream = capture_->trackOutputs[static_cast<std::size_t>(track.slot)])
        {
            if (stream->getNumChannels() >= track.outputLayout.numChannels || slot.groupScratch.getNumChannels() < 2)
                stream->write(slot.main.getArrayOfReadPointers(), track.outputLayout.numChannels, numSamples);
            else
            {
                foldDown(slot.main, track.outputLayout.numChannels, conversionMatrix(track.outputLayout, ChannelLayout::stereo()),
                         slot.groupScratch, numSamples);
                stream->write(slot.groupScratch.getArrayOfReadPointers(), 2, numSamples);
            }
        }
}

void AudioEngine::renderAudioClips(const RenderTrack& track, SlotState& slot) noexcept
{
    const int numSamples = currentNumSamples_;
    const double blockEnd = blockStartSeconds_ + numSamples * secondsPerSample_;
    auto* const* channels = slot.main.getArrayOfWritePointers();
    const int numChannels = std::min(track.layout.numChannels, slot.main.getNumChannels());

    for (const auto& clip : track.clips)
    {
        if (clip.startSeconds >= blockEnd)
            break; // clips are sorted by start: nothing later can overlap this block
        if (clip.endSeconds <= blockStartSeconds_)
            continue;

        dsp::renderClip(clip, blockStartSeconds_, secondsPerSample_, numSamples, channels, numChannels);
    }
}

void AudioEngine::addTargetedMidi(std::uint32_t trackId, juce::MidiBuffer& midi) const noexcept
{
    if (!liveBlockPending_)
        return;
    for (int i = 0; i < numTargetedMidi_; ++i)
    {
        const auto& event = targetedMidi_[static_cast<std::size_t>(i)];
        if (event.trackId == trackId)
            midi.addEvent(event.data, event.size, 0);
    }
}

void AudioEngine::renderMidiClips(const RenderTrack& track, SlotState& slot) noexcept
{
    const int numSamples = currentNumSamples_;
    const double t0 = blockStartSeconds_;
    const double t1 = t0 + numSamples * secondsPerSample_;
    const double tempoBeatsPerSecond = graph_->tempo / 60.0;

    const auto offsetOf = [&](double seconds)
    {
        const auto offset = static_cast<int>(std::floor((seconds - t0) / secondsPerSample_));
        return std::clamp(offset, 0, numSamples - 1);
    };

    // One clip (or one iteration of a looped one) from clipStart to clipEnd.
    const auto schedule = [&](const RenderMidiClip& clip, double clipStart, double clipEnd)
    {
        const double beatsPerSecond = tempoBeatsPerSecond * (clip.rate > 0.0 ? clip.rate : 1.0);
        const auto beatAt = [&](double seconds)
        { return clip.contentStartBeat + (seconds - clipStart) * beatsPerSecond; };
        const auto timeOf = [&](double beat)
        { return clipStart + (beat - clip.contentStartBeat) / beatsPerSecond; };

        const double beginBeat = beatAt(std::max(t0, clipStart));
        const double endBeat = beatAt(std::min(t1, clipEnd));
        const double clipEndBeat = beatAt(clipEnd);
        const bool clipEndsInBlock = clipEnd <= t1;

        // Note-offs first, so a note re-struck at the same sample is released before it restarts.
        for (const auto& note : clip.notes)
        {
            if (note.startBeat < clip.contentStartBeat || note.startBeat >= clipEndBeat)
                continue;

            const double offBeat = std::min(note.endBeat, clipEndBeat);
            const bool inBlock = offBeat >= beginBeat && (offBeat < endBeat || (clipEndsInBlock && offBeat <= endBeat + 1.0e-9));
            if (inBlock)
                slot.midi.addEvent(juce::MidiMessage::noteOff(note.channel, note.note),
                                   clipEndsInBlock && offBeat >= endBeat - 1.0e-9 ? numSamples - 1 : offsetOf(timeOf(offBeat)));
        }

        for (const auto& note : clip.notes)
        {
            if (note.startBeat >= endBeat)
                break;
            if (note.startBeat < beginBeat || note.startBeat < clip.contentStartBeat)
                continue;

            slot.midi.addEvent(juce::MidiMessage::noteOn(note.channel, note.note, note.velocity),
                               offsetOf(timeOf(note.startBeat)));
        }

        for (const auto& control : clip.controls)
        {
            if (control.beat >= endBeat)
                break;
            if (control.beat < beginBeat || control.beat < clip.contentStartBeat)
                continue;

            const int offset = offsetOf(timeOf(control.beat));
            if (control.controller == 128)
                slot.midi.addEvent(juce::MidiMessage::pitchWheel(control.channel,
                                                                 juce::jlimit(0, 16383, juce::roundToInt(control.value * 16383.0f))),
                                   offset);
            else
                slot.midi.addEvent(juce::MidiMessage::controllerEvent(control.channel, control.controller,
                                                                      juce::jlimit(0, 127, juce::roundToInt(control.value * 127.0f))),
                                   offset);
        }
    };

    for (const auto& clip : track.midiClips)
    {
        if (clip.startSeconds >= t1)
            break;
        if (clip.endSeconds <= t0)
            continue;

        if (clip.loopLength > 0.0 && clip.loopLength < clip.endSeconds - clip.startSeconds - 1.0e-9)
        {
            // Looped: every iteration replays the first loopLength seconds of content.
            const auto first = static_cast<long long>(std::max(0.0, std::floor((t0 - clip.startSeconds) / clip.loopLength)));
            const auto last = static_cast<long long>(std::floor((std::min(t1, clip.endSeconds) - clip.startSeconds) / clip.loopLength));
            for (auto k = first; k <= last; ++k)
            {
                const double iterationStart = clip.startSeconds + static_cast<double>(k) * clip.loopLength;
                const double iterationEnd = std::min(iterationStart + clip.loopLength, clip.endSeconds);
                if (iterationEnd > t0 && iterationStart < t1 && iterationEnd > iterationStart)
                    schedule(clip, iterationStart, iterationEnd);
            }
            continue;
        }
        schedule(clip, clip.startSeconds, clip.endSeconds);
    }
}

void AudioEngine::renderAudioTrackSources(const RenderTrack& track, SlotState& slot) noexcept
{
    const int numSamples = currentNumSamples_;

    if (!(track.monitor == MonitorMode::automatic && track.armed))
    {
        // Not record-enabled with automatic monitoring: the clips, plus the input when monitoring is "IN".
        if (playing_)
            renderAudioClips(track, slot);
        if (track.monitor == MonitorMode::always)
            addInput(track, slot, 0, numSamples);
        return;
    }

    // Record-enabled. Input Only: always the input. Auto Input (tape style): the input while stopped
    // and while recording inside the punch window; the track's clips while playing otherwise.
    int from = 0;
    int to = numSamples;
    if (playing_ && !graph_->inputOnlyMonitoring)
    {
        const bool recording = capture_ != nullptr && (!capture_->inputs.empty() || capture_->captureMidi);
        if (!recording)
        {
            from = to = 0;
        }
        else
        {
            const auto sampleAt = [&](double seconds)
            {
                const double position = std::ceil((seconds - blockStartSeconds_) / secondsPerSample_ - 1.0e-9);
                return static_cast<int>(std::clamp(position, 0.0, static_cast<double>(numSamples)));
            };
            from = sampleAt(graph_->punchStart);
            to = std::max(from, sampleAt(graph_->punchEnd));
        }

        if (from > 0 || to < numSamples)
        {
            renderAudioClips(track, slot);
            if (to > from)
                for (int ch = 0; ch < track.layout.numChannels; ++ch)
                    slot.main.clear(ch, from, to - from);
        }
    }

    addInput(track, slot, from, to);
}

void AudioEngine::addInput(const RenderTrack& track, SlotState& slot, int from, int to) noexcept
{
    if (to <= from || track.inputChannel < 0 || track.inputChannel >= numInputs_)
        return;

    if (track.inputCount > 2)
    {
        // A multichannel track: one input per channel, consecutive from the first.
        for (int ch = 0; ch < std::min(track.inputCount, track.layout.numChannels); ++ch)
            if (track.inputChannel + ch < numInputs_)
                slot.main.addFrom(ch, from, inputBus_, track.inputChannel + ch, from, to - from);
        return;
    }

    const int second = track.inputStereo && track.inputChannel + 1 < numInputs_ ? track.inputChannel + 1 : track.inputChannel;
    slot.main.addFrom(0, from, inputBus_, track.inputChannel, from, to - from);
    slot.main.addFrom(1, from, inputBus_, second, from, to - from);
}

void AudioEngine::sumBusSources(const RenderTrack& track, SlotState& slot) noexcept
{
    const int numSamples = currentNumSamples_;

    const int numChannels = std::min(track.layout.numChannels, slot.main.getNumChannels());

    // Tracks routed here already deliver this bus's channels (their faders convert).
    for (const int source : track.busSources)
    {
        const auto& sourceSlot = slots_[static_cast<std::size_t>(graph_->tracks[static_cast<std::size_t>(source)].slot)];
        for (int ch = 0; ch < std::min(numChannels, sourceSlot.main.getNumChannels()); ++ch)
            slot.main.addFrom(ch, 0, sourceSlot.main, ch, 0, numSamples);
    }

    for (const auto& send : track.sends)
    {
        const auto& sourceSlot = slots_[static_cast<std::size_t>(graph_->tracks[static_cast<std::size_t>(send.sourceIndex)].slot)];
        const auto& buffer = send.preFader ? sourceSlot.pre : sourceSlot.main;
        const double gain = send.gainAutomation.isEmpty() ? send.gain
                                                          : automationToGain(send.gainAutomation.valueAt(blockStartSeconds_));
        if (send.convert)
        {
            // Into a bus of another layout: by speaker.
            for (int o = 0; o < std::min(send.matrix.numOutputs, numChannels); ++o)
                for (int i = 0; i < std::min(send.matrix.numInputs, buffer.getNumChannels()); ++i)
                    if (const double g = send.matrix.gain[static_cast<std::size_t>(o)][static_cast<std::size_t>(i)]; std::abs(g) > 1.0e-12)
                        slot.main.addFrom(o, 0, buffer, i, 0, numSamples, g * gain);
            continue;
        }
        for (int ch = 0; ch < std::min(numChannels, buffer.getNumChannels()); ++ch)
            slot.main.addFrom(ch, 0, buffer, ch, 0, numSamples, gain);
    }

    if (track.auxSourceIndex >= 0)
    {
        const auto& source = graph_->tracks[static_cast<std::size_t>(track.auxSourceIndex)];
        if (!source.devices.empty())
        {
            const auto* node = source.devices.front().node;
            for (int ch = 0; ch < 2; ++ch)
            {
                const Sample* aux = node->getAuxOutput(track.auxChannel + ch);
                if (aux == nullptr)
                    continue;
                if (!track.layout.isMultichannel())
                {
                    juce::FloatVectorOperations::add(slot.main.getWritePointer(ch), aux, numSamples);
                    continue;
                }
                for (int o = 0; o < std::min(track.auxMatrix.numOutputs, numChannels); ++o)
                    if (const double g = track.auxMatrix.gain[static_cast<std::size_t>(o)][static_cast<std::size_t>(ch)]; std::abs(g) > 1.0e-12)
                        juce::FloatVectorOperations::addWithMultiply(slot.main.getWritePointer(o), aux, g, numSamples);
            }
        }
    }
}

void AudioEngine::applyParameterAutomation(const std::vector<ParameterLane>& lanes) noexcept
{
    for (const auto& lane : lanes)
    {
        if (lane.parameter == nullptr || lane.curve.isEmpty())
            continue;

        const auto value = static_cast<float>(lane.curve.valueAt(blockStartSeconds_));
        if (std::abs(lane.parameter->getValue() - value) > 1.0e-6f)
            lane.parameter->setValue(value);
    }
}

void AudioEngine::applyFader(const RenderTrack& track, SlotState& slot) noexcept
{
    const int numSamples = currentNumSamples_;

    double volume = track.volume;
    double pan = track.pan;
    if (!track.volumeAutomation.isEmpty())
        volume = automationToGain(track.volumeAutomation.valueAt(blockStartSeconds_));
    if (!track.panAutomation.isEmpty())
        pan = track.panAutomation.valueAt(blockStartSeconds_) * 2.0 - 1.0;
    for (const auto& vca : track.vcaGains)
        volume *= vca.automation.isEmpty() ? vca.gain : automationToGain(vca.automation.valueAt(blockStartSeconds_));

    if (track.isWide())
    {
        applyWideFader(track, slot, volume, pan);
        return;
    }

    const auto [panLeft, panRight] = balancePanGains(pan);
    const double sign = track.phaseInverted ? -1.0 : 1.0;
    slot.gainLeft.setTargetValue(track.audible ? volume * panLeft * sign : 0.0);
    slot.gainRight.setTargetValue(track.audible ? volume * panRight * sign : 0.0);

    Sample* left = slot.main.getWritePointer(0);
    Sample* right = slot.main.getWritePointer(1);
    Sample peakLeft = 0.0;
    Sample peakRight = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        left[i] *= slot.gainLeft.getNextValue();
        right[i] *= slot.gainRight.getNextValue();
        peakLeft = std::max(peakLeft, std::abs(left[i]));
        peakRight = std::max(peakRight, std::abs(right[i]));
    }

    const auto meterIndex = static_cast<std::size_t>(track.slot) * kMaxChannels;
    rt::atomicStoreMax(telemetry_.trackPeaks[meterIndex], static_cast<float>(peakLeft));
    rt::atomicStoreMax(telemetry_.trackPeaks[meterIndex + 1], static_cast<float>(peakRight));
}

void AudioEngine::applyWideFader(const RenderTrack& track, SlotState& slot, double volume, double pan) noexcept
{
    // Multichannel: the fader is a matrix from the track's channels to its destination's (surround
    // panner or conversion by speaker), with volume, phase and mute folded in. It ramps towards the
    // block's target like the stereo gains do.
    const int numSamples = currentNumSamples_;
    const int numIn = std::min(track.layout.numChannels, slot.main.getNumChannels());
    const int numOut = std::min(track.outputLayout.numChannels, slot.out.getNumChannels());

    auto target = outputMatrix(track.layout, track.outputLayout, pan, track.panFrontRear, track.panCentre);
    const double scale = track.audible ? volume * (track.phaseInverted ? -1.0 : 1.0) : 0.0;
    for (int o = 0; o < numOut; ++o)
        for (int i = 0; i < numIn; ++i)
            target.gain[static_cast<std::size_t>(o)][static_cast<std::size_t>(i)] *= scale;

    if (!slot.matrixValid || slot.matrix.numInputs != numIn || slot.matrix.numOutputs != numOut)
    {
        slot.matrix = ChannelMatrix{}; // (fade in from silence, like a fresh stereo track)
        slot.matrix.numInputs = numIn;
        slot.matrix.numOutputs = numOut;
        slot.matrixTarget = slot.matrix;
        slot.rampRemaining = 0;
        slot.matrixValid = true;
    }

    // A new target starts a fresh linear ramp from where the gains are now.
    bool changed = false;
    for (int o = 0; o < numOut && !changed; ++o)
        for (int i = 0; i < numIn && !changed; ++i)
            changed = std::abs(target.gain[static_cast<std::size_t>(o)][static_cast<std::size_t>(i)]
                               - slot.matrixTarget.gain[static_cast<std::size_t>(o)][static_cast<std::size_t>(i)]) > 1.0e-12;
    if (changed)
    {
        slot.matrixTarget = target;
        slot.rampRemaining = std::max(1, static_cast<int>(std::lround(kTrackGainRampSeconds * sampleRate_)));
    }

    ChannelMatrix end = slot.matrixTarget;
    if (slot.rampRemaining > numSamples)
    {
        const double fraction = static_cast<double>(numSamples) / slot.rampRemaining;
        for (int o = 0; o < numOut; ++o)
            for (int i = 0; i < numIn; ++i)
            {
                const auto oi = static_cast<std::size_t>(o);
                const auto ii = static_cast<std::size_t>(i);
                end.gain[oi][ii] = slot.matrix.gain[oi][ii] + (slot.matrixTarget.gain[oi][ii] - slot.matrix.gain[oi][ii]) * fraction;
            }
    }
    slot.rampRemaining = std::max(0, slot.rampRemaining - numSamples);

    for (int o = 0; o < numOut; ++o)
    {
        Sample* destination = slot.out.getWritePointer(o);
        std::fill_n(destination, numSamples, 0.0);
        for (int i = 0; i < numIn; ++i)
        {
            const double g0 = slot.matrix.gain[static_cast<std::size_t>(o)][static_cast<std::size_t>(i)];
            const double g1 = end.gain[static_cast<std::size_t>(o)][static_cast<std::size_t>(i)];
            const Sample* source = slot.main.getReadPointer(i);
            if (std::abs(g1 - g0) < 1.0e-12)
            {
                if (std::abs(g0) > 1.0e-12)
                    juce::FloatVectorOperations::addWithMultiply(destination, source, g0, numSamples);
                continue;
            }
            const double delta = (g1 - g0) / numSamples;
            for (int k = 0; k < numSamples; ++k)
                destination[k] += source[k] * (g0 + delta * (k + 1));
        }
    }
    slot.matrix = end;

    const auto meterIndex = static_cast<std::size_t>(track.slot) * kMaxChannels;
    for (int o = 0; o < numOut; ++o)
    {
        slot.main.copyFrom(o, 0, slot.out, o, 0, numSamples);
        const auto range = juce::FloatVectorOperations::findMinAndMax(slot.out.getReadPointer(o), numSamples);
        const double peak = std::max(std::abs(range.getStart()), std::abs(range.getEnd()));
        rt::atomicStoreMax(telemetry_.trackPeaks[meterIndex + static_cast<std::size_t>(o)], static_cast<float>(peak));
    }
    for (int ch = numOut; ch < slot.main.getNumChannels(); ++ch)
        slot.main.clear(ch, 0, numSamples);
}

void AudioEngine::runChain(const std::vector<RenderDevice>& chain, juce::AudioBuffer<Sample>& buffer, int numChannels,
                           const ChannelGroups& groups, juce::MidiBuffer& midi, juce::AudioBuffer<Sample>* groupScratch) noexcept
{
    const int numSamples = currentNumSamples_;

    for (const auto& device : chain)
    {
        const juce::AudioBuffer<Sample>* sidechain = nullptr;
        if (device.sidechainSource >= 0)
        {
            const auto& source = graph_->tracks[static_cast<std::size_t>(device.sidechainSource)];
            sidechain = &slots_[static_cast<std::size_t>(source.slot)].main;
        }

        if (numChannels <= 2 || groups.count <= 1)
        {
            device.node->process(buffer, numSamples, midi, sidechain);
            continue;
        }

        // Linked instances: the companions follow the primary's parameters and bypass.
        const auto& parameters = device.node->getProcessor().getParameters();
        for (int g = 1; g < groups.count; ++g)
            if (auto* companion = device.companions[static_cast<std::size_t>(g - 1)])
            {
                companion->setBypassed(device.node->isBypassed());
                const auto& own = companion->getProcessor().getParameters();
                for (int p = 0; p < std::min(parameters.size(), own.size()); ++p)
                {
                    const float value = parameters.getUnchecked(p)->getValue();
                    if (std::abs(own.getUnchecked(p)->getValue() - value) > 1.0e-7f)
                        own.getUnchecked(p)->setValue(value);
                }
            }

        for (int g = 0; g < groups.count; ++g)
        {
            auto* node = g == 0 ? device.node : device.companions[static_cast<std::size_t>(g - 1)];
            const auto& group = groups.groups[static_cast<std::size_t>(g)];
            if (node == nullptr || group.first >= numChannels)
                continue; // (no instance: these channels pass through)

            if (group.second >= 0 && group.second < numChannels)
            {
                Sample* pair[2] = {buffer.getWritePointer(group.first), buffer.getWritePointer(group.second)};
                juce::AudioBuffer<Sample> view(pair, 2, numSamples);
                node->process(view, numSamples, midi, sidechain);
            }
            else if (groupScratch != nullptr && groupScratch->getNumChannels() >= 2)
            {
                // A single channel through a stereo device: dual mono in, the average out.
                groupScratch->copyFrom(0, 0, buffer, group.first, 0, numSamples);
                groupScratch->copyFrom(1, 0, buffer, group.first, 0, numSamples);
                Sample* pair[2] = {groupScratch->getWritePointer(0), groupScratch->getWritePointer(1)};
                juce::AudioBuffer<Sample> view(pair, 2, numSamples);
                node->process(view, numSamples, midi, sidechain);
                Sample* channel = buffer.getWritePointer(group.first);
                for (int i = 0; i < numSamples; ++i)
                    channel[i] = 0.5 * (pair[0][i] + pair[1][i]);
            }
        }
    }
}

void AudioEngine::foldDown(const juce::AudioBuffer<Sample>& source, int numChannels, const ChannelMatrix& matrix,
                           juce::AudioBuffer<Sample>& stereo, int numSamples) noexcept
{
    for (int o = 0; o < 2; ++o)
    {
        Sample* destination = stereo.getWritePointer(o);
        std::fill_n(destination, numSamples, 0.0);
        for (int i = 0; i < std::min(numChannels, matrix.numInputs); ++i)
            if (const double g = matrix.gain[static_cast<std::size_t>(o)][static_cast<std::size_t>(i)]; std::abs(g) > 1.0e-12)
                juce::FloatVectorOperations::addWithMultiply(destination, source.getReadPointer(i), g, numSamples);
    }
}

int AudioEngine::masterChannels() const noexcept
{
    return graph_ != nullptr ? std::min(graph_->masterLayout.numChannels, masterBus_.getNumChannels()) : 2;
}

void AudioEngine::applyPostFader(const RenderTrack& track, SlotState& slot) noexcept
{
    const int numSamples = currentNumSamples_;
    const double volume = track.postVolumeAutomation.isEmpty() ? track.postVolume
                                                               : automationToGain(track.postVolumeAutomation.valueAt(blockStartSeconds_));
    slot.postGain.setTargetValue(track.postAudible ? volume : 0.0);
    const int numChannels = std::min(track.outputLayout.numChannels, slot.main.getNumChannels());
    auto* const* channels = slot.main.getArrayOfWritePointers();
    for (int i = 0; i < numSamples; ++i)
    {
        const Sample gain = slot.postGain.getNextValue();
        for (int ch = 0; ch < numChannels; ++ch)
            channels[ch][i] *= gain;
    }

    // Post-fader devices (like the inserts of a master fader).
    runChain(track.postDevices, slot.main, numChannels, track.outputGroups, slot.midi, &slot.groupScratch);
}

void AudioEngine::applyMasterFader(int numSamples) noexcept
{
    const double volume = graph_->masterFaderAutomation.isEmpty() ? graph_->masterFaderVolume
                                                                  : automationToGain(graph_->masterFaderAutomation.valueAt(blockStartSeconds_));
    masterFaderGain_.setTargetValue(graph_->masterFaderAudible ? volume : 0.0);
    const int numMaster = masterChannels();
    auto* const* channels = masterBus_.getArrayOfWritePointers();
    for (int i = 0; i < numSamples; ++i)
    {
        const Sample gain = masterFaderGain_.getNextValue();
        for (int ch = 0; ch < numMaster; ++ch)
            channels[ch][i] *= gain;
    }

    applyParameterAutomation(graph_->masterFaderParameterLanes);
    masterMidi_.clear();
    runChain(graph_->masterFaderDevices, masterBus_, numMaster, graph_->masterGroups, masterMidi_, &masterScratch_);
}

void AudioEngine::applyCompensationDelay(const RenderTrack& track, SlotState& slot) noexcept
{
    const int delay = std::clamp(track.compensationDelay, 0, kMaxCompensationSamples - 1);

    if (delay != slot.currentDelay)
    {
        slot.delay.clear();
        slot.delayWrite = 0;
        slot.currentDelay = delay;
    }

    if (delay == 0)
        return;

    const int numSamples = currentNumSamples_;
    const int capacity = kMaxCompensationSamples;

    for (int ch = 0; ch < std::min(track.outputLayout.numChannels, slot.delay.getNumChannels()); ++ch)
    {
        Sample* signal = slot.main.getWritePointer(ch);
        Sample* ring = slot.delay.getWritePointer(ch);
        int write = slot.delayWrite;

        for (int i = 0; i < numSamples; ++i)
        {
            ring[write] = signal[i];
            signal[i] = ring[(write - delay + capacity) % capacity];
            write = (write + 1) % capacity;
        }
    }

    slot.delayWrite = (slot.delayWrite + numSamples) % capacity;
}

//==================================================================================================
// Master

void AudioEngine::renderMaster(int numSamples) noexcept
{
    // Tracks routed to the master deliver its channels (their faders convert).
    const int numMaster = masterChannels();
    for (const int source : graph_->masterSources)
    {
        const auto& sourceSlot = slots_[static_cast<std::size_t>(graph_->tracks[static_cast<std::size_t>(source)].slot)];
        for (int ch = 0; ch < std::min(numMaster, sourceSlot.main.getNumChannels()); ++ch)
            masterBus_.addFrom(ch, 0, sourceSlot.main, ch, 0, numSamples);
    }

    applyParameterAutomation(graph_->masterParameterLanes);
    masterMidi_.clear();
    runChain(graph_->masterDevices, masterBus_, numMaster, graph_->masterGroups, masterMidi_, &masterScratch_);
}

void AudioEngine::captureInputs(int /*inputOffset*/, int numSamples) noexcept
{
    for (const auto& target : capture_->inputs)
    {
        if (target.stream == nullptr || target.firstChannel >= numInputs_)
            continue;

        std::array<const Sample*, kMaxChannels> channels{};
        const int count = std::clamp(target.numChannels, 1, kMaxChannels);
        for (int ch = 0; ch < count; ++ch)
            channels[static_cast<std::size_t>(ch)] = inputBus_.getReadPointer(std::min(target.firstChannel + ch, numInputs_ - 1));
        target.stream->write(channels.data(), count, numSamples);
    }
}

void AudioEngine::writeOutputsAndMeter(float* const* outputs, int numOutputs, int offset, int numSamples) noexcept
{
    const int numMaster = masterChannels();
    std::array<Sample, kMaxChannels> peaks{};
    std::uint64_t clipped = 0;

    for (int ch = 0; ch < numMaster; ++ch)
    {
        const Sample* channel = masterBus_.getReadPointer(ch);
        Sample peak = 0.0;
        for (int i = 0; i < numSamples; ++i)
        {
            const Sample v = std::abs(channel[i]);
            peak = std::max(peak, v);
            clipped += v > 1.0 ? 1u : 0u;
        }
        peaks[static_cast<std::size_t>(ch)] = peak;
    }

    const auto silence = [&](int ch)
    {
        if (outputs[ch] != nullptr)
            std::fill_n(outputs[ch] + offset, numSamples, 0.0f);
    };

    if (numOutputs >= numMaster)
    {
        // Every master channel has its own output, in the master's channel order.
        for (int ch = 0; ch < numOutputs; ++ch)
        {
            if (outputs[ch] == nullptr)
                continue;
            if (ch >= numMaster)
            {
                silence(ch);
                continue;
            }
            const Sample* source = masterBus_.getReadPointer(ch);
            for (int i = 0; i < numSamples; ++i)
                outputs[ch][offset + i] = static_cast<IoSample>(source[i]);
        }
    }
    else
    {
        // Fewer outputs than master channels: a stereo fold-down (and its mono sum on one output).
        const bool stereoMaster = numMaster <= 2;
        const auto& fold = graph_ != nullptr ? graph_->masterFoldDown : ChannelMatrix{};
        for (int ch = 0; ch < numOutputs; ++ch)
        {
            if (outputs[ch] == nullptr)
                continue;
            if (ch >= 2)
            {
                silence(ch);
                continue;
            }
            float* destination = outputs[ch] + offset;
            for (int i = 0; i < numSamples; ++i)
            {
                double left = 0.0;
                double right = 0.0;
                if (stereoMaster)
                {
                    left = masterBus_.getSample(0, i);
                    right = masterBus_.getSample(std::min(1, numMaster - 1), i);
                }
                else
                {
                    for (int m = 0; m < std::min(numMaster, fold.numInputs); ++m)
                    {
                        const Sample v = masterBus_.getSample(m, i);
                        left += fold.gain[0][static_cast<std::size_t>(m)] * v;
                        right += fold.gain[1][static_cast<std::size_t>(m)] * v;
                    }
                }
                destination[i] = static_cast<IoSample>(numOutputs == 1 ? 0.5 * (left + right) : (ch == 0 ? left : right));
            }
        }
    }

    rt::atomicStoreMax(telemetry_.masterPeakLeft, static_cast<float>(peaks[0]));
    rt::atomicStoreMax(telemetry_.masterPeakRight, static_cast<float>(numChannels_ == 1 || numMaster < 2 ? peaks[0] : peaks[1]));
    for (int ch = 0; ch < numMaster; ++ch)
        rt::atomicStoreMax(telemetry_.masterPeaks[static_cast<std::size_t>(ch)], static_cast<float>(peaks[static_cast<std::size_t>(ch)]));
    telemetry_.masterChannels.store(numMaster, std::memory_order_relaxed);

    if (clipped > 0)
        telemetry_.clippedSamples.fetch_add(clipped, std::memory_order_relaxed);
}

void AudioEngine::publishLoad(Clock::duration elapsed, int numSamples) noexcept
{
    const double periodSeconds = static_cast<double>(numSamples) / sampleRate_;
    const double elapsedSeconds = std::chrono::duration<double>(elapsed).count();
    const double instantaneousLoad = elapsedSeconds / periodSeconds;

    smoothedLoad_ += kLoadSmoothingCoefficient * (instantaneousLoad - smoothedLoad_);

    telemetry_.dspLoad.store(static_cast<float>(smoothedLoad_), std::memory_order_relaxed);
    rt::atomicStoreMax(telemetry_.dspLoadPeak, static_cast<float>(instantaneousLoad));
}

void AudioEngine::updatePlayHead() noexcept
{
    const double tempo = graph_ != nullptr ? graph_->tempo : 120.0;
    const double beatsPerSecond = tempo / 60.0;
    const double ppq = positionSeconds_ * beatsPerSecond;

    juce::AudioPlayHead::PositionInfo info;
    info.setBpm(tempo);
    info.setTimeSignature(juce::AudioPlayHead::TimeSignature{4, 4});
    info.setTimeInSeconds(positionSeconds_);
    info.setTimeInSamples(static_cast<juce::int64>(std::llround(positionSeconds_ * sampleRate_)));
    info.setPpqPosition(ppq);
    info.setPpqPositionOfLastBarStart(std::floor(ppq / 4.0) * 4.0);
    info.setIsPlaying(playing_);
    info.setIsRecording(capture_ != nullptr && (!capture_->inputs.empty() || capture_->captureMidi));
    info.setIsLooping(loopEnabled_);
    info.setLoopPoints(juce::AudioPlayHead::LoopPoints{loopStart_ * beatsPerSecond, loopEnd_ * beatsPerSecond});
    playHead_.info = info;
}

} // namespace dojo::engine
