#include "session/GraphCompiler.h"

#include "engine/dsp/ClipStretcher.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace dojo::session
{
namespace
{
double dbToGain(double db) { return juce::Decibels::decibelsToGain(db, kMinusInfinityDb); }

/** Sum of the latencies of the active devices of a chain. */
int chainLatency(const std::vector<engine::RenderDevice>& devices)
{
    int latency = 0;
    for (const auto& d : devices)
        if (d.node != nullptr && !d.node->isBypassed())
            latency += std::max(0, d.node->getLatencySamples());
    return latency;
}

/** Directed graph of "a feeds b" edges over graph indices, refusing edges that would close a loop. */
class RoutingGraph
{
public:
    explicit RoutingGraph(std::size_t size)
        : feeds_(size)
    {
    }

    bool tryAdd(int from, int to)
    {
        if (from == to || reaches(to, from))
            return false;
        feeds_[static_cast<std::size_t>(from)].push_back(to);
        return true;
    }

private:
    [[nodiscard]] bool reaches(int from, int target) const
    {
        std::vector<bool> seen(feeds_.size(), false);
        std::vector<int> stack{from};
        while (!stack.empty())
        {
            const int node = stack.back();
            stack.pop_back();
            if (node == target)
                return true;
            if (seen[static_cast<std::size_t>(node)])
                continue;
            seen[static_cast<std::size_t>(node)] = true;
            for (const int next : feeds_[static_cast<std::size_t>(node)])
                stack.push_back(next);
        }
        return false;
    }

    std::vector<std::vector<int>> feeds_;
};

void applyCrossfades(std::vector<engine::RenderClip>& clips)
{
    for (std::size_t i = 0; i + 1 < clips.size(); ++i)
    {
        auto& a = clips[i];
        auto& b = clips[i + 1];
        const double overlap = std::min(a.endSeconds, b.endSeconds) - b.startSeconds;
        if (overlap <= 0.0)
            continue;

        if (a.fadeOutSeconds < overlap)
        {
            a.fadeOutSeconds = overlap;
            a.fadeOutEqualPower = true;
        }
        if (b.fadeInSeconds < overlap)
        {
            b.fadeInSeconds = overlap;
            b.fadeInEqualPower = true;
        }
    }
}
} // namespace

//==================================================================================================
// GraphBuilder

int GraphBuilder::getSlot(std::uint32_t trackId) const
{
    const auto it = slots_.find(trackId);
    return it != slots_.end() ? it->second : -1;
}

void GraphBuilder::setRenderFormat(double sampleRate, int maximumBlockSize)
{
    if (sampleRate > 0.0)
        sampleRate_ = sampleRate;
    if (maximumBlockSize > 0)
        maxBlock_ = std::max(maximumBlockSize, 64);
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

std::shared_ptr<engine::ClipStretcher> GraphBuilder::getStretcher(std::uint32_t clipId, int numChannels)
{
    auto& stretcher = stretchers_[clipId];
    if (stretcher == nullptr || std::abs(stretcher->getConfiguredSampleRate() - sampleRate_) > 1.0e-6 ||
        stretcher->getMaximumBlockSize() < maxBlock_)
        stretcher = std::make_shared<engine::ClipStretcher>(numChannels, sampleRate_, maxBlock_);
    return stretcher;
}

std::unique_ptr<engine::RenderGraph> GraphBuilder::build(const Session& session, AudioAssetCache& assets,
                                                         DeviceManager* devices)
{
    using engine::TrackKind;

    // Inactive tracks (and everything inside an inactive folder) are left out entirely: they are not
    // rendered and their devices are not run.
    std::vector<juce::ValueTree> allTracks;
    for (const auto& t : session.getAllTracks())
        if (Session::isEffectivelyActive(t))
            allTracks.push_back(t);

    // --- Slots: forget tracks that no longer exist, keep the rest stable ----------------------------
    std::set<std::uint32_t> liveIds;
    for (const auto& t : allTracks)
        liveIds.insert(track::getId(t));

    std::vector<bool> used(static_cast<std::size_t>(engine::kMaxTracks), false);
    for (auto it = slots_.begin(); it != slots_.end();)
    {
        if (liveIds.count(it->first) == 0)
        {
            it = slots_.erase(it);
        }
        else
        {
            used[static_cast<std::size_t>(it->second)] = true;
            ++it;
        }
    }

    // --- Graph indices --------------------------------------------------------------------------
    std::vector<juce::ValueTree> trees;
    std::map<std::uint32_t, int> indexOfId;
    std::vector<int> slotOf;
    for (const auto& t : allTracks)
    {
        const int slot = assignSlot(track::getId(t), used);
        if (slot < 0)
            continue;
        indexOfId[track::getId(t)] = static_cast<int>(trees.size());
        trees.push_back(t);
        slotOf.push_back(slot);
    }

    const auto count = trees.size();
    const auto indexOf = [&](std::uint32_t id)
    {
        const auto it = indexOfId.find(id);
        return it != indexOfId.end() ? it->second : -1;
    };

    auto graph = std::make_unique<engine::RenderGraph>();
    graph->tempo = session.getTempo();
    graph->speed = session.getSpeed();
    graph->loopEnabled = session.isLoopEnabled();
    graph->loopStart = session.getLoopStart();
    graph->loopEnd = session.getLoopEnd();
    graph->hasTransportSettings = true;
    graph->arrangementEnd = session.getEndTime();
    graph->masterGain = dbToGain(session.getMasterVolumeDb());
    graph->tracks.resize(count);

    const double globalTranspose = session.getTranspose();
    const double speed = session.getSpeed();
    // With "keep pitch", varispeed must not change the pitch: transpose audio back down.
    const double speedCompensation = session.getKeepPitch() ? -12.0 * std::log2(speed) : 0.0;

    // --- Device chains (needed first: sidechains are routing edges) ------------------------------
    std::vector<std::vector<std::pair<juce::ValueTree, std::shared_ptr<engine::ProcessorNode>>>> chainNodes(count);
    if (devices != nullptr)
    {
        for (std::size_t i = 0; i < count; ++i)
            for (const auto& d : Session::getDevices(trees[i]))
                if (auto node = devices->getNode(d))
                    chainNodes[i].emplace_back(d, std::move(node));
    }

    // --- Routing edges, loops refused --------------------------------------------------------------
    RoutingGraph routing(count);
    std::vector<int> destination(count, -1); // -1 = master
    std::vector<std::vector<int>> busSources(count);
    std::vector<int> auxSource(count, -1);
    std::vector<std::vector<std::pair<int, juce::ValueTree>>> incomingSends(count); // (source, SEND tree)
    std::vector<std::vector<int>> sidechainOf(count); // per device, the source index (-1 = none)

    for (std::size_t i = 0; i < count; ++i)
    {
        const auto dest = session.getDestination(trees[i]);
        int target = dest.isValid() ? indexOf(track::getId(dest)) : -1;
        if (dest.isValid() && target < 0)
        {
            destination[i] = -2; // routed to an inactive bus: its output goes nowhere
            continue;
        }
        if (target >= 0 && !routing.tryAdd(static_cast<int>(i), target))
        {
            // The assignment would close a loop: fall back to the folder, then to the master.
            const auto folderTree = Session::getParentFolder(trees[i]);
            target = folderTree.isValid() ? indexOf(track::getId(folderTree)) : -1;
            if (target >= 0 && !routing.tryAdd(static_cast<int>(i), target))
                target = -1;
        }
        destination[i] = target;
        if (target >= 0)
            busSources[static_cast<std::size_t>(target)].push_back(static_cast<int>(i));
    }

    for (std::size_t i = 0; i < count; ++i)
    {
        if (track::getKind(trees[i]) != TrackKind::bus)
            continue;
        const int source = indexOf(track::getAuxSource(trees[i]));
        if (source >= 0 && track::getKind(trees[static_cast<std::size_t>(source)]) == TrackKind::instrument &&
            routing.tryAdd(source, static_cast<int>(i)))
            auxSource[i] = source;
    }

    for (std::size_t i = 0; i < count; ++i)
    {
        for (const auto& s : Session::getSends(trees[i]))
        {
            const int target = indexOf(send::getTarget(s));
            if (target < 0 || track::getKind(trees[static_cast<std::size_t>(target)]) != TrackKind::bus)
                continue;
            if (routing.tryAdd(static_cast<int>(i), target))
                incomingSends[static_cast<std::size_t>(target)].emplace_back(static_cast<int>(i), s);
        }
    }

    for (std::size_t i = 0; i < count; ++i)
    {
        for (const auto& [deviceTree, node] : chainNodes[i])
        {
            int source = -1;
            if (const auto id = device::getSidechain(deviceTree); id != 0 && node->hasSidechainInput())
            {
                source = indexOf(id);
                if (source >= 0 && !routing.tryAdd(source, static_cast<int>(i)))
                    source = -1;
            }
            sidechainOf[i].push_back(source);
        }
    }

    // --- Dependency levels -------------------------------------------------------------------------
    std::vector<std::vector<int>> dependsOn(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        auto& deps = dependsOn[i];
        deps.insert(deps.end(), busSources[i].begin(), busSources[i].end());
        for (const auto& [source, s] : incomingSends[i])
            deps.push_back(source);
        if (auxSource[i] >= 0)
            deps.push_back(auxSource[i]);
        for (const int source : sidechainOf[i])
            if (source >= 0)
                deps.push_back(source);
    }

    std::vector<int> level(count, -1);
    const std::function<int(int)> levelOf = [&](int index) -> int
    {
        auto& l = level[static_cast<std::size_t>(index)];
        if (l >= 0)
            return l;
        l = 0; // edges are acyclic (enforced above), this just guards against surprises
        int result = 0;
        for (const int dep : dependsOn[static_cast<std::size_t>(index)])
            result = std::max(result, levelOf(dep) + 1);
        l = result;
        return result;
    };

    int numLevels = 0;
    for (std::size_t i = 0; i < count; ++i)
        numLevels = std::max(numLevels, levelOf(static_cast<int>(i)) + 1);

    graph->levels.resize(static_cast<std::size_t>(numLevels));
    for (std::size_t i = 0; i < count; ++i)
        graph->levels[static_cast<std::size_t>(level[i])].push_back(static_cast<int>(i));

    // --- Mute / solo across the routing tree ------------------------------------------------------
    bool anySoloed = false;
    for (const auto& t : trees)
        anySoloed = anySoloed || track::isSoloed(t);

    std::vector<bool> soloedUpstream(count, false);   // this track or something feeding it is soloed
    std::vector<bool> soloedDownstream(count, false); // this track or something it feeds is soloed
    if (anySoloed)
    {
        // Levels give a topological order: sources before the busses they feed.
        for (const auto& lvl : graph->levels)
            for (const int i : lvl)
            {
                bool up = track::isSoloed(trees[static_cast<std::size_t>(i)]);
                for (const int dep : dependsOn[static_cast<std::size_t>(i)])
                    up = up || soloedUpstream[static_cast<std::size_t>(dep)];
                soloedUpstream[static_cast<std::size_t>(i)] = up;
            }

        for (auto lvl = graph->levels.rbegin(); lvl != graph->levels.rend(); ++lvl)
            for (const int i : *lvl)
            {
                const auto ui = static_cast<std::size_t>(i);
                bool down = track::isSoloed(trees[ui]);
                if (destination[ui] >= 0)
                    down = down || soloedDownstream[static_cast<std::size_t>(destination[ui])];
                for (std::size_t b = 0; b < count; ++b)
                    for (const auto& [source, s] : incomingSends[b])
                        if (source == i)
                            down = down || soloedDownstream[b];
                soloedDownstream[ui] = down;
            }
    }

    // --- Tracks -------------------------------------------------------------------------------------
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto& t = trees[i];
        auto& rt = graph->tracks[i];

        rt.trackId = track::getId(t);
        rt.slot = slotOf[i];
        rt.kind = track::getKind(t);
        rt.mono = rt.kind != TrackKind::instrument && track::getFormat(t) == TrackFormat::mono;
        rt.volume = dbToGain(track::getVolumeDb(t));
        rt.pan = track::getPan(t);
        rt.audible = !track::isMuted(t) && (!anySoloed || soloedUpstream[i] || soloedDownstream[i]);
        rt.phaseInverted = track::isPhaseInverted(t);
        rt.outputIndex = destination[i];

        // Devices.
        for (std::size_t d = 0; d < chainNodes[i].size(); ++d)
        {
            engine::RenderDevice device;
            device.node = chainNodes[i][d].second.get();
            device.sidechainSource = sidechainOf[i][d];
            rt.devices.push_back(device);
            graph->keepAlive.push_back(chainNodes[i][d].second);
        }

        // Automation.
        for (const auto& lane : Session::getAutomationLanes(t))
        {
            const auto target = automation::getTarget(lane);
            std::uint32_t id = 0;
            int parameterIndex = 0;

            if (target == automation::volumeTarget())
            {
                rt.volumeAutomation = automation::toCurve(lane);
            }
            else if (target == automation::panTarget())
            {
                rt.panAutomation = automation::toCurve(lane);
            }
            else if (automation::parseParameterTarget(target, id, parameterIndex))
            {
                for (const auto& [deviceTree, node] : chainNodes[i])
                {
                    if (device::getId(deviceTree) != id)
                        continue;
                    const auto& parameters = node->getProcessor().getParameters();
                    if (parameterIndex < parameters.size())
                        rt.parameterLanes.push_back({parameters[parameterIndex], automation::toCurve(lane)});
                }
            }
        }

        // Sources.
        const double trackPitch = track::getPitch(t);
        const double trackSpeed = std::max(kMinStretch, track::getSpeed(t));

        if (rt.kind == TrackKind::audio)
        {
            for (const auto& c : t)
            {
                if (!c.hasType(ids::clip))
                    continue;

                const auto file = clip::getFile(c);
                const auto asset = assets.find(file);

                if (asset == nullptr)
                {
                    if (!assets.isLoading(file))
                        assets.request(file, {});
                    continue;
                }

                engine::RenderClip rc;
                rc.startSeconds = clip::getStart(c);
                rc.endSeconds = clip::getEnd(c);
                rc.sourceOffsetSeconds = clip::getOffset(c);
                rc.fadeInSeconds = clip::getFadeIn(c);
                rc.fadeOutSeconds = clip::getFadeOut(c);
                rc.gain = dbToGain(clip::getGainDb(c));
                rc.stretch = std::max(kMinStretch, clip::getStretch(c)) / trackSpeed;
                rc.pitchSemitones = clip::getPitch(c) + trackPitch + globalTranspose + speedCompensation;
                rc.numChannels = asset->numChannels;
                rc.sourceChannel = clip::getSourceChannel(c);
                rc.sourceLength = asset->numFrames;
                rc.sourceSampleRate = asset->sampleRate;

                if (asset->isStreamed())
                    rc.streamed = asset->streamed.get();
                else
                    rc.channels = asset->audio.getArrayOfReadPointers();

                // Time-stretch (duration without pitch) and pitch-shift go through a persistent
                // stretcher. Varispeed alone is plain resampling in the clip renderer.
                if (std::abs(rc.pitchSemitones) > 1.0e-6 || std::abs(rc.stretch - 1.0) > 1.0e-9)
                {
                    auto stretcher = getStretcher(clip::getId(c), rc.numChannels);
                    rc.stretcher = stretcher.get();
                    graph->keepAlive.push_back(stretcher);
                }

                rt.clips.push_back(rc);
                graph->keepAlive.push_back(asset);
            }

            std::sort(rt.clips.begin(), rt.clips.end(),
                      [](const auto& a, const auto& b) { return a.startSeconds < b.startSeconds; });
            applyCrossfades(rt.clips);

            rt.inputChannel = track::getInput(t);
            rt.inputStereo = track::isInputStereo(t);
            rt.monitor = track::getMonitor(t);
            rt.armed = track::isArmed(t);
        }
        else if (rt.kind == TrackKind::instrument)
        {
            const int transpose = juce::roundToInt(globalTranspose + trackPitch);

            for (const auto& c : t)
            {
                if (!c.hasType(ids::midiClip))
                    continue;

                engine::RenderMidiClip mc;
                mc.startSeconds = clip::getStart(c);
                mc.endSeconds = clip::getEnd(c);
                mc.contentStartBeat = clip::getOffset(c);
                mc.rate = trackSpeed;

                for (const auto& n : c)
                {
                    if (n.hasType(ids::note))
                    {
                        engine::RenderNote rn;
                        rn.startBeat = note::getStart(n);
                        rn.endBeat = note::getEnd(n);
                        rn.note = juce::jlimit(0, 127, note::getNote(n) + transpose);
                        rn.velocity = note::getVelocity(n);
                        rn.channel = note::getChannel(n);
                        mc.notes.push_back(rn);
                    }
                    else if (n.hasType(ids::control))
                    {
                        engine::RenderControl control;
                        control.beat = n.getProperty(ids::beat, 0.0);
                        control.controller = n.getProperty(ids::controller, 1);
                        control.value = static_cast<float>(static_cast<double>(n.getProperty(ids::value, 0.0)));
                        control.channel = n.getProperty(ids::channel, 1);
                        mc.controls.push_back(control);
                    }
                }

                std::sort(mc.notes.begin(), mc.notes.end(),
                          [](const auto& a, const auto& b) { return a.startBeat < b.startBeat; });
                std::sort(mc.controls.begin(), mc.controls.end(),
                          [](const auto& a, const auto& b) { return a.beat < b.beat; });
                rt.midiClips.push_back(std::move(mc));
            }

            std::sort(rt.midiClips.begin(), rt.midiClips.end(),
                      [](const auto& a, const auto& b) { return a.startSeconds < b.startSeconds; });

            rt.monitor = track::getMonitor(t);
            rt.armed = track::isArmed(t);
            rt.receivesLiveMidi = rt.armed || rt.monitor == engine::MonitorMode::always;
        }
        else // bus
        {
            rt.busSources = busSources[i];
            rt.auxSourceIndex = auxSource[i];
            rt.auxChannel = track::getAuxChannel(t);

            for (const auto& [source, s] : incomingSends[i])
            {
                engine::RenderSend rs;
                rs.sourceIndex = source;
                rs.gain = dbToGain(send::getGainDb(s));
                rs.preFader = send::isPreFader(s);

                const auto sendTarget = automation::sendTarget(send::getId(s));
                for (const auto& lane : Session::getAutomationLanes(trees[static_cast<std::size_t>(source)]))
                    if (automation::getTarget(lane) == sendTarget)
                        rs.gainAutomation = automation::toCurve(lane);

                if (rs.preFader)
                    graph->tracks[static_cast<std::size_t>(source)].needsPreFaderCopy = true;

                rt.sends.push_back(std::move(rs));
            }
        }
    }

    // Pre-fader flags were possibly set on tracks built earlier; make sure they stick for later ones.
    for (std::size_t b = 0; b < count; ++b)
        for (const auto& [source, s] : incomingSends[b])
            if (send::isPreFader(s))
                graph->tracks[static_cast<std::size_t>(source)].needsPreFaderCopy = true;

    // --- Master ---------------------------------------------------------------------------------------
    for (std::size_t i = 0; i < count; ++i)
        if (destination[i] == -1)
            graph->masterSources.push_back(static_cast<int>(i));

    const auto masterTree = session.getMaster();
    if (devices != nullptr && masterTree.isValid())
    {
        for (const auto& d : Session::getDevices(masterTree))
        {
            auto node = devices->getNode(d);
            if (node == nullptr)
                continue;

            engine::RenderDevice device;
            device.node = node.get();
            if (const auto id = device::getSidechain(d); id != 0 && node->hasSidechainInput())
                device.sidechainSource = indexOf(id);
            graph->masterDevices.push_back(device);
            graph->keepAlive.push_back(node);
        }
    }

    if (masterTree.isValid())
    {
        for (const auto& lane : Session::getAutomationLanes(masterTree))
        {
            const auto target = automation::getTarget(lane);
            std::uint32_t id = 0;
            int parameterIndex = 0;

            if (target == automation::volumeTarget())
            {
                graph->masterVolumeAutomation = automation::toCurve(lane);
            }
            else if (automation::parseParameterTarget(target, id, parameterIndex))
            {
                for (const auto& device : graph->masterDevices)
                {
                    const auto deviceTree = session.findDevice(id);
                    if (!deviceTree.isValid() || devices == nullptr || devices->findNode(id).get() != device.node)
                        continue;
                    const auto& parameters = device.node->getProcessor().getParameters();
                    if (parameterIndex < parameters.size())
                        graph->masterParameterLanes.push_back({parameters[parameterIndex], automation::toCurve(lane)});
                }
            }
        }
    }

    // --- Plug-in delay compensation ---------------------------------------------------------------
    // arrival[i]: latency of track i's output relative to the timeline (before its compensation).
    // Iterated to a fixed point so that every path meeting at a bus arrives at the same time.
    std::vector<int> arrival(count, 0);
    std::vector<int> compensation(count, 0);

    for (int iteration = 0; iteration < 16; ++iteration)
    {
        for (const auto& lvl : graph->levels)
        {
            for (const int idx : lvl)
            {
                const auto i = static_cast<std::size_t>(idx);
                int input = 0;
                for (const int source : busSources[i])
                    input = std::max(input, arrival[static_cast<std::size_t>(source)] + compensation[static_cast<std::size_t>(source)]);
                for (const auto& [source, s] : incomingSends[i])
                    input = std::max(input, arrival[static_cast<std::size_t>(source)] +
                                                (send::isPreFader(s) ? 0 : compensation[static_cast<std::size_t>(source)]));
                if (auxSource[i] >= 0)
                {
                    // Aux outputs come straight from the instrument (the first device of its track).
                    const auto& instrumentChain = graph->tracks[static_cast<std::size_t>(auxSource[i])].devices;
                    if (!instrumentChain.empty())
                        input = std::max(input, std::max(0, instrumentChain.front().node->getLatencySamples()));
                }
                arrival[i] = input + chainLatency(graph->tracks[i].devices);
            }
        }

        // Each destination waits for its latest input; each source is delayed to match its latest
        // destination.
        std::vector<int> target(count, 0);
        int masterTarget = 0;
        for (std::size_t i = 0; i < count; ++i)
        {
            const int out = arrival[i] + compensation[i];
            if (destination[i] >= 0)
                target[static_cast<std::size_t>(destination[i])] = std::max(target[static_cast<std::size_t>(destination[i])], out);
            else if (destination[i] == -1) // (-2: silenced, it reaches nothing)
                masterTarget = std::max(masterTarget, out);
        }
        for (std::size_t b = 0; b < count; ++b)
            for (const auto& [source, s] : incomingSends[b])
                if (!send::isPreFader(s))
                    target[b] = std::max(target[b], arrival[static_cast<std::size_t>(source)] + compensation[static_cast<std::size_t>(source)]);

        bool changed = false;
        for (std::size_t i = 0; i < count; ++i)
        {
            int needed = destination[i] >= 0    ? target[static_cast<std::size_t>(destination[i])] - arrival[i]
                         : destination[i] == -1 ? masterTarget - arrival[i]
                                                : 0;
            for (std::size_t b = 0; b < count; ++b)
                for (const auto& [source, s] : incomingSends[b])
                    if (source == static_cast<int>(i) && !send::isPreFader(s))
                        needed = std::max(needed, target[b] - arrival[i]);

            needed = std::clamp(needed, 0, engine::kMaxCompensationSamples - 1);
            if (needed > compensation[i])
            {
                compensation[i] = needed;
                changed = true;
            }
        }

        graph->totalLatency = masterTarget + chainLatency(graph->masterDevices);
        masterInputLatency_ = masterTarget;
        if (!changed)
            break;
    }

    for (std::size_t i = 0; i < count; ++i)
        graph->tracks[i].compensationDelay = compensation[i];

    totalLatency_ = graph->totalLatency;

    // Forget stretchers of clips that no longer exist.
    std::set<std::uint32_t> liveClips;
    for (const auto& t : trees)
        for (const auto& c : t)
            if (c.hasType(ids::clip))
                liveClips.insert(clip::getId(c));
    for (auto it = stretchers_.begin(); it != stretchers_.end();)
        it = liveClips.count(it->first) == 0 ? stretchers_.erase(it) : std::next(it);

    return graph;
}

//==================================================================================================
// GraphCompiler

GraphCompiler::GraphCompiler(Session& session, AudioAssetCache& assets, DeviceManager* devices, Publisher publish)
    : session_(session)
    , assets_(assets)
    , devices_(devices)
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

void GraphCompiler::setRenderFormat(double sampleRate, int maximumBlockSize)
{
    builder_.setRenderFormat(sampleRate, maximumBlockSize);
    triggerAsyncUpdate();
}

void GraphCompiler::valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier& property)
{
    // Pure UI state and saved device state don't change what is rendered.
    if (property == ids::state || property == ids::collapsed || property == ids::showAutomation ||
        property == ids::visible || property == ids::name || property == ids::colour)
        return;
    triggerAsyncUpdate();
}

void GraphCompiler::handleAsyncUpdate()
{
    if (devices_ != nullptr)
        devices_->purgeUnused();

    if (publish_)
        publish_(builder_.build(session_, assets_, devices_));
}

} // namespace dojo::session
