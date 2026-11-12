#include "session/DeviceManager.h"

#include <vector>

namespace dojo::session
{

DeviceManager::DeviceManager(Session& session, Creator creator)
    : session_(session)
    , creator_(std::move(creator))
{
    session_.getState().addListener(this);
    startTimerHz(10);
}

DeviceManager::~DeviceManager()
{
    stopTimer();
    session_.getState().removeListener(this);

    const std::lock_guard lock(mutex_);
    for (auto& [id, entry] : entries_)
        if (entry.node != nullptr)
            entry.node->getProcessor().removeListener(this);
}

std::shared_ptr<engine::ProcessorNode> DeviceManager::getNode(const juce::ValueTree& deviceTree)
{
    if (!deviceTree.hasType(ids::device))
        return nullptr;

    const auto id = device::getId(deviceTree);
    const auto type = device::getType(deviceTree);
    double sampleRate = 48000.0;
    int blockSize = 512;

    {
        const std::lock_guard lock(mutex_);
        sampleRate = sampleRate_;
        blockSize = blockSize_;
        if (const auto it = entries_.find(id); it != entries_.end() && it->second.type == type)
        {
            if (it->second.node != nullptr)
                it->second.node->setBypassed(device::isBypassed(deviceTree));
            return it->second.node;
        }
    }

    Entry entry;
    entry.type = type;

    juce::String error;
    auto processor = creator_ ? creator_(deviceTree, sampleRate, blockSize, error) : nullptr;

    if (processor == nullptr)
    {
        entry.error = error.isNotEmpty() ? error : "Unknown device type \"" + type + "\".";
    }
    else
    {
        const auto state = deviceTree.getProperty(ids::state).toString();
        if (state.isNotEmpty())
        {
            juce::MemoryBlock block;
            if (block.fromBase64Encoding(state) && block.getSize() > 0)
                processor->setStateInformation(block.getData(), static_cast<int>(block.getSize()));
        }

        auto node = std::make_shared<engine::ProcessorNode>(std::move(processor));
        node->setNonRealtime(nonRealtime_);
        node->prepare(sampleRate, blockSize, playHead_);
        node->setBypassed(device::isBypassed(deviceTree));
        node->getProcessor().addListener(this);
        entry.node = std::move(node);
    }

    auto node = entry.node;
    {
        const std::lock_guard lock(mutex_);
        if (auto it = entries_.find(id); it != entries_.end() && it->second.node != nullptr)
            it->second.node->getProcessor().removeListener(this);
        entries_[id] = std::move(entry);
    }

    if (node != nullptr && onNodeCreated)
        onNodeCreated(id);

    return node;
}

std::vector<std::shared_ptr<engine::ProcessorNode>> DeviceManager::getCompanions(const juce::ValueTree& deviceTree, int count)
{
    auto primary = getNode(deviceTree);
    if (primary == nullptr || count <= 0)
        return {};

    const auto id = device::getId(deviceTree);
    double sampleRate = 48000.0;
    int blockSize = 512;
    std::vector<std::shared_ptr<engine::ProcessorNode>> companions;
    {
        const std::lock_guard lock(mutex_);
        sampleRate = sampleRate_;
        blockSize = blockSize_;
        const auto it = entries_.find(id);
        if (it == entries_.end())
            return {};
        companions = it->second.companions;
    }

    if (static_cast<int>(companions.size()) < count)
    {
        juce::MemoryBlock state;
        primary->getProcessor().getStateInformation(state);
        while (static_cast<int>(companions.size()) < count)
        {
            juce::String error;
            auto processor = creator_ ? creator_(deviceTree, sampleRate, blockSize, error) : nullptr;
            if (processor == nullptr)
                break;
            if (state.getSize() > 0)
                processor->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            auto node = std::make_shared<engine::ProcessorNode>(std::move(processor));
            node->setNonRealtime(nonRealtime_);
            node->prepare(sampleRate, blockSize, playHead_);
            node->setBypassed(primary->isBypassed());
            companions.push_back(std::move(node));
        }

        const std::lock_guard lock(mutex_);
        if (const auto it = entries_.find(id); it != entries_.end())
            it->second.companions = companions;
    }

    companions.resize(std::min(companions.size(), static_cast<std::size_t>(count)));
    return companions;
}

std::shared_ptr<engine::ProcessorNode> DeviceManager::findNode(std::uint32_t deviceId) const
{
    const std::lock_guard lock(mutex_);
    const auto it = entries_.find(deviceId);
    return it != entries_.end() ? it->second.node : nullptr;
}

juce::String DeviceManager::getError(std::uint32_t deviceId) const
{
    const std::lock_guard lock(mutex_);
    const auto it = entries_.find(deviceId);
    return it != entries_.end() ? it->second.error : juce::String();
}

void DeviceManager::prepareAll(double sampleRate, int maximumBlockSize)
{
    std::vector<std::shared_ptr<engine::ProcessorNode>> nodes;
    {
        const std::lock_guard lock(mutex_);
        sampleRate_ = sampleRate;
        blockSize_ = maximumBlockSize;
        for (auto& [id, entry] : entries_)
        {
            if (entry.node != nullptr)
                nodes.push_back(entry.node);
            nodes.insert(nodes.end(), entry.companions.begin(), entry.companions.end());
        }
    }

    for (auto& node : nodes)
        node->prepare(sampleRate, maximumBlockSize, playHead_);
}

void DeviceManager::setNonRealtime(bool nonRealtime)
{
    const std::lock_guard lock(mutex_);
    nonRealtime_ = nonRealtime;
    for (auto& [id, entry] : entries_)
    {
        if (entry.node != nullptr)
            entry.node->setNonRealtime(nonRealtime);
        for (auto& companion : entry.companions)
            companion->setNonRealtime(nonRealtime);
    }
}

void DeviceManager::storeState(juce::ValueTree deviceTree, engine::ProcessorNode& node)
{
    juce::MemoryBlock block;
    node.getProcessor().getStateInformation(block);
    deviceTree.setProperty(ids::state, block.toBase64Encoding(), nullptr);
}

void DeviceManager::storeStates()
{
    const auto store = [this](const juce::ValueTree& owner)
    {
        for (auto d : Session::getDevices(owner))
            if (auto node = findNode(device::getId(d)))
                storeState(d, *node);
    };

    store(session_.getMaster());
    for (const auto& t : session_.getAllTracks())
        store(t);
}

void DeviceManager::purgeUnused()
{
    std::vector<std::shared_ptr<engine::ProcessorNode>> dropped;
    {
        const std::lock_guard lock(mutex_);
        for (auto it = entries_.begin(); it != entries_.end();)
        {
            if (!session_.findDevice(it->first).isValid())
            {
                if (it->second.node != nullptr)
                {
                    it->second.node->getProcessor().removeListener(this);
                    dropped.push_back(it->second.node);
                }
                it = entries_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
    // `dropped` releases our references here, outside the lock; graphs still using a node keep it alive.
}

void DeviceManager::valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree& child, int)
{
    // Keep the removed device's latest state in its tree, so that undo restores it exactly.
    const auto snapshot = [this](juce::ValueTree tree, auto& self) -> void
    {
        if (tree.hasType(ids::device))
        {
            if (auto node = findNode(device::getId(tree)))
                storeState(tree, *node);
            return;
        }
        for (auto c : tree)
            self(c, self);
    };
    snapshot(child, snapshot);
}

void DeviceManager::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree.hasType(ids::device) && property == ids::bypassed)
        if (auto node = findNode(device::getId(tree)))
            node->setBypassed(device::isBypassed(tree));
}

void DeviceManager::timerCallback()
{
    if (parameterChanged_.exchange(false) && onParameterChanged)
        onParameterChanged();
    if (latencyChanged_.exchange(false) && onLatencyChanged)
        onLatencyChanged();
}

} // namespace dojo::session
