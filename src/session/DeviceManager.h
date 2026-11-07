#pragma once

#include "engine/ProcessorNode.h"
#include "session/Session.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

namespace dojo::session
{

/**
    Owns the live processor instance behind every DEVICE in the session — built-in devices and hosted
    plug-ins alike — and keeps them in step with the document:

    - instances are created on demand (getNode) by the `Creator`, restored from the tree's saved
      state, and prepared at the engine's current sample rate and block size;
    - prepareAll() re-prepares everything when the audio device restarts (never while rendering);
    - a removed device's latest state is written back into its (detached) tree, so undoing the
      removal brings it back exactly as it was;
    - storeStates() writes every instance's state into the tree before the session is saved.

    Render graphs hold shared_ptrs to the nodes they use, so a node outlives its removal from the
    session until the audio thread has stopped using it. Message thread only, except prepareAll(),
    which may come from the thread that starts the audio device.
*/
class DeviceManager final : private juce::ValueTree::Listener,
                            private juce::AudioProcessorListener,
                            private juce::Timer
{
public:
    using Creator = std::function<std::unique_ptr<juce::AudioProcessor>(const juce::ValueTree& device, double sampleRate,
                                                                        int blockSize, juce::String& error)>;

    DeviceManager(Session& session, Creator creator);
    ~DeviceManager() override;

    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    /** The node for a DEVICE tree, created if needed. Null if it could not be created. */
    std::shared_ptr<engine::ProcessorNode> getNode(const juce::ValueTree& device);
    /** The node for a device id if it exists already (does not create). */
    [[nodiscard]] std::shared_ptr<engine::ProcessorNode> findNode(std::uint32_t deviceId) const;
    /** Why a device could not be created (empty if it was). */
    [[nodiscard]] juce::String getError(std::uint32_t deviceId) const;

    /** (Re)prepares every instance. Called when the engine is prepared; never while rendering. */
    void prepareAll(double sampleRate, int maximumBlockSize);
    [[nodiscard]] double getSampleRate() const noexcept { return sampleRate_; }
    [[nodiscard]] int getBlockSize() const noexcept { return blockSize_; }

    void setPlayHead(juce::AudioPlayHead* playHead) noexcept { playHead_ = playHead; }
    /** Offline rendering mode for every instance (exports). Never while rendering live. */
    void setNonRealtime(bool nonRealtime);

    /** Writes the state of every live instance into its DEVICE tree (not undoable). */
    void storeStates();
    /** Forgets instances whose DEVICE trees are no longer in the session. */
    void purgeUnused();

    /** Called on the message thread (coalesced) after any device parameter changed. */
    std::function<void()> onParameterChanged;
    /** Called on the message thread (coalesced) after a device reported a new latency (e.g. a
        limiter's lookahead), so the delay compensation can be recomputed. */
    std::function<void()> onLatencyChanged;
    /** Called when an instance has been created (e.g. to refresh device panels). */
    std::function<void(std::uint32_t deviceId)> onNodeCreated;

private:
    struct Entry
    {
        std::shared_ptr<engine::ProcessorNode> node;
        juce::String type;
        juce::String error;
    };

    void storeState(juce::ValueTree device, engine::ProcessorNode& node);

    void valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int index) override;
    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property) override;

    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override { parameterChanged_ = true; }
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details) override
    {
        if (details.parameterInfoChanged || details.programChanged || details.nonParameterStateChanged)
            parameterChanged_ = true;
        if (details.latencyChanged)
            latencyChanged_ = true;
    }
    void timerCallback() override;

    Session& session_;
    Creator creator_;
    mutable std::mutex mutex_; ///< guards entries_ against prepareAll() from the device thread
    std::map<std::uint32_t, Entry> entries_;
    double sampleRate_ = 48000.0;
    int blockSize_ = 512;
    juce::AudioPlayHead* playHead_ = nullptr;
    bool nonRealtime_ = false;
    std::atomic<bool> parameterChanged_{false};
    std::atomic<bool> latencyChanged_{false};
};

} // namespace dojo::session
