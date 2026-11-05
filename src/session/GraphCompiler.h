#pragma once

#include "engine/RenderGraph.h"
#include "session/AudioAssetCache.h"
#include "session/Session.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <map>
#include <memory>

namespace dojo::session
{

/**
    Translates the session document into an engine::RenderGraph.

    Resolves everything the audio thread should not have to think about: mute/solo logic, dB to
    linear, pan law, phase inversion, clip ordering, and which decoded audio each clip plays. Also
    assigns each track a stable engine "slot" (index into the engine's per-track state and meters).
*/
class GraphBuilder final
{
public:
    /** Builds a graph for the current session. Clips whose audio is not decoded yet are skipped,
        and decoding is requested for them. */
    std::unique_ptr<engine::RenderGraph> build(const Session& session, AudioAssetCache& assets);

    /** The engine slot of a track (-1 if the track has not been compiled yet). */
    [[nodiscard]] int getSlot(std::uint32_t trackId) const;

    /** Balance pan law used for every track: centre is unity on both sides; panning attenuates the
        opposite side linearly. Returns {left, right}. */
    static std::pair<double, double> panGains(double pan) noexcept;

private:
    int assignSlot(std::uint32_t trackId, std::vector<bool>& used);

    std::map<std::uint32_t, int> slots_;
};

/**
    Keeps the engine in sync with the session: every change to the document (or a newly decoded
    audio file) schedules one rebuild, coalesced to at most one per message-loop iteration, and the
    fresh graph is handed to `publish` (normally EngineController::publishGraph).
*/
class GraphCompiler final : private juce::ValueTree::Listener,
                            private juce::AsyncUpdater,
                            private AudioAssetCache::Listener
{
public:
    using Publisher = std::function<void(std::unique_ptr<engine::RenderGraph>)>;

    GraphCompiler(Session& session, AudioAssetCache& assets, Publisher publish);
    ~GraphCompiler() override;

    /** Rebuilds and publishes immediately (instead of on the next message-loop iteration). */
    void rebuildNow();

    [[nodiscard]] int getSlot(std::uint32_t trackId) const { return builder_.getSlot(trackId); }

private:
    void handleAsyncUpdate() override;
    void audioAssetLoaded(const juce::File&) override { triggerAsyncUpdate(); }

    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override
    {
        triggerAsyncUpdate();
    }
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override { triggerAsyncUpdate(); }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override { triggerAsyncUpdate(); }
    void valueTreeChildOrderChanged(juce::ValueTree&, int, int) override { triggerAsyncUpdate(); }
    void valueTreeRedirected(juce::ValueTree&) override { triggerAsyncUpdate(); }

    Session& session_;
    AudioAssetCache& assets_;
    Publisher publish_;
    GraphBuilder builder_;
};

} // namespace dojo::session
