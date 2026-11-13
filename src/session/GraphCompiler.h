#pragma once

#include "engine/RenderGraph.h"
#include "session/AudioAssetCache.h"
#include "session/DeviceManager.h"
#include "session/Session.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <map>
#include <memory>

namespace dojo::engine
{
class ClipStretcher;
}

namespace dojo::session
{

/**
    Translates the session document into an engine::RenderGraph.

    Resolves everything the audio thread should not have to think about:
    - routing: folders, output assignments to busses, sends, multi-out aux returns and sidechains,
      with feedback loops refused; tracks are grouped into dependency levels for parallel rendering;
    - mute/solo across the routing tree (soloing a track keeps the busses it feeds audible, soloing a
      bus keeps everything feeding it audible);
    - plug-in delay compensation: every signal path meeting at a bus or the master is delayed to the
      latest one;
    - automation lanes, dB to linear, crossfades between overlapping clips (equal power), clip
      time-stretch / pitch (ClipStretcher instances survive rebuilds), global speed and transposition;
    - which decoded or disk-streamed audio each clip plays, and which live device instance each
      device is.
    Assigns each track a stable engine "slot" (index into the engine's per-track state and meters).
*/
class GraphBuilder final
{
public:
    /** Builds a graph for the current session. Clips whose audio is not loaded yet are skipped, and
        loading is requested for them. `devices` may be null (no device chains). */
    std::unique_ptr<engine::RenderGraph> build(const Session& session, AudioAssetCache& assets,
                                               DeviceManager* devices);

    /** The engine slot of a track (-1 if the track has not been compiled yet). */
    [[nodiscard]] int getSlot(std::uint32_t trackId) const;

    /** Sample rate and maximum block size the engine runs at (for clip stretchers). */
    void setRenderFormat(double sampleRate, int maximumBlockSize);

    /** Latency of the master output in samples, as of the last build. */
    [[nodiscard]] int getTotalLatency() const noexcept { return totalLatency_; }
    /** Latency at the master bus input (where all track outputs are aligned), as of the last build. */
    [[nodiscard]] int getMasterInputLatency() const noexcept { return masterInputLatency_; }
    /** How late a track's output (after its compensation delay) is relative to the timeline, as of
        the last build: where it meets the rest of what its destination receives. -1 if unknown. */
    [[nodiscard]] int getOutputLatency(std::uint32_t trackId) const
    {
        const auto it = outputLatencies_.find(trackId);
        return it != outputLatencies_.end() ? it->second : -1;
    }

    /** Balance pan law used for every track. Returns {left, right}. */
    static std::pair<double, double> panGains(double pan) noexcept { return engine::balancePanGains(pan); }

private:
    /** A stable slot for a track; `wide` tracks (multichannel signals) get one of the wide slots. */
    int assignSlot(std::uint32_t trackId, std::vector<bool>& used, bool wide);
    std::shared_ptr<engine::ClipStretcher> getStretcher(std::uint32_t clipId, int numChannels);

    std::map<std::uint32_t, int> slots_;
    std::map<std::uint32_t, std::shared_ptr<engine::ClipStretcher>> stretchers_;
    double sampleRate_ = 48000.0;
    int maxBlock_ = 2048;
    int totalLatency_ = 0;
    int masterInputLatency_ = 0;
    std::map<std::uint32_t, int> outputLatencies_;
    /** Alignment delay lines of sends (by SEND id) and of aux inputs (by bus track id), kept for the
        next build so their signal carries on through rebuilds. */
    std::map<std::uint32_t, std::shared_ptr<engine::InputDelayLine>> sendDelays_;
    std::map<std::uint32_t, std::shared_ptr<engine::InputDelayLine>> auxDelays_;
};

/**
    Keeps the engine in sync with the session: every change to the document (or a newly loaded
    audio file) schedules one rebuild, coalesced to at most one per message-loop iteration, and the
    fresh graph is handed to `publish` (normally EngineController::publishGraph).
*/
class GraphCompiler final : private juce::ValueTree::Listener,
                            private juce::AsyncUpdater,
                            private AudioAssetCache::Listener
{
public:
    using Publisher = std::function<void(std::unique_ptr<engine::RenderGraph>)>;

    GraphCompiler(Session& session, AudioAssetCache& assets, DeviceManager* devices, Publisher publish);
    ~GraphCompiler() override;

    /** Rebuilds and publishes immediately (instead of on the next message-loop iteration). */
    void rebuildNow();
    /** Schedules a rebuild (e.g. after a device's latency changed). */
    void rebuildSoon() { triggerAsyncUpdate(); }

    /** Sample rate / block size of the engine (message thread); triggers a rebuild. */
    void setRenderFormat(double sampleRate, int maximumBlockSize);

    [[nodiscard]] int getSlot(std::uint32_t trackId) const { return builder_.getSlot(trackId); }
    [[nodiscard]] int getTotalLatency() const noexcept { return builder_.getTotalLatency(); }
    [[nodiscard]] int getMasterInputLatency() const noexcept { return builder_.getMasterInputLatency(); }
    [[nodiscard]] int getOutputLatency(std::uint32_t trackId) const { return builder_.getOutputLatency(trackId); }

    /** Builds a graph without publishing it (for offline renders on a separate engine). */
    std::unique_ptr<engine::RenderGraph> buildGraph() { return builder_.build(session_, assets_, devices_); }

private:
    void handleAsyncUpdate() override;
    void audioAssetLoaded(const juce::File&) override { triggerAsyncUpdate(); }

    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property) override;
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override { triggerAsyncUpdate(); }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override { triggerAsyncUpdate(); }
    void valueTreeChildOrderChanged(juce::ValueTree&, int, int) override { triggerAsyncUpdate(); }
    void valueTreeRedirected(juce::ValueTree&) override { triggerAsyncUpdate(); }

    Session& session_;
    AudioAssetCache& assets_;
    DeviceManager* devices_;
    Publisher publish_;
    GraphBuilder builder_;
};

} // namespace dojo::session
