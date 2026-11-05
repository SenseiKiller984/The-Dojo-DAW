#include "session/GraphCompiler.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>

namespace dojo::session
{

//==================================================================================================
// GraphBuilder

std::pair<double, double> GraphBuilder::panGains(double pan) noexcept
{
    pan = juce::jlimit(-1.0, 1.0, pan);
    return {pan <= 0.0 ? 1.0 : 1.0 - pan, pan >= 0.0 ? 1.0 : 1.0 + pan};
}

int GraphBuilder::getSlot(std::uint32_t trackId) const
{
    const auto it = slots_.find(trackId);
    return it != slots_.end() ? it->second : -1;
}

int GraphBuilder::assignSlot(std::uint32_t trackId, std::vector<bool>& used)
{
    if (const auto it = slots_.find(trackId); it != slots_.end())
        return it->second;

    for (int slot = 0; slot < engine::kMaxTracks; ++slot)
    {
        if (!used[static_cast<std::size_t>(slot)])
        {
            used[static_cast<std::size_t>(slot)] = true;
            slots_[trackId] = slot;
            return slot;
        }
    }

    return -1; // more tracks than the engine renders
}

std::unique_ptr<engine::RenderGraph> GraphBuilder::build(const Session& session, AudioAssetCache& assets)
{
    const auto& state = session.getState();

    // Forget slots of tracks that no longer exist, then mark the remaining ones as used.
    std::vector<bool> used(static_cast<std::size_t>(engine::kMaxTracks), false);
    for (auto it = slots_.begin(); it != slots_.end();)
    {
        if (!session.findTrack(it->first).isValid())
        {
            it = slots_.erase(it);
        }
        else
        {
            used[static_cast<std::size_t>(it->second)] = true;
            ++it;
        }
    }

    bool anySoloed = false;
    for (const auto& t : state)
        anySoloed = anySoloed || track::isSoloed(t);

    auto graph = std::make_unique<engine::RenderGraph>();
    graph->masterGain = juce::Decibels::decibelsToGain(session.getMasterVolumeDb(), kMinusInfinityDb);
    graph->tracks.reserve(static_cast<std::size_t>(state.getNumChildren()));

    for (const auto& t : state)
    {
        const auto trackId = track::getId(t);
        const int slot = assignSlot(trackId, used);
        if (slot < 0)
            continue;

        const bool audible = !track::isMuted(t) && (!anySoloed || track::isSoloed(t));
        const double volume = juce::Decibels::decibelsToGain(track::getVolumeDb(t), kMinusInfinityDb);
        const double sign = track::isPhaseInverted(t) ? -1.0 : 1.0;
        const auto [panLeft, panRight] = panGains(track::getPan(t));

        engine::RenderTrack renderTrack;
        renderTrack.trackId = trackId;
        renderTrack.slot = slot;
        renderTrack.gainLeft = audible ? volume * panLeft * sign : 0.0;
        renderTrack.gainRight = audible ? volume * panRight * sign : 0.0;

        for (const auto& c : t)
        {
            const auto file = clip::getFile(c);
            const auto asset = assets.find(file);

            if (asset == nullptr)
            {
                if (!assets.isLoading(file))
                    assets.request(file, {});
                continue;
            }

            engine::RenderClip renderClip;
            renderClip.startSeconds = clip::getStart(c);
            renderClip.endSeconds = clip::getEnd(c);
            renderClip.sourceOffsetSeconds = clip::getOffset(c);
            renderClip.fadeInSeconds = clip::getFadeIn(c);
            renderClip.fadeOutSeconds = clip::getFadeOut(c);
            renderClip.gain = juce::Decibels::decibelsToGain(clip::getGainDb(c), kMinusInfinityDb);
            renderClip.channels = asset->audio.getArrayOfReadPointers();
            renderClip.numChannels = asset->audio.getNumChannels();
            renderClip.sourceLength = asset->audio.getNumSamples();
            renderClip.sourceSampleRate = asset->sampleRate;
            renderTrack.clips.push_back(renderClip);
            graph->keepAlive.push_back(asset);
        }

        std::sort(renderTrack.clips.begin(), renderTrack.clips.end(),
                  [](const auto& a, const auto& b) { return a.startSeconds < b.startSeconds; });

        graph->tracks.push_back(std::move(renderTrack));
    }

    return graph;
}

//==================================================================================================
// GraphCompiler

GraphCompiler::GraphCompiler(Session& session, AudioAssetCache& assets, Publisher publish)
    : session_(session)
    , assets_(assets)
    , publish_(std::move(publish))
{
    session_.getState().addListener(this);
    assets_.addListener(this);
    triggerAsyncUpdate();
}

GraphCompiler::~GraphCompiler()
{
    cancelPendingUpdate();
    assets_.removeListener(this);
    session_.getState().removeListener(this);
}

void GraphCompiler::rebuildNow()
{
    cancelPendingUpdate();
    handleAsyncUpdate();
}

void GraphCompiler::handleAsyncUpdate()
{
    if (publish_)
        publish_(builder_.build(session_, assets_));
}

} // namespace dojo::session
