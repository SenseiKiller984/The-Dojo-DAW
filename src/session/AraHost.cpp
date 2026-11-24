#include "session/AraHost.h"

#include "engine/ProcessorNode.h"
#include "session/AudioAssetCache.h"
#include "session/AudioEdits.h"
#include "session/DeviceManager.h"
#include "session/Session.h"

#if JUCE_PLUGINHOST_ARA
// The VST3 binding (ARA's IPlugInEntryPoint2 on the plug-in's component): the SDK headers, quietly.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wzero-as-null-pointer-constant"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wredundant-decls"
#pragma GCC diagnostic ignored "-Woverloaded-virtual"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <ARA_API/ARAVST3.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
#endif

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace dojo::session
{

#if JUCE_PLUGINHOST_ARA
namespace
{
namespace Model = juce::ARAHostModel;

constexpr auto kAllRoles = static_cast<ARA::ARAPlugInInstanceRoleFlags>(ARA::kARAPlaybackRendererRole | ARA::kARAEditorRendererRole
                                                                        | ARA::kARAEditorViewRole);

/** An audio source in a document: what the plug-ins read. Usually an audio file, shared by its
    clips; a clip with native edits (PITCH, SPECTRAL) has one of its own, reading its edited audio. */
struct Source
{
    juce::String key; ///< the file's path, or "clip:<id>" (a clip's own)
    juce::File file;  ///< what it reads now
    std::string name;
    std::string persistentId;
    double sampleRate = 0.0;
    int channels = 0;
    juce::int64 frames = 0;
    AudioAssetPtr asset;                  ///< decoded in memory, or streamed (then read from the file)
    juce::AudioFormatManager* formats = nullptr;
    std::unique_ptr<Model::AudioSource> ara;
    int users = 0;                        ///< clips using it
    bool fresh = false;                   ///< made in this synchronisation
};
using SourceRef = Model::ConversionFunctions<Source*, ARA::ARAAudioSourceHostRef>;

/** A reader a plug-in made for a source. One thread at a time (as ARA promises). */
struct Reader
{
    Source* source = nullptr;
    bool use64Bit = false;
    std::unique_ptr<juce::AudioFormatReader> file; ///< (when the source is not decoded in memory)
    juce::AudioBuffer<float> scratch;

    bool read(juce::int64 position, juce::int64 count, void* const* buffers)
    {
        const int channels = source->channels;
        constexpr int kChunk = 32768;
        for (juce::int64 done = 0; done < count; done += kChunk)
        {
            const auto n = static_cast<int>(std::min<juce::int64>(kChunk, count - done));
            const auto at = position + done;
            scratch.setSize(channels, n, false, false, true);
            scratch.clear();
            const auto& asset = source->asset;
            if (asset != nullptr && !asset->isStreamed())
            {
                // Decoded: copy the part inside the file (silence outside it).
                const auto from = std::max<juce::int64>(at, 0);
                const auto to = std::min<juce::int64>(at + n, asset->audio.getNumSamples());
                if (to > from)
                    for (int c = 0; c < channels; ++c)
                        scratch.copyFrom(c, static_cast<int>(from - at), asset->audio, std::min(c, asset->audio.getNumChannels() - 1),
                                         static_cast<int>(from), static_cast<int>(to - from));
            }
            else if (file != nullptr)
            {
                if (!file->read(scratch.getArrayOfWritePointers(), channels, at, n)) // (zero-fills outside the file)
                    return false;
            }
            else
            {
                return false;
            }
            for (int c = 0; c < channels; ++c)
            {
                if (use64Bit)
                {
                    auto* out = static_cast<double*>(buffers[c]) + done;
                    const auto* in = scratch.getReadPointer(c);
                    for (int i = 0; i < n; ++i)
                        out[i] = static_cast<double>(in[i]);
                }
                else
                {
                    std::memcpy(static_cast<float*>(buffers[c]) + done, scratch.getReadPointer(c), static_cast<std::size_t>(n) * sizeof(float));
                }
            }
        }
        return true;
    }
};
using ReaderRef = Model::ConversionFunctions<Reader*, ARA::ARAAudioReaderHostRef>;

class AudioAccess final : public ARA::Host::AudioAccessControllerInterface
{
public:
    ARA::ARAAudioReaderHostRef createAudioReaderForSource(ARA::ARAAudioSourceHostRef sourceRef, bool use64BitSamples) noexcept override
    {
        auto reader = std::make_unique<Reader>();
        reader->source = SourceRef::fromHostRef(sourceRef);
        reader->use64Bit = use64BitSamples;
        const auto& asset = reader->source->asset;
        if ((asset == nullptr || asset->isStreamed()) && reader->source->formats != nullptr)
            reader->file.reset(reader->source->formats->createReaderFor(reader->source->file));
        auto* raw = reader.get();
        const std::lock_guard lock(mutex_);
        readers_.emplace(raw, std::move(reader));
        return ReaderRef::toHostRef(raw);
    }

    bool readAudioSamples(ARA::ARAAudioReaderHostRef readerRef, ARA::ARASamplePosition position, ARA::ARASampleCount count,
                          void* const* buffers) noexcept override
    {
        auto* reader = ReaderRef::fromHostRef(readerRef);
        return reader != nullptr && reader->read(position, count, buffers);
    }

    void destroyAudioReader(ARA::ARAAudioReaderHostRef readerRef) noexcept override
    {
        const std::lock_guard lock(mutex_);
        readers_.erase(ReaderRef::fromHostRef(readerRef));
    }

private:
    std::mutex mutex_;
    std::map<Reader*, std::unique_ptr<Reader>> readers_;
};

/** An archive being read: its bytes and the id it was written under. */
struct ArchiveReader
{
    juce::MemoryBlock data;
    std::string archiveId;
};
using ArchiveReaderRef = Model::ConversionFunctions<ArchiveReader*, ARA::ARAArchiveReaderHostRef>;
using ArchiveWriterRef = Model::ConversionFunctions<juce::MemoryOutputStream*, ARA::ARAArchiveWriterHostRef>;

class Archiving final : public ARA::Host::ArchivingControllerInterface
{
public:
    ARA::ARASize getArchiveSize(ARA::ARAArchiveReaderHostRef readerRef) noexcept override
    {
        return static_cast<ARA::ARASize>(ArchiveReaderRef::fromHostRef(readerRef)->data.getSize());
    }

    bool readBytesFromArchive(ARA::ARAArchiveReaderHostRef readerRef, ARA::ARASize position, ARA::ARASize length,
                              ARA::ARAByte* buffer) noexcept override
    {
        const auto& data = ArchiveReaderRef::fromHostRef(readerRef)->data;
        if (position + length > data.getSize())
            return false;
        std::memcpy(buffer, static_cast<const char*>(data.getData()) + position, length);
        return true;
    }

    bool writeBytesToArchive(ARA::ARAArchiveWriterHostRef writerRef, ARA::ARASize position, ARA::ARASize length,
                             const ARA::ARAByte* buffer) noexcept override
    {
        auto* writer = ArchiveWriterRef::fromHostRef(writerRef);
        return writer->setPosition(static_cast<juce::int64>(position)) && writer->write(buffer, length);
    }

    void notifyDocumentArchivingProgress(float) noexcept override {}
    void notifyDocumentUnarchivingProgress(float) noexcept override {}

    ARA::ARAPersistentID getDocumentArchiveID(ARA::ARAArchiveReaderHostRef readerRef) noexcept override
    {
        const auto& id = ArchiveReaderRef::fromHostRef(readerRef)->archiveId;
        return id.empty() ? nullptr : id.c_str();
    }
};

/** The session's tempo map, for the plug-ins' content readers (they may ask from any thread). */
struct SharedTempoMap
{
    std::shared_ptr<const time::TempoMap> get() const
    {
        const std::lock_guard<std::mutex> lock(mutex);
        return map;
    }
    void set(std::shared_ptr<const time::TempoMap> m)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        map = std::move(m);
    }

private:
    mutable std::mutex mutex;
    std::shared_ptr<const time::TempoMap> map = std::make_shared<const time::TempoMap>();
};

/** The tempo map as ARA tempo entries: time and quarter position pairs with a steady tempo between
    them, so ramps and curves become steps of an eighth of a beat (and one beat past the last point). */
std::vector<ARA::ARAContentTempoEntry> tempoEntries(const time::TempoMap& map)
{
    std::vector<ARA::ARAContentTempoEntry> entries;
    const auto add = [&](double beat) { entries.push_back({map.secondsAt(beat), beat}); };
    const auto& points = map.getPoints();
    for (std::size_t i = 0; i < points.size(); ++i)
    {
        const double from = points[i].beat;
        if (i + 1 == points.size())
        {
            add(from);
            add(from + 1.0);
            break;
        }
        const double to = points[i + 1].beat;
        if (points[i].shape == time::TempoShape::jump)
        {
            add(from);
            continue;
        }
        const int steps = std::clamp(static_cast<int>(std::ceil((to - from) * 8.0)), 1, 4096);
        for (int k = 0; k < steps; ++k)
            add(from + (to - from) * k / steps);
    }
    return entries;
}

std::vector<ARA::ARAContentBarSignature> barSignatures(const time::TempoMap& map)
{
    std::vector<ARA::ARAContentBarSignature> bars;
    for (const auto& s : map.getSignatures())
        bars.push_back({s.signature.numerator, s.signature.denominator, map.beatOfBar(s.bar)});
    return bars;
}

/** A reader of the musical context's tempo entries or bar signatures. */
struct ContentReader
{
    ARA::ARAContentType type = ARA::kARAContentTypeTempoEntries;
    std::vector<ARA::ARAContentTempoEntry> tempo;
    std::vector<ARA::ARAContentBarSignature> bars;
};
using ContentReaderRef = Model::ConversionFunctions<ContentReader*, ARA::ARAContentReaderHostRef>;

class ContentAccess final : public ARA::Host::ContentAccessControllerInterface
{
public:
    explicit ContentAccess(std::shared_ptr<SharedTempoMap> map)
        : map_(std::move(map))
    {
    }

    bool isMusicalContextContentAvailable(ARA::ARAMusicalContextHostRef, ARA::ARAContentType type) noexcept override
    {
        return type == ARA::kARAContentTypeTempoEntries || type == ARA::kARAContentTypeBarSignatures;
    }
    ARA::ARAContentGrade getMusicalContextContentGrade(ARA::ARAMusicalContextHostRef, ARA::ARAContentType) noexcept override
    {
        return ARA::kARAContentGradeAdjusted;
    }
    ARA::ARAContentReaderHostRef createMusicalContextContentReader(ARA::ARAMusicalContextHostRef, ARA::ARAContentType type,
                                                                   const ARA::ARAContentTimeRange*) noexcept override
    {
        auto* reader = new ContentReader();
        reader->type = type;
        const auto map = map_->get();
        reader->tempo = tempoEntries(*map);
        reader->bars = barSignatures(*map);
        return ContentReaderRef::toHostRef(reader);
    }
    bool isAudioSourceContentAvailable(ARA::ARAAudioSourceHostRef, ARA::ARAContentType) noexcept override { return false; }
    ARA::ARAContentGrade getAudioSourceContentGrade(ARA::ARAAudioSourceHostRef, ARA::ARAContentType) noexcept override
    {
        return ARA::kARAContentGradeInitial;
    }
    ARA::ARAContentReaderHostRef createAudioSourceContentReader(ARA::ARAAudioSourceHostRef, ARA::ARAContentType,
                                                                const ARA::ARAContentTimeRange*) noexcept override
    {
        return nullptr;
    }
    ARA::ARAInt32 getContentReaderEventCount(ARA::ARAContentReaderHostRef readerRef) noexcept override
    {
        const auto* reader = ContentReaderRef::fromHostRef(readerRef);
        return static_cast<ARA::ARAInt32>(reader->type == ARA::kARAContentTypeTempoEntries ? reader->tempo.size() : reader->bars.size());
    }
    const void* getContentReaderDataForEvent(ARA::ARAContentReaderHostRef readerRef, ARA::ARAInt32 index) noexcept override
    {
        auto* reader = ContentReaderRef::fromHostRef(readerRef);
        if (reader->type == ARA::kARAContentTypeTempoEntries)
            return &reader->tempo[static_cast<std::size_t>(std::clamp<ARA::ARAInt32>(index, 0, static_cast<ARA::ARAInt32>(reader->tempo.size()) - 1))];
        return &reader->bars[static_cast<std::size_t>(std::clamp<ARA::ARAInt32>(index, 0, static_cast<ARA::ARAInt32>(reader->bars.size()) - 1))];
    }
    void destroyContentReader(ARA::ARAContentReaderHostRef readerRef) noexcept override { delete ContentReaderRef::fromHostRef(readerRef); }

private:
    std::shared_ptr<SharedTempoMap> map_;
};

class ModelUpdates final : public ARA::Host::ModelUpdateControllerInterface
{
public:
    void notifyAudioSourceAnalysisProgress(ARA::ARAAudioSourceHostRef, ARA::ARAAnalysisProgressState, float) noexcept override {}
    void notifyAudioSourceContentChanged(ARA::ARAAudioSourceHostRef, const ARA::ARAContentTimeRange*, ARA::ContentUpdateScopes) noexcept override {}
    void notifyAudioModificationContentChanged(ARA::ARAAudioModificationHostRef, const ARA::ARAContentTimeRange*,
                                               ARA::ContentUpdateScopes) noexcept override
    {
    }
    void notifyPlaybackRegionContentChanged(ARA::ARAPlaybackRegionHostRef, const ARA::ARAContentTimeRange*, ARA::ContentUpdateScopes) noexcept override
    {
    }
    void notifyDocumentDataChanged() noexcept override {}
};

/** A plug-in's transport requests, handed to the message thread (they may come from any thread). */
struct TransportRequests
{
    std::function<void(bool)> playback;
    std::function<void(double)> locate;
    std::function<void(double, double)> cycleRange; ///< start, duration (seconds)
    std::function<void(bool)> cycleEnabled;
};

class PlaybackControl final : public ARA::Host::PlaybackControllerInterface
{
public:
    explicit PlaybackControl(std::weak_ptr<TransportRequests> requests)
        : requests_(std::move(requests))
    {
    }
    void requestStartPlayback() noexcept override { post([](TransportRequests& r) { if (r.playback) r.playback(true); }); }
    void requestStopPlayback() noexcept override { post([](TransportRequests& r) { if (r.playback) r.playback(false); }); }
    void requestSetPlaybackPosition(ARA::ARATimePosition position) noexcept override
    {
        post([position](TransportRequests& r) { if (r.locate) r.locate(position); });
    }
    void requestSetCycleRange(ARA::ARATimePosition start, ARA::ARATimeDuration duration) noexcept override
    {
        post([start, duration](TransportRequests& r) { if (r.cycleRange) r.cycleRange(start, duration); });
    }
    void requestEnableCycle(bool enable) noexcept override
    {
        post([enable](TransportRequests& r) { if (r.cycleEnabled) r.cycleEnabled(enable); });
    }

private:
    void post(std::function<void(TransportRequests&)> action)
    {
        juce::MessageManager::callAsync([weak = requests_, run = std::move(action)]
                                        {
                                            if (const auto requests = weak.lock())
                                                run(*requests);
                                        });
    }
    std::weak_ptr<TransportRequests> requests_;
};

/** A clip on an ARA track: its audio modification and playback region. */
struct ClipModel
{
    std::uint32_t clipId = 0;
    std::uint32_t trackId = 0;
    Source* source = nullptr;
    std::string name;
    std::string persistentId;
    double start = 0.0;
    double length = 0.0;
    double offset = 0.0;
    double stretch = 1.0;
    std::unique_ptr<Model::AudioModification> modification;
    std::unique_ptr<Model::PlaybackRegion> region;
};
using ClipRef = Model::ConversionFunctions<ClipModel*, ARA::ARAPlaybackRegionHostRef>;
using ModificationRef = Model::ConversionFunctions<ClipModel*, ARA::ARAAudioModificationHostRef>;

/** A region sequence: an ARA track. */
struct Sequence
{
    std::string name;
    std::unique_ptr<Model::RegionSequence> ara;
};

/** One plug-in factory's document for the project, owned by the host: taken down (its model
    objects, then its controller) while a plug-in bound to it is still alive. */
struct Document
{
    juce::String factoryId;
    std::string archiveId;
    ARA::ARAPlaybackTransformationFlags transformations = 0;
    juce::ARAFactoryWrapper factory;
    std::unique_ptr<juce::ARAHostDocumentController> controller;
    std::unique_ptr<Model::MusicalContext> context;
    std::map<std::uint32_t, Sequence> sequences;           ///< by track id
    std::map<juce::String, std::unique_ptr<Source>> sources; ///< by key
    std::map<std::uint32_t, std::unique_ptr<ClipModel>> clips; ///< by clip id
    std::shared_ptr<const time::TempoMap> tempoMap; ///< the map its musical context last had
    juce::MemoryBlock pendingArchive;                      ///< from the session, given back once the regions exist
    bool takenDown = false;

    ~Document() { takeDown(); }

    ARA::Host::DocumentController& dc() { return controller->getDocumentController(); }

    /** Destroys every model object (inside one edit cycle). */
    void takeDown()
    {
        if (controller == nullptr || takenDown)
            return;
        takenDown = true;
        {
            const juce::ARAEditGuard guard(dc());
            for (auto& [id, clip] : clips)
            {
                clip->region.reset();
                clip->modification.reset();
            }
            clips.clear();
            for (auto& [path, source] : sources)
                if (source->ara != nullptr)
                {
                    source->ara->enableAudioSourceSamplesAccess(false);
                    source->ara.reset();
                }
            sources.clear();
            sequences.clear();
            context.reset();
        }
    }
};
using ContextRef = Model::ConversionFunctions<Document*, ARA::ARAMusicalContextHostRef>;
using SequenceRef = Model::ConversionFunctions<Sequence*, ARA::ARARegionSequenceHostRef>;

/** A plug-in instance bound to a document. */
struct Binding
{
    std::uint32_t deviceId = 0;
    std::uint32_t trackId = 0;
    juce::String pluginName;
    Document* document = nullptr;
    const ARA::ARAPlugInExtensionInstance* extension = nullptr;
    std::unique_ptr<Model::PlaybackRendererInterface> playback;
    std::unique_ptr<Model::EditorRendererInterface> editor;
    std::set<std::uint32_t> assigned; ///< the clips whose regions it has
};

/** Binds a VST3 plug-in to a document through ARA's IPlugInEntryPoint2, keeping the whole extension
    instance (JUCE's binding hides the editor view, which the clip editor's selection needs). */
const ARA::ARAPlugInExtensionInstance* bindVst3(juce::AudioPluginInstance& instance, ARA::ARADocumentControllerRef documentController)
{
    auto* client = instance.getVST3Client();
    auto* component = client != nullptr ? client->getIComponentPtr() : nullptr;
    if (component == nullptr)
        return nullptr;
    ARA::IPlugInEntryPoint2* entry = nullptr;
    if (component->queryInterface(ARA::IPlugInEntryPoint2::iid, reinterpret_cast<void**>(&entry)) != Steinberg::kResultOk || entry == nullptr)
        return nullptr;
    const auto* extension = entry->bindToDocumentControllerWithRoles(documentController, kAllRoles, kAllRoles);
    entry->release();
    return extension;
}
} // namespace

struct AraHost::Impl final : private juce::ValueTree::Listener, private juce::AsyncUpdater, private AudioAssetCache::Listener
{
    Impl(AraHost& o, Session& s, AudioAssetCache& a, DeviceManager& d)
        : owner(o)
        , session(s)
        , assets(a)
        , devices(d)
    {
        transport = std::make_shared<TransportRequests>();
        transport->playback = [this](bool play)
        {
            if (owner.onRequestPlayback)
                owner.onRequestPlayback(play);
        };
        transport->locate = [this](double seconds)
        {
            if (owner.onRequestLocate)
                owner.onRequestLocate(seconds);
        };
        // The cycle is the session's loop (an undo step, as from the transport bar).
        transport->cycleRange = [this](double start, double duration)
        {
            session.beginTransaction("Set Loop");
            session.setLoop(session.isLoopEnabled(), start, start + duration);
        };
        transport->cycleEnabled = [this](bool enable)
        {
            if (enable == session.isLoopEnabled())
                return;
            session.beginTransaction("Toggle Loop");
            session.setLoopEnabled(enable);
        };
        session.getState().addListener(this);
        assets.addListener(this);
    }

    ~Impl() override
    {
        cancelPendingUpdate();
        session.getState().removeListener(this);
        assets.removeListener(this);
        reset();
    }

    // --- Binding -----------------------------------------------------------------------------------

    void deviceCreated(const juce::ValueTree& device, juce::AudioProcessor& processor)
    {
        if (!device::isAra(device))
            return;
        const auto track = Session::getDeviceOwner(device);
        const auto trackId = track::getId(track);
        if (Session::getAraDevice(track) != device)
            return; // (not its track's ARA device any more)
        auto* instance = dynamic_cast<juce::AudioPluginInstance*>(&processor);
        if (instance == nullptr || !instance->getPluginDescription().hasARAExtension)
        {
            problems[trackId] = "The plug-in has no ARA extension.";
            return;
        }
        juce::ARAFactoryWrapper factory;
        juce::createARAFactoryAsync(*instance, [&factory](juce::ARAFactoryWrapper f) { factory = std::move(f); }); // (VST3: at once)
        if (factory.get() == nullptr)
        {
            problems[trackId] = instance->getName() + " gave no ARA factory.";
            return;
        }
        auto* document = documentFor(factory);
        if (document == nullptr)
        {
            problems[trackId] = instance->getName() + " could not make an ARA document.";
            return;
        }
        const auto* extension = bindVst3(*instance, document->dc().getRef());
        if (extension == nullptr)
        {
            problems[trackId] = instance->getName() + " could not be bound to its ARA document.";
            retireIfUnused(*document, false); // (now, while this instance keeps its module loaded)
            return;
        }

        Binding binding;
        binding.deviceId = device::getId(device);
        binding.trackId = trackId;
        binding.pluginName = instance->getName();
        binding.document = document;
        binding.extension = extension;
        const Model::PlugInExtensionInstance wrapped{extension};
        if (auto playback = wrapped.getPlaybackRendererInterface(); playback.isValid())
            binding.playback = std::make_unique<Model::PlaybackRendererInterface>(std::move(playback));
        if (auto editor = wrapped.getEditorRendererInterface(); editor.isValid())
            binding.editor = std::make_unique<Model::EditorRendererInterface>(std::move(editor));
        bindings[binding.deviceId] = std::move(binding);
        problems.erase(trackId);
        triggerAsyncUpdate();
        owner.sendChangeMessage();
    }

    void deviceRemoved(std::uint32_t deviceId)
    {
        const auto it = bindings.find(deviceId);
        if (it == bindings.end())
            return;
        release(it->second);
        auto* document = it->second.document;
        bindings.erase(it);
        if (document != nullptr)
            retireIfUnused(*document, true);
        owner.sendSynchronousChangeMessage(); // (an editor of the plug-in goes now, before the plug-in)
        triggerAsyncUpdate();
    }

    /** Takes a document down when no plug-in is bound to it any more (keeping its edits in the
        session first, if asked). Call while the last plug-in is still alive: the document's code is
        in its module. */
    void retireIfUnused(Document& document, bool keepEdits)
    {
        for (const auto& [deviceId, binding] : bindings)
            if (binding.document == &document)
                return;
        if (keepEdits)
            storeArchive(document);
        document.takeDown();
        documents.erase(document.factoryId);
    }

    /** Takes all its regions from a plug-in, and its renderer interfaces (it is out of play). */
    void release(Binding& binding)
    {
        reconfigure(binding.deviceId,
                    [&]
                    {
                        if (auto* document = binding.document)
                            for (const auto id : binding.assigned)
                                if (const auto c = document->clips.find(id); c != document->clips.end() && c->second->region != nullptr)
                                {
                                    if (binding.playback != nullptr)
                                        binding.playback->remove(*c->second->region);
                                    if (binding.editor != nullptr)
                                        binding.editor->remove(*c->second->region);
                                }
                        binding.assigned.clear();
                        binding.playback.reset();
                        binding.editor.reset();
                    });
    }

    void reconfigure(std::uint32_t deviceId, const std::function<void()>& change)
    {
        if (auto node = devices.findNode(deviceId))
            node->reconfigure([&change](juce::AudioProcessor&) { change(); });
        else
            change();
    }

    Document* documentFor(const juce::ARAFactoryWrapper& factory)
    {
        const juce::String id(factory.get()->factoryID);
        if (const auto it = documents.find(id); it != documents.end())
            return it->second.get();

        auto document = std::make_unique<Document>();
        document->factoryId = id;
        document->archiveId = factory.get()->documentArchiveID != nullptr ? factory.get()->documentArchiveID : "";
        document->transformations = factory.get()->supportedPlaybackTransformationFlags;
        document->factory = factory;
        document->controller = juce::ARAHostDocumentController::create(factory, "The Dojo", std::make_unique<AudioAccess>(),
                                                                         std::make_unique<Archiving>(), std::make_unique<ContentAccess>(tempo),
                                                                         std::make_unique<ModelUpdates>(), std::make_unique<PlaybackControl>(transport));
        if (document->controller == nullptr)
            return nullptr;
        tempo->set(session.getTempoMapShared());
        document->tempoMap = tempo->get();
        {
            const juce::ARAEditGuard guard(document->dc());
            auto properties = Model::MusicalContext::getEmptyProperties();
            properties.name = "The Dojo";
            properties.orderIndex = 0;
            properties.color = nullptr;
            document->context = std::make_unique<Model::MusicalContext>(ContextRef::toHostRef(document.get()), document->dc(), properties);
        }
        // Its edits, from the session (given back once the regions are there).
        for (const auto& stored : session.getState().getChildWithName(ids::araDocuments))
            if (stored.hasType(ids::araDocument) && stored.getProperty(ids::araFactory).toString() == id)
                document->pendingArchive.fromBase64Encoding(stored.getProperty(ids::araArchive).toString());
        auto* raw = document.get();
        documents[id] = std::move(document);
        return raw;
    }

    // --- Following the session ---------------------------------------------------------------------

    struct Wanted
    {
        std::uint32_t trackId = 0;
        juce::File file;
        AudioAssetPtr asset;
        std::string name;
        double start = 0.0, length = 0.0, offset = 0.0, stretch = 1.0;
        // Its audio source: the file's, shared, or with native edits the clip's own (whatever
        // file it plays: the edits' render, or its own file before the first or while bypassed).
        juce::String sourceKey;
        std::string sourceName, sourceId;
    };

    static bool isClipsOwn(const juce::String& key) { return key.startsWith("clip:"); }

    /** A clip's state in its plug-in, carried to the source it moves to (native edits added or
        removed): a partial archive of its audio modification and the source it had. */
    struct Transfer
    {
        std::uint32_t clipId = 0;
        juce::MemoryBlock archive;
        std::string sourceId, modificationId;
        juce::File file; ///< what its source read
    };

    void synchronise()
    {
        cancelPendingUpdate();
        std::set<std::uint32_t> nowRendering;
        for (const auto& [deviceId, binding] : bindings)
        {
            const auto device = session.findDevice(deviceId);
            if (device.isValid() && binding.playback != nullptr && !device::isBypassed(device)
                && Session::getAraDevice(Session::getDeviceOwner(device)) == device)
                nowRendering.insert(binding.trackId);
        }
        if (session.getTempoMapShared() != tempo->get())
            tempo->set(session.getTempoMapShared());
        for (auto& [id, document] : documents)
            synchronise(*document);
        if (nowRendering != rendering)
        {
            rendering = nowRendering;
            if (owner.onRenderingChanged)
                owner.onRenderingChanged();
            owner.sendChangeMessage();
        }
    }

    void synchronise(Document& document)
    {
        auto& dc = document.dc();
        // What the session wants: the clips of each bound plug-in's track (while it is its first device).
        std::map<std::uint32_t, Wanted> wanted;
        std::map<std::uint32_t, std::string> trackNames;
        std::map<std::uint32_t, std::set<std::uint32_t>> assign; // device id -> clip ids
        bool waiting = false;                                     // (a clip's audio still loading)
        for (const auto& [deviceId, binding] : bindings)
        {
            if (binding.document != &document)
                continue;
            auto& clipsFor = assign[deviceId];
            const auto device = session.findDevice(deviceId);
            const auto track = Session::getDeviceOwner(device);
            if (!device.isValid() || binding.playback == nullptr || Session::getAraDevice(track) != device)
                continue;
            trackNames[binding.trackId] = track::getName(track).toStdString();
            for (const auto& c : track)
            {
                if (!c.hasType(ids::clip))
                    continue;
                const auto file = clip::getFile(c);
                auto asset = assets.find(file);
                if (asset == nullptr)
                {
                    if (file.existsAsFile())
                    {
                        waiting = true;
                        if (!assets.isLoading(file))
                            assets.request(file, {});
                    }
                    continue;
                }
                Wanted w;
                w.trackId = binding.trackId;
                w.file = file;
                w.asset = std::move(asset);
                w.name = clip::getName(c).toStdString();
                const auto frames = std::to_string(w.asset->numFrames);
                if (audioedits::getEdit(c).isValid())
                {
                    // (portable: the same clip of the same file, wherever the project folder is)
                    const auto own = audioedits::getSource(c).getFileName();
                    w.sourceKey = "clip:" + juce::String(clip::getId(c));
                    w.sourceName = (own + " (edited)").toStdString();
                    w.sourceId = own.toStdString() + "#" + frames + "#clip-" + std::to_string(clip::getId(c));
                }
                else
                {
                    w.sourceKey = file.getFullPathName();
                    w.sourceName = file.getFileName().toStdString();
                    w.sourceId = w.sourceName + "#" + frames; // (portable: the same file, wherever the project folder is)
                }
                w.start = clip::getStart(c);
                w.length = clip::getLength(c);
                w.offset = clip::getOffset(c);
                w.stretch = std::max(1.0e-3, clip::getStretch(c));
                wanted[clip::getId(c)] = std::move(w);
                clipsFor.insert(clip::getId(c));
            }
        }

        // 1. Plug-ins give up the regions they lose (or whose clip changes source or track): out of play.
        //    A clip's own source (native edits) stays as long as its audio has the same shape, the
        //    file it reads changing in place (3).
        const auto sameSource = [&](const ClipModel& model, const Wanted& w)
        {
            return model.source != nullptr && model.source->key == w.sourceKey && model.trackId == w.trackId
                   && model.source->frames == w.asset->numFrames && model.source->channels == std::max(1, w.asset->numChannels)
                   && std::abs(model.source->sampleRate - w.asset->sampleRate) < 1.0e-6;
        };
        // A clip whose native edits come or go keeps what its plug-in did with it: archived now (no
        // edit cycle open), given to its new modification once it is there.
        std::vector<Transfer> transfers;
        for (const auto& [clipId, model] : document.clips)
        {
            const auto w = wanted.find(clipId);
            if (w == wanted.end() || model->source == nullptr || model->modification == nullptr || model->trackId != w->second.trackId
                || sameSource(*model, w->second) || !(isClipsOwn(model->source->key) || isClipsOwn(w->second.sourceKey)))
                continue;
            juce::MemoryOutputStream archive;
            const ARA::ARAAudioSourceRef sources[] = {model->source->ara->getPluginRef()};
            const ARA::ARAAudioModificationRef modifications[] = {model->modification->getPluginRef()};
            ARA::ARAStoreObjectsFilter filter{};
            filter.structSize = ARA::kARAStoreObjectsFilterMinSize;
            filter.documentData = ARA::kARAFalse;
            filter.audioSourceRefsCount = 1;
            filter.audioSourceRefs = sources;
            filter.audioModificationRefsCount = 1;
            filter.audioModificationRefs = modifications;
            if (!dc.storeObjectsToArchive(ArchiveWriterRef::toHostRef(&archive), &filter))
                continue;
            Transfer transfer;
            transfer.clipId = clipId;
            transfer.archive = archive.getMemoryBlock();
            transfer.sourceId = model->source->persistentId;
            transfer.modificationId = model->persistentId;
            transfer.file = model->source->file;
            transfers.push_back(std::move(transfer));
        }
        for (auto& [deviceId, binding] : bindings)
        {
            if (binding.document != &document)
                continue;
            std::vector<ClipModel*> losing;
            for (const auto id : binding.assigned)
            {
                const auto w = wanted.find(id);
                const auto model = document.clips.find(id);
                if (model == document.clips.end())
                    continue;
                if (assign[deviceId].count(id) == 0 || w == wanted.end() || !sameSource(*model->second, w->second))
                    losing.push_back(model->second.get());
            }
            if (!losing.empty())
                reconfigure(deviceId,
                            [&]
                            {
                                for (auto* model : losing)
                                {
                                    if (binding.playback != nullptr)
                                        binding.playback->remove(*model->region);
                                    if (binding.editor != nullptr)
                                        binding.editor->remove(*model->region);
                                    binding.assigned.erase(model->clipId);
                                }
                            });
        }

        // A clip's own source whose file changed (edits rendered again, bypassed, or rendered the
        // first time): no reading while it changes (this waits for reads under way).
        std::vector<Source*> refilled;
        for (const auto& [clipId, model] : document.clips)
        {
            const auto w = wanted.find(clipId);
            if (w == wanted.end() || !sameSource(*model, w->second) || model->source->file == w->second.file
                || std::find(refilled.begin(), refilled.end(), model->source) != refilled.end())
                continue;
            model->source->ara->enableAudioSourceSamplesAccess(false);
            model->source->file = w->second.file;
            model->source->asset = w->second.asset;
            refilled.push_back(model->source);
        }

        // 2. The model, in one edit cycle.
        {
            const juce::ARAEditGuard guard(dc);
            // 3. ... its samples changed: the plug-in keeps its modification and region, and reads
            //    it again (how its edits follow the new audio is up to it).
            for (auto* source : refilled)
                dc.updateAudioSourceContent(source->ara->getPluginRef(), nullptr, ARA::ContentUpdateScopes::samplesAreAffected());
            if (document.tempoMap != tempo->get() && document.context != nullptr)
            {
                document.tempoMap = tempo->get();
                dc.updateMusicalContextContent(document.context->getPluginRef(), nullptr, ARA::ContentUpdateScopes::timelineIsAffected());
            }
            // Clips gone (or moved to another file or track).
            for (auto it = document.clips.begin(); it != document.clips.end();)
            {
                const auto w = wanted.find(it->first);
                if (w == wanted.end() || !sameSource(*it->second, w->second))
                {
                    it->second->region.reset();
                    it->second->modification.reset();
                    if (it->second->source != nullptr)
                        --it->second->source->users;
                    it = document.clips.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            // Sequences: one per ARA track, named after it.
            for (const auto& [trackId, name] : trackNames)
            {
                auto& sequence = document.sequences[trackId];
                auto properties = Model::RegionSequence::getEmptyProperties();
                properties.orderIndex = static_cast<ARA::ARAInt32>(trackId);
                properties.musicalContextRef = document.context->getPluginRef();
                properties.color = nullptr;
                if (sequence.ara == nullptr)
                {
                    sequence.name = name;
                    properties.name = sequence.name.c_str();
                    sequence.ara = std::make_unique<Model::RegionSequence>(SequenceRef::toHostRef(&sequence), dc, properties);
                }
                else if (sequence.name != name)
                {
                    sequence.name = name;
                    properties.name = sequence.name.c_str();
                    sequence.ara->update(properties);
                }
            }
            // Clips new or changed.
            for (const auto& [clipId, w] : wanted)
            {
                auto& model = document.clips[clipId];
                const bool created = model == nullptr;
                if (created)
                {
                    model = std::make_unique<ClipModel>();
                    model->clipId = clipId;
                    model->trackId = w.trackId;
                    model->source = sourceFor(document, w);
                    ++model->source->users;
                    model->persistentId = "clip-" + std::to_string(clipId);
                    auto properties = Model::AudioModification::getEmptyProperties();
                    properties.name = w.name.c_str();
                    properties.persistentID = model->persistentId.c_str();
                    model->modification = std::make_unique<Model::AudioModification>(ModificationRef::toHostRef(model.get()), dc,
                                                                                       *model->source->ara, properties);
                }
                const bool changed = created || model->name != w.name || std::abs(model->start - w.start) > 1.0e-9
                                     || std::abs(model->length - w.length) > 1.0e-9 || std::abs(model->offset - w.offset) > 1.0e-9
                                     || std::abs(model->stretch - w.stretch) > 1.0e-9;
                if (!changed)
                    continue;
                model->name = w.name;
                model->start = w.start;
                model->length = w.length;
                model->offset = w.offset;
                model->stretch = w.stretch;
                auto properties = Model::PlaybackRegion::getEmptyProperties();
                const bool stretched = std::abs(w.stretch - 1.0) > 1.0e-9;
                properties.transformationFlags =
                    stretched && (document.transformations & ARA::kARAPlaybackTransformationTimestretch) != 0
                        ? static_cast<ARA::ARAPlaybackTransformationFlags>(ARA::kARAPlaybackTransformationTimestretch)
                        : static_cast<ARA::ARAPlaybackTransformationFlags>(ARA::kARAPlaybackTransformationNoChanges);
                properties.startInModificationTime = w.offset;
                properties.durationInModificationTime = w.length / w.stretch; // (stretch: timeline seconds per source second)
                properties.startInPlaybackTime = w.start;
                properties.durationInPlaybackTime = w.length;
                properties.musicalContextRef = document.context->getPluginRef();
                properties.regionSequenceRef = document.sequences[w.trackId].ara->getPluginRef();
                properties.name = model->name.c_str();
                properties.color = nullptr;
                if (model->region == nullptr)
                    model->region = std::make_unique<Model::PlaybackRegion>(ClipRef::toHostRef(model.get()), dc, *model->modification, properties);
                else
                    model->region->update(properties);
            }
            // What the plug-in did with the clips that moved: their modification's state, and their old
            // source's where the new one is theirs alone, new and reading the same audio (native
            // edits added before they rendered, or removed while bypassed).
            for (const auto& transfer : transfers)
            {
                const auto model = document.clips.find(transfer.clipId);
                if (model == document.clips.end() || model->second->modification == nullptr)
                    continue;
                const auto* source = model->second->source;
                const bool withSource = source->fresh && source->users == 1
                                        && source->file == transfer.file;
                const ARA::ARAPersistentID archivedSources[] = {transfer.sourceId.c_str()};
                const ARA::ARAPersistentID currentSources[] = {source->persistentId.c_str()};
                const ARA::ARAPersistentID archivedModifications[] = {transfer.modificationId.c_str()};
                const ARA::ARAPersistentID currentModifications[] = {model->second->persistentId.c_str()};
                ARA::ARARestoreObjectsFilter filter{};
                filter.structSize = ARA::kARARestoreObjectsFilterMinSize;
                filter.documentData = ARA::kARAFalse;
                filter.audioSourceIDsCount = withSource ? 1u : 0u;
                filter.audioSourceArchiveIDs = withSource ? archivedSources : nullptr;
                filter.audioSourceCurrentIDs = withSource ? currentSources : nullptr;
                filter.audioModificationIDsCount = 1;
                filter.audioModificationArchiveIDs = archivedModifications;
                filter.audioModificationCurrentIDs = currentModifications;
                ArchiveReader reader;
                reader.data = transfer.archive;
                reader.archiveId = document.archiveId;
                dc.restoreObjectsFromArchive(ArchiveReaderRef::toHostRef(&reader), &filter);
            }
            // Sources and sequences nothing uses any more.
            for (auto it = document.sources.begin(); it != document.sources.end();)
            {
                if (it->second->users <= 0)
                {
                    it->second->ara->enableAudioSourceSamplesAccess(false);
                    it->second->ara.reset();
                    it = document.sources.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            for (auto it = document.sequences.begin(); it != document.sequences.end();)
                it = trackNames.count(it->first) == 0 ? document.sequences.erase(it) : std::next(it);
        }
        for (auto* source : refilled)
            source->ara->enableAudioSourceSamplesAccess(true);
        for (auto& [key, source] : document.sources)
            source->fresh = false;

        // 3. The edits saved with the session, once every region they may belong to is there (before the
        //    plug-ins are prepared with the regions, so that they find them restored).
        if (!document.pendingArchive.isEmpty() && !waiting && !document.clips.empty())
        {
            ArchiveReader reader;
            reader.data = std::move(document.pendingArchive);
            reader.archiveId = document.archiveId;
            document.pendingArchive.reset();
            const juce::ARAEditGuard guard(dc);
            dc.restoreObjectsFromArchive(ArchiveReaderRef::toHostRef(&reader), nullptr);
        }

        // 4. Plug-ins take the regions they gain: out of play.
        for (auto& [deviceId, binding] : bindings)
        {
            if (binding.document != &document)
                continue;
            std::vector<ClipModel*> gaining;
            for (const auto id : assign[deviceId])
                if (binding.assigned.count(id) == 0)
                    if (const auto model = document.clips.find(id); model != document.clips.end() && model->second->region != nullptr)
                        gaining.push_back(model->second.get());
            if (!gaining.empty())
                reconfigure(deviceId,
                            [&]
                            {
                                for (auto* model : gaining)
                                {
                                    if (binding.playback != nullptr)
                                        binding.playback->add(*model->region);
                                    if (binding.editor != nullptr)
                                        binding.editor->add(*model->region);
                                    binding.assigned.insert(model->clipId);
                                }
                            });
        }
    }

    Source* sourceFor(Document& document, const Wanted& w)
    {
        const auto& key = w.sourceKey;
        if (const auto it = document.sources.find(key); it != document.sources.end())
        {
            auto& known = *it->second;
            if (known.frames == w.asset->numFrames && known.channels == std::max(1, w.asset->numChannels)
                && std::abs(known.sampleRate - w.asset->sampleRate) < 1.0e-6)
                return &known;
            // Its audio changed shape (so its clips all let it go): a new source for the new audio.
            known.ara->enableAudioSourceSamplesAccess(false);
            known.ara.reset();
            document.sources.erase(it);
        }
        auto source = std::make_unique<Source>();
        source->key = key;
        source->fresh = true;
        source->file = w.file;
        source->asset = w.asset;
        source->formats = &assets.getFormatManager();
        source->sampleRate = w.asset->sampleRate;
        source->channels = std::max(1, w.asset->numChannels);
        source->frames = w.asset->numFrames;
        source->name = w.sourceName;
        source->persistentId = w.sourceId;
        auto properties = Model::AudioSource::getEmptyProperties();
        properties.name = source->name.c_str();
        properties.persistentID = source->persistentId.c_str();
        properties.sampleCount = source->frames;
        properties.sampleRate = source->sampleRate;
        properties.channelCount = source->channels;
        properties.merits64BitSamples = false;
        source->ara = std::make_unique<Model::AudioSource>(SourceRef::toHostRef(source.get()), document.dc(), properties);
        source->ara->enableAudioSourceSamplesAccess(true);
        auto* raw = source.get();
        document.sources[key] = std::move(source);
        return raw;
    }

    // --- Persistence -------------------------------------------------------------------------------

    void storeArchives()
    {
        synchronise();
        for (auto& [id, document] : documents)
            storeArchive(*document);
    }

    /** A document's archive into the session (not undoable: it is what the plug-in knows). */
    void storeArchive(Document& document)
    {
        juce::MemoryOutputStream archive;
        if (!document.dc().storeObjectsToArchive(ArchiveWriterRef::toHostRef(&archive), nullptr))
            return;
        auto root = session.getState().getOrCreateChildWithName(ids::araDocuments, nullptr);
        auto stored = juce::ValueTree();
        for (const auto& child : root)
            if (child.getProperty(ids::araFactory).toString() == document.factoryId)
                stored = child;
        if (!stored.isValid())
        {
            stored = juce::ValueTree(ids::araDocument);
            stored.setProperty(ids::araFactory, document.factoryId, nullptr);
            root.appendChild(stored, nullptr);
        }
        stored.setProperty(ids::araArchive, archive.getMemoryBlock().toBase64Encoding(), nullptr);
    }

    void reset()
    {
        const bool hadBindings = !bindings.empty();
        for (auto& [deviceId, binding] : bindings)
            release(binding);
        bindings.clear();
        if (hadBindings)
            owner.sendSynchronousChangeMessage(); // (editors of the plug-ins go now)
        for (auto& [id, document] : documents)
            document->takeDown(); // (its plug-ins are still alive: their modules are loaded)
        documents.clear();
        problems.clear();
        if (!rendering.empty())
        {
            rendering.clear();
            if (owner.onRenderingChanged)
                owner.onRenderingChanged();
        }
    }

    // --- The clip editor ---------------------------------------------------------------------------

    void showSelection(std::uint32_t trackId, const std::vector<std::uint32_t>& clipIds)
    {
        for (auto& [deviceId, binding] : bindings)
        {
            if (binding.trackId != trackId || binding.extension == nullptr || binding.extension->editorViewRef == nullptr)
                continue;
            std::vector<ARA::ARAPlaybackRegionRef> regions;
            for (const auto id : clipIds)
                if (const auto model = binding.document->clips.find(id); model != binding.document->clips.end() && model->second->region != nullptr)
                    regions.push_back(model->second->region->getPluginRef());
            std::vector<ARA::ARARegionSequenceRef> sequences;
            if (const auto s = binding.document->sequences.find(trackId); s != binding.document->sequences.end() && s->second.ara != nullptr)
                sequences.push_back(s->second.ara->getPluginRef());
            ARA::ARAViewSelection selection{};
            selection.structSize = ARA::kARAViewSelectionMinSize;
            selection.playbackRegionRefsCount = regions.size();
            selection.playbackRegionRefs = regions.empty() ? nullptr : regions.data();
            selection.regionSequenceRefsCount = sequences.size();
            selection.regionSequenceRefs = sequences.empty() ? nullptr : sequences.data();
            selection.timeRange = nullptr;
            ARA::Host::EditorView view{binding.extension};
            view.notifySelection(&selection);
        }
    }

    TrackInfo getTrackInfo(std::uint32_t trackId) const
    {
        TrackInfo info;
        for (const auto& [deviceId, binding] : bindings)
            if (binding.trackId == trackId)
            {
                info.bound = true;
                info.plugin = binding.pluginName;
                info.deviceId = deviceId;
                info.regions = static_cast<int>(binding.assigned.size());
            }
        info.rendering = rendering.count(trackId) != 0;
        if (const auto p = problems.find(trackId); p != problems.end() && !info.bound)
            info.problem = p->second;
        return info;
    }

    // --- Listeners: follow the session (coalesced) ---------------------------------------------------

    void handleAsyncUpdate() override { synchronise(); }
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier& property) override
    {
        if (!bindings.empty() && property != ids::state && property != ids::araArchive)
            triggerAsyncUpdate();
    }
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override
    {
        if (!bindings.empty())
            triggerAsyncUpdate();
    }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override
    {
        if (!bindings.empty())
            triggerAsyncUpdate();
    }
    void valueTreeChildOrderChanged(juce::ValueTree&, int, int) override
    {
        if (!bindings.empty())
            triggerAsyncUpdate();
    }
    void audioAssetLoaded(const juce::File&) override
    {
        if (!bindings.empty())
            triggerAsyncUpdate();
    }

    AraHost& owner;
    Session& session;
    AudioAssetCache& assets;
    DeviceManager& devices;
    std::shared_ptr<SharedTempoMap> tempo = std::make_shared<SharedTempoMap>();
    std::shared_ptr<TransportRequests> transport;
    std::map<juce::String, std::unique_ptr<Document>> documents; ///< by factory id
    std::map<std::uint32_t, Binding> bindings;                   ///< by device id
    std::map<std::uint32_t, juce::String> problems;              ///< by track id: why its plug-in is not bound
    std::set<std::uint32_t> rendering;                           ///< tracks whose clips the plug-ins play
};

AraHost::AraHost(Session& session, AudioAssetCache& assets, DeviceManager& devices)
    : impl_(std::make_unique<Impl>(*this, session, assets, devices))
{
}

AraHost::~AraHost() = default;

bool AraHost::isAvailable() noexcept { return true; }
bool AraHost::isAraPlugin(const juce::PluginDescription& description) noexcept { return description.hasARAExtension; }
void AraHost::deviceCreated(const juce::ValueTree& device, juce::AudioProcessor& processor) { impl_->deviceCreated(device, processor); }
void AraHost::deviceRemoved(std::uint32_t deviceId) { impl_->deviceRemoved(deviceId); }
bool AraHost::rendersClipsOf(std::uint32_t trackId) const { return impl_->rendering.count(trackId) != 0; }
void AraHost::synchronise() { impl_->synchronise(); }
void AraHost::storeArchives() { impl_->storeArchives(); }
void AraHost::reset() { impl_->reset(); }
void AraHost::showSelection(std::uint32_t trackId, const std::vector<std::uint32_t>& clipIds) { impl_->showSelection(trackId, clipIds); }
AraHost::TrackInfo AraHost::getTrackInfo(std::uint32_t trackId) const { return impl_->getTrackInfo(trackId); }

#else // without ARA: nothing is ever bound

struct AraHost::Impl
{
};

AraHost::AraHost(Session&, AudioAssetCache&, DeviceManager&) {}
AraHost::~AraHost() = default;
bool AraHost::isAvailable() noexcept { return false; }
bool AraHost::isAraPlugin(const juce::PluginDescription&) noexcept { return false; }
void AraHost::deviceCreated(const juce::ValueTree&, juce::AudioProcessor&) {}
void AraHost::deviceRemoved(std::uint32_t) {}
bool AraHost::rendersClipsOf(std::uint32_t) const { return false; }
void AraHost::synchronise() {}
void AraHost::storeArchives() {}
void AraHost::reset() {}
void AraHost::showSelection(std::uint32_t, const std::vector<std::uint32_t>&) {}
AraHost::TrackInfo AraHost::getTrackInfo(std::uint32_t) const { return {}; }

#endif

} // namespace dojo::session
