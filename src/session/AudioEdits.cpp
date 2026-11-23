#include "session/AudioEdits.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace dojo::session
{
using namespace audioedit;

namespace
{
juce::String encodeCurve(const std::vector<float>& values)
{
    juce::MemoryBlock block(values.data(), values.size() * sizeof(float));
    return block.toBase64Encoding();
}

std::vector<float> decodeCurve(const juce::var& text)
{
    juce::MemoryBlock block;
    if (!block.fromBase64Encoding(text.toString()))
        return {};
    std::vector<float> values(block.getSize() / sizeof(float));
    if (!values.empty())
        std::memcpy(values.data(), block.getData(), values.size() * sizeof(float));
    return values;
}

/** Every clip in a tree (on tracks, in take lanes...) with this id. */
juce::ValueTree findClipIn(const juce::ValueTree& tree, std::uint32_t id)
{
    for (const auto& child : tree)
    {
        if (child.hasType(ids::clip) && clip::getId(child) == id)
            return child;
        if (!child.hasType(ids::clip) && child.getNumChildren() > 0)
            if (auto found = findClipIn(child, id); found.isValid())
                return found;
    }
    return {};
}

void forEachEditedClip(const juce::ValueTree& tree, const std::function<void(juce::ValueTree)>& visit)
{
    for (auto child : tree)
    {
        if (child.hasType(ids::clip))
        {
            if (child.getChildWithName(ids::audioEdit).isValid())
                visit(child);
        }
        else if (child.getNumChildren() > 0)
            forEachEditedClip(child, visit);
    }
}

bool isCancelled(const std::shared_ptr<std::atomic<bool>>& cancel) { return cancel != nullptr && cancel->load(); }
} // namespace

// The part of a source being edited, decoded, with its renderers' caches.
struct AudioEdits::Material
{
    juce::File source;
    double from = 0.0;
    double to = 0.0;
    double sampleRate = 44100.0;
    juce::int64 first = 0; ///< the sample of `from`
    juce::AudioBuffer<float> audio;
    std::mutex lock; ///< (one render at a time; guards what follows)
    pitch::Renderer pitch;
    std::shared_ptr<const juce::AudioBuffer<float>> processed; ///< with the spectral edits
    std::uint64_t processedKey = 0;
    std::uint64_t pitchInputKey = 0; ///< what the pitch renderer's cache was made from

    /** The audio with spectral operations applied (kept for the next time); the lock is held. Null if
        cancelled. */
    std::shared_ptr<const juce::AudioBuffer<float>> withSpectral(const std::vector<spectral::Operation>& ops, const std::atomic<bool>* cancel)
    {
        const auto key = spectral::keyOf(ops) + 1;
        if (processed != nullptr && processedKey == key)
            return processed;
        auto result = std::make_shared<juce::AudioBuffer<float>>(spectral::apply(audio, sampleRate, ops, cancel));
        if (cancel != nullptr && cancel->load())
            return {};
        processed = result;
        processedKey = key;
        return processed;
    }

    static std::shared_ptr<Material> load(juce::AudioFormatManager& formats, const juce::File& source, double from, double to, juce::String& error)
    {
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(source));
        if (reader == nullptr)
        {
            error = "Could not read " + source.getFullPathName();
            return {};
        }
        auto m = std::make_shared<Material>();
        m->source = source;
        m->from = from;
        m->to = to;
        m->sampleRate = reader->sampleRate;
        m->first = std::clamp<juce::int64>(static_cast<juce::int64>(std::llround(from * reader->sampleRate)), 0, reader->lengthInSamples);
        const auto count = std::clamp<juce::int64>(static_cast<juce::int64>(std::llround((to - from) * reader->sampleRate)), 0,
                                                   reader->lengthInSamples - m->first);
        m->audio.setSize(static_cast<int>(reader->numChannels), static_cast<int>(count));
        if (!reader->read(&m->audio, 0, static_cast<int>(count), m->first, true, true))
        {
            error = "Could not read " + source.getFullPathName();
            return {};
        }
        return m;
    }

    [[nodiscard]] bool covers(const juce::File& file, double f, double t) const { return file == source && std::abs(f - from) < 1.0e-9 && std::abs(t - to) < 1.0e-9; }
};

AudioEdits::AudioEdits(Session& session, AudioAssetCache& assets)
    : session_(session)
    , assets_(assets)
    , renderFolder_(juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("The Dojo").getChildFile("Edits"))
{
    session_.getState().addListener(this);
}

AudioEdits::~AudioEdits()
{
    session_.getState().removeListener(this);
    for (auto& [id, entry] : entries_)
        if (entry.cancel != nullptr)
            entry.cancel->store(true);
    pool_.removeAllJobs(true, 30000);
}

AudioEdits::State AudioEdits::getState(const juce::ValueTree& clip) const
{
    if (!clip.isValid())
        return {};
    const auto found = entries_.find(clip::getId(clip));
    if (found == entries_.end())
        return {};
    const auto& entry = found->second;
    State state;
    state.status = entry.status;
    state.progress = entry.progress->load();
    state.problem = entry.problem;
    if (state.status == Status::idle && (entry.rendering || entry.dirty))
        state.status = Status::rendering;
    return state;
}

juce::ValueTree AudioEdits::clipOf(juce::ValueTree tree)
{
    while (tree.isValid() && !tree.hasType(ids::clip))
        tree = tree.getParent();
    return tree;
}

bool AudioEdits::regionOf(const juce::ValueTree& clip, Region& region, juce::String& error) const
{
    if (!clip.isValid() || !clip.hasType(ids::clip))
    {
        error = "Audio editing works on audio clips.";
        return false;
    }
    region.source = audioedits::getSource(clip);
    double fileLength = 0.0;
    {
        std::unique_ptr<juce::AudioFormatReader> reader(assets_.getFormatManager().createReaderFor(region.source));
        if (reader == nullptr || reader->sampleRate <= 0.0)
        {
            error = "Could not read " + region.source.getFullPathName();
            return false;
        }
        fileLength = static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
    }
    const double s0 = clip::sourceAt(clip, 0.0);
    const double s1 = clip::sourceAt(clip, clip::getLoopLength(clip));
    region.from = std::clamp(std::min(s0, s1) - 0.2, 0.0, fileLength);
    region.to = std::clamp(std::max(s0, s1) + 0.2, 0.0, fileLength);
    // (an edit keeps the part it was made on, while it covers the clip)
    const auto edit = audioedits::getEdit(clip);
    if (edit.isValid())
    {
        const double from = static_cast<double>(edit.getProperty(ids::from, region.from));
        const double to = static_cast<double>(edit.getProperty(ids::to, region.to));
        if (from <= region.from + 1.0e-9 && to >= region.to - 1.0e-9)
        {
            region.from = from;
            region.to = to;
        }
    }
    if (region.to - region.from < 0.05)
    {
        error = "The clip plays too little of its file.";
        return false;
    }
    return true;
}

std::shared_ptr<AudioEdits::Material> AudioEdits::materialFor(std::uint32_t clipId, const Region& region)
{
    const auto found = entries_.find(clipId);
    if (found != entries_.end() && found->second.material != nullptr && found->second.material->covers(region.source, region.from, region.to))
        return found->second.material;
    return {};
}

std::vector<spectral::Operation> AudioEdits::spectralOperationsOf(const juce::ValueTree& clip)
{
    std::vector<spectral::Operation> ops;
    const double from = audioedits::getFrom(clip);
    for (const auto& op : audioedits::getSpectralOperations(clip))
        ops.push_back(audioedits::toOperation(op, from));
    return ops;
}

bool AudioEdits::analysePitch(const juce::ValueTree& clip, pitch::Mode mode, juce::String& error)
{
    Region region;
    if (!regionOf(clip, region, error))
        return false;
    const auto source = region.source;
    const double from = region.from;
    const double to = region.to;
    // (the spectral edits, at the times of this region)
    std::vector<spectral::Operation> ops;
    for (const auto& op : audioedits::getSpectralOperations(clip))
        ops.push_back(audioedits::toOperation(op, from));

    const auto id = clip::getId(clip);
    auto& entry = entries_[id];
    if (entry.cancel != nullptr)
        entry.cancel->store(true);
    entry.cancel = std::make_shared<std::atomic<bool>>(false);
    entry.generation += 1;
    entry.status = Status::analysing;
    entry.problem.clear();
    entry.progress->store(0.0f);
    entry.dirty = false;
    auto material = entry.material != nullptr && entry.material->covers(source, from, to) ? entry.material : nullptr;

    pool_.addJob([weak = juce::WeakReference<AudioEdits>(this), formats = &assets_.getFormatManager(), id, generation = entry.generation, cancel = entry.cancel,
                  progress = entry.progress, material, source, from, to, mode, ops]() mutable
                 {
                     juce::String problem;
                     if (material == nullptr)
                         material = Material::load(*formats, source, from, to, problem);
                     std::vector<pitch::Note> notes;
                     std::shared_ptr<const juce::AudioBuffer<float>> input;
                     if (material != nullptr)
                     {
                         const std::lock_guard<std::mutex> lock(material->lock);
                         input = material->withSpectral(ops, cancel.get());
                     }
                     if (input != nullptr && !isCancelled(cancel))
                     {
                         const int count = input->getNumSamples();
                         const int channels = input->getNumChannels();
                         std::vector<float> mono(static_cast<std::size_t>(count), 0.0f);
                         for (int ch = 0; ch < channels; ++ch)
                         {
                             const float* in = input->getReadPointer(ch);
                             for (int i = 0; i < count; ++i)
                                 mono[static_cast<std::size_t>(i)] += in[i] / static_cast<float>(channels);
                         }
                         notes = pitch::analyse(mono.data(), count, material->sampleRate, mode, cancel.get(), [progress](float p) { progress->store(p); });
                     }
                     if (isCancelled(cancel))
                         return juce::ThreadPoolJob::jobHasFinished;
                     juce::MessageManager::callAsync([weak, id, generation, material, found = std::move(notes), mode, problem]() mutable
                                                     {
                                                         if (auto* self = weak.get())
                                                             self->analysisDone(id, generation, std::move(material), std::move(found), mode, problem);
                                                     });
                     return juce::ThreadPoolJob::jobHasFinished;
                 });
    sendChangeMessage();
    return true;
}

void AudioEdits::analysisDone(std::uint32_t clipId, int generation, std::shared_ptr<Material> material, std::vector<pitch::Note> notes,
                              pitch::Mode mode, juce::String error)
{
    const auto found = entries_.find(clipId);
    if (found == entries_.end() || found->second.generation != generation)
        return;
    auto& entry = found->second;
    auto clip = findClipIn(session_.getState(), clipId);
    if (!clip.isValid() || material == nullptr || error.isNotEmpty())
    {
        entry.status = Status::failed;
        entry.problem = error.isNotEmpty() ? error : juce::String("The clip is gone.");
        sendChangeMessage();
        return;
    }
    entry.material = material;
    entry.status = Status::idle;
    entry.progress->store(1.0f);

    auto* um = &session_.getUndoManager();
    session_.beginTransaction("Analyse Pitch");
    auto edit = audioedits::getEdit(clip);
    if (!edit.isValid())
    {
        juce::ValueTree fresh(ids::audioEdit);
        fresh.setProperty(ids::source, clip::getFile(clip).getFullPathName(), nullptr);
        fresh.setProperty(ids::from, material->from, nullptr);
        fresh.setProperty(ids::to, material->to, nullptr);
        clip.appendChild(fresh, um);
        edit = fresh;
    }
    else
    {
        edit.setProperty(ids::from, material->from, um);
        edit.setProperty(ids::to, material->to, um);
        if (auto old = edit.getChildWithName(ids::pitchEdit); old.isValid())
            edit.removeChild(old, um);
    }
    juce::ValueTree pitchTree(ids::pitchEdit);
    pitchTree.setProperty(ids::mode, mode == pitch::Mode::polyphonic ? "polyphonic" : "melodic", nullptr);
    pitchTree.setProperty(ids::formants, true, nullptr);
    for (const auto& note : notes)
        pitchTree.appendChild(audioedits::makeNote(note), nullptr);
    edit.appendChild(pitchTree, um);
    sendChangeMessage();
}

void AudioEdits::removePitch(juce::ValueTree clip)
{
    auto edit = audioedits::getEdit(clip);
    if (!edit.isValid())
        return;
    session_.beginTransaction("Remove Pitch Edits");
    removePart(clip, edit.getChildWithName(ids::pitchEdit));
}

void AudioEdits::removeSpectralOperation(juce::ValueTree op)
{
    auto spectralTree = op.getParent();
    const auto clip = clipOf(op);
    if (!spectralTree.isValid() || !clip.isValid())
        return;
    session_.beginTransaction("Remove Spectral Edit");
    if (spectralTree.getNumChildren() > 1)
        spectralTree.removeChild(op, &session_.getUndoManager());
    else
        removePart(clip, spectralTree);
}

void AudioEdits::removePart(juce::ValueTree clip, juce::ValueTree part)
{
    auto edit = audioedits::getEdit(clip);
    auto* um = &session_.getUndoManager();
    // The last part: the whole edit goes, and the clip plays its own file.
    int others = 0;
    for (const auto& child : edit)
        if (child != part && (child.hasType(ids::pitchEdit) || (child.hasType(ids::spectralEdit) && child.getNumChildren() > 0)))
            ++others;
    if (others == 0)
    {
        clip.setProperty(ids::file, audioedits::getSource(clip).getFullPathName(), um);
        clip.removeChild(edit, um);
    }
    else if (part.isValid())
        edit.removeChild(part, um);
}

void AudioEdits::setBypassed(juce::ValueTree clip, bool bypassed)
{
    auto edit = audioedits::getEdit(clip);
    if (!edit.isValid() || isBypassed(clip) == bypassed)
        return;
    edit.setProperty(ids::bypass, bypassed, nullptr);
}

bool AudioEdits::isBypassed(const juce::ValueTree& clip) { return static_cast<bool>(audioedits::getEdit(clip).getProperty(ids::bypass, false)); }

bool AudioEdits::isBusy() const
{
    if (pendingLoads_ > 0 || pendingSpectrograms_ > 0)
        return true;
    for (const auto& [id, entry] : entries_)
        if (entry.status == Status::analysing || entry.rendering || entry.dirty)
            return true;
    return false;
}

bool AudioEdits::waitUntilIdle(int timeoutMs)
{
    const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
    while (isBusy())
    {
        if (isTimerRunning())
            timerCallback();
        if (juce::Time::getMillisecondCounterHiRes() > end)
            return false;
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
    }
    return true;
}

void AudioEdits::sessionLoaded()
{
    forEachEditedClip(session_.getState(),
                      [this](juce::ValueTree clip)
                      {
                          auto edit = audioedits::getEdit(clip);
                          const juce::File rendered(edit.getProperty(ids::rendered).toString());
                          const bool playsRender = rendered != juce::File() && clip::getFile(clip) == rendered;
                          if (playsRender && rendered.existsAsFile())
                              return;
                          clip.setProperty(ids::file, audioedits::getSource(clip).getFullPathName(), nullptr);
                          edit.setProperty(ids::rendered, juce::String(), nullptr);
                          markDirty(clip);
                      });
}

void AudioEdits::reset()
{
    for (auto& [id, entry] : entries_)
        if (entry.cancel != nullptr)
            entry.cancel->store(true);
    entries_.clear();
    stopTimer();
    sendChangeMessage();
}

// --- Spectral editing --------------------------------------------------------------------------------------

juce::ValueTree AudioEdits::addSpectralOperation(juce::ValueTree clip, spectral::Operation op, const juce::String& undoName)
{
    Region region;
    juce::String error;
    if (!regionOf(clip, region, error))
        return {};
    auto* um = &session_.getUndoManager();
    session_.beginTransaction(undoName);
    auto edit = audioedits::getEdit(clip);
    if (!edit.isValid())
    {
        juce::ValueTree fresh(ids::audioEdit);
        fresh.setProperty(ids::source, region.source.getFullPathName(), nullptr);
        fresh.setProperty(ids::from, region.from, nullptr);
        fresh.setProperty(ids::to, region.to, nullptr);
        fresh.appendChild(juce::ValueTree(ids::spectralEdit), nullptr);
        clip.appendChild(fresh, um);
        edit = fresh;
    }
    auto spectralTree = edit.getChildWithName(ids::spectralEdit);
    if (!spectralTree.isValid())
    {
        spectralTree = juce::ValueTree(ids::spectralEdit);
        edit.appendChild(spectralTree, um);
    }
    // Seconds of the clip to seconds of the source.
    const double a = clip::sourceAt(clip, std::min(op.start, op.end));
    const double b = clip::sourceAt(clip, std::max(op.start, op.end));
    op.start = std::min(a, b);
    op.end = std::max(a, b);
    auto tree = audioedits::makeOperation(op);
    spectralTree.appendChild(tree, um);
    return tree;
}

std::vector<float> AudioEdits::learnNoise(const juce::ValueTree& clip, double clipStart, double clipEnd, juce::String& error)
{
    Region region;
    if (!regionOf(clip, region, error))
        return {};
    auto material = materialFor(clip::getId(clip), region);
    if (material == nullptr)
    {
        material = Material::load(assets_.getFormatManager(), region.source, region.from, region.to, error);
        if (material == nullptr)
            return {};
        entries_[clip::getId(clip)].material = material;
    }
    const double a = clip::sourceAt(clip, std::min(clipStart, clipEnd)) - region.from;
    const double b = clip::sourceAt(clip, std::max(clipStart, clipEnd)) - region.from;
    if (b - a < 0.02)
    {
        error = "Select at least 20 ms of noise alone.";
        return {};
    }
    return spectral::learnProfile(material->audio, material->sampleRate, a, b);
}

std::shared_ptr<const spectral::Spectrogram> AudioEdits::getSpectrogram(const juce::ValueTree& clip)
{
    Region region;
    juce::String error;
    if (!regionOf(clip, region, error))
        return {};
    std::vector<spectral::Operation> ops;
    for (const auto& op : audioedits::getSpectralOperations(clip))
        ops.push_back(audioedits::toOperation(op, region.from));
    const auto id = clip::getId(clip);
    auto& entry = entries_[id];
    const auto key = static_cast<std::uint64_t>((region.source.getFullPathName() + ":" + juce::String(region.from) + ":" + juce::String(region.to) + ":"
                                                  + juce::String(static_cast<juce::int64>(spectral::keyOf(ops))))
                                                     .hashCode64());
    if (entry.spectrogramKey == key || entry.spectrogramWanted == key)
        return entry.spectrogram;
    entry.spectrogramWanted = key;
    ++pendingSpectrograms_;
    pool_.addJob([weak = juce::WeakReference<AudioEdits>(this), formats = &assets_.getFormatManager(), id, key, region, ops,
                  material = materialFor(id, region)]() mutable
                 {
                     juce::String problem;
                     if (material == nullptr)
                         material = Material::load(*formats, region.source, region.from, region.to, problem);
                     std::shared_ptr<spectral::Spectrogram> result;
                     if (material != nullptr)
                     {
                         std::shared_ptr<const juce::AudioBuffer<float>> input;
                         {
                             const std::lock_guard<std::mutex> lock(material->lock);
                             input = material->withSpectral(ops, nullptr);
                         }
                         if (input != nullptr)
                             result = std::make_shared<spectral::Spectrogram>(spectral::analyse(*input, material->sampleRate));
                     }
                     juce::MessageManager::callAsync([weak, id, key, material, result, from = region.from]
                                                     {
                                                         auto* self = weak.get();
                                                         if (self == nullptr)
                                                             return;
                                                         --self->pendingSpectrograms_;
                                                         auto& e = self->entries_[id];
                                                         if (e.spectrogramWanted == key && result != nullptr)
                                                         {
                                                             e.spectrogram = result;
                                                             e.spectrogramKey = key;
                                                             e.spectrogramStart = from;
                                                             if (e.material == nullptr && material != nullptr)
                                                                 e.material = material;
                                                         }
                                                         if (e.spectrogramWanted == key)
                                                             e.spectrogramWanted = 0;
                                                         self->sendChangeMessage();
                                                     });
                     return juce::ThreadPoolJob::jobHasFinished;
                 });
    return entry.spectrogram;
}

double AudioEdits::getSpectrogramStart(const juce::ValueTree& clip) const
{
    const auto found = entries_.find(clip::getId(clip));
    return found != entries_.end() ? found->second.spectrogramStart : 0.0;
}

// --- Rendering ---------------------------------------------------------------------------------------------

void AudioEdits::markDirty(const juce::ValueTree& clip)
{
    if (!clip.isValid() || !clip.hasType(ids::clip))
        return;
    auto& entry = entries_[clip::getId(clip)];
    entry.dirty = true;
    if (entry.rendering && entry.cancel != nullptr)
        entry.cancel->store(true); // (restarted with the newest edits)
    startTimer(120);
}

void AudioEdits::timerCallback()
{
    stopTimer();
    for (auto& [id, entry] : entries_)
    {
        if (!entry.dirty || entry.rendering || entry.status == Status::analysing)
            continue;
        const auto clip = findClipIn(session_.getState(), id);
        if (!clip.isValid() || !audioedits::getEdit(clip).isValid())
        {
            entry.dirty = false;
            continue;
        }
        startRender(clip, entry);
    }
}

void AudioEdits::startRender(const juce::ValueTree& clip, Entry& entry)
{
    entry.dirty = false;
    const auto edit = audioedits::getEdit(clip);
    const auto source = audioedits::getSource(clip);
    std::vector<pitch::Edit> edits;
    for (const auto& note : audioedits::getPitchNotes(clip))
        edits.push_back(audioedits::toEdit(note));
    const auto ops = spectralOperationsOf(clip);
    const bool anyPitchEdit = std::any_of(edits.begin(), edits.end(), [](const pitch::Edit& e) { return e.edited(); });
    const bool anySpectralEdit = std::any_of(ops.begin(), ops.end(), [](const spectral::Operation& o) { return o.enabled; });
    if (isBypassed(clip) || (!anyPitchEdit && !anySpectralEdit))
    {
        play(clip, source);
        entry.status = Status::idle;
        sendChangeMessage();
        return;
    }

    pitch::RenderOptions options;
    options.mode = audioedits::getPitchMode(clip);
    options.preserveFormants = audioedits::keepsFormants(clip);
    const double from = static_cast<double>(edit.getProperty(ids::from, 0.0));
    const double to = static_cast<double>(edit.getProperty(ids::to, 0.0));
    auto material = entry.material != nullptr && entry.material->covers(source, from, to) ? entry.material : nullptr;
    if (!renderFolder_.createDirectory())
    {
        entry.status = Status::failed;
        entry.problem = "Could not create " + renderFolder_.getFullPathName();
        sendChangeMessage();
        return;
    }
    const auto target = renderFolder_.getNonexistentChildFile(juce::File::createLegalFileName(clip::getName(clip)) + " (edited)", ".wav", false);
    target.create(); // (reserved: the next render picks another name)

    entry.rendering = true;
    entry.generation += 1;
    entry.cancel = std::make_shared<std::atomic<bool>>(false);
    entry.problem.clear();
    pool_.addJob(
        [weak = juce::WeakReference<AudioEdits>(this), formats = &assets_.getFormatManager(), id = clip::getId(clip), generation = entry.generation,
         cancel = entry.cancel, material, source, from, to, noteEdits = std::move(edits), ops, options, target]() mutable
        {
            juce::String problem;
            if (material == nullptr)
                material = Material::load(*formats, source, from, to, problem);
            bool written = false;
            if (material != nullptr)
            {
                const std::lock_guard<std::mutex> lock(material->lock);
                juce::AudioBuffer<float> rendered;
                // The spectral edits first, then the pitch edits on what they leave.
                const auto input = material->withSpectral(ops, cancel.get());
                if (input != nullptr && material->pitchInputKey != material->processedKey)
                {
                    material->pitch.clear();
                    material->pitchInputKey = material->processedKey;
                }
                if (input != nullptr && material->pitch.render(*input, material->sampleRate, noteEdits, options, rendered, cancel.get()))
                {
                    // The whole source, with the analysed part replaced by its render.
                    std::unique_ptr<juce::AudioFormatReader> reader(formats->createReaderFor(source));
                    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(target);
                    juce::WavAudioFormat wav;
                    std::unique_ptr<juce::AudioFormatWriter> writer;
                    if (reader != nullptr && static_cast<juce::FileOutputStream*>(stream.get())->openedOk())
                    {
                        static_cast<juce::FileOutputStream*>(stream.get())->setPosition(0);
                        static_cast<juce::FileOutputStream*>(stream.get())->truncate();
                        writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                                 .withSampleRate(reader->sampleRate)
                                                                 .withNumChannels(static_cast<int>(reader->numChannels))
                                                                 .withBitsPerSample(32)
                                                                 .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
                    }
                    if (writer != nullptr)
                    {
                        constexpr int kChunk = 65536;
                        juce::AudioBuffer<float> chunk(static_cast<int>(reader->numChannels), kChunk);
                        const auto copy = [&](juce::int64 start, juce::int64 end)
                        {
                            for (auto at = start; at < end && !isCancelled(cancel); at += kChunk)
                            {
                                const auto n = static_cast<int>(std::min<juce::int64>(kChunk, end - at));
                                reader->read(&chunk, 0, n, at, true, true);
                                if (!writer->writeFromAudioSampleBuffer(chunk, 0, n))
                                    return false;
                            }
                            return true;
                        };
                        const auto regionEnd = material->first + rendered.getNumSamples();
                        written = copy(0, material->first) && writer->writeFromAudioSampleBuffer(rendered, 0, rendered.getNumSamples())
                                  && copy(regionEnd, reader->lengthInSamples) && !isCancelled(cancel);
                        writer.reset();
                    }
                    if (!written && !isCancelled(cancel))
                        problem = "Could not write " + target.getFullPathName();
                }
            }
            if (!written)
                target.deleteFile();
            juce::MessageManager::callAsync([weak, id, generation, material, result = written ? target : juce::File(), problem, cancel]()
                                            {
                                                if (auto* self = weak.get())
                                                {
                                                    auto found = self->entries_.find(id);
                                                    if (found != self->entries_.end() && found->second.generation == generation && material != nullptr)
                                                        found->second.material = material;
                                                    self->renderDone(id, generation, result, isCancelled(cancel) ? juce::String("cancelled") : problem);
                                                }
                                                else
                                                    result.deleteFile();
                                            });
            return juce::ThreadPoolJob::jobHasFinished;
        });
    sendChangeMessage();
}

void AudioEdits::renderDone(std::uint32_t clipId, int generation, juce::File target, juce::String error)
{
    const auto found = entries_.find(clipId);
    if (found == entries_.end())
    {
        target.deleteFile();
        return;
    }
    auto& entry = found->second;
    if (entry.generation != generation)
    {
        target.deleteFile();
        return;
    }
    entry.rendering = false;
    const auto clip = findClipIn(session_.getState(), clipId);
    if (error == "cancelled" || !clip.isValid() || !audioedits::getEdit(clip).isValid())
    {
        target.deleteFile();
        entry.status = Status::idle;
        if (entry.dirty)
            startTimer(10);
        sendChangeMessage();
        return;
    }
    if (error.isNotEmpty() || target == juce::File())
    {
        entry.status = Status::failed;
        entry.problem = error.isNotEmpty() ? error : juce::String("The render failed.");
        sendChangeMessage();
        return;
    }
    // Loaded first, so the clip never plays a file that is not there yet.
    ++pendingLoads_;
    assets_.request(target,
                    [weak = juce::WeakReference<AudioEdits>(this), clipId, generation, target](AudioAssetPtr asset, const juce::String& problem)
                    {
                        auto* self = weak.get();
                        if (self == nullptr)
                            return;
                        --self->pendingLoads_;
                        const auto record = self->entries_.find(clipId);
                        const auto edited = findClipIn(self->session_.getState(), clipId);
                        if (record == self->entries_.end() || record->second.generation != generation || !edited.isValid()
                            || !audioedits::getEdit(edited).isValid() || asset == nullptr)
                        {
                            if (asset == nullptr && record != self->entries_.end() && record->second.generation == generation)
                            {
                                record->second.status = Status::failed;
                                record->second.problem = problem;
                            }
                            self->deleteIfUnused(target);
                            self->sendChangeMessage();
                            return;
                        }
                        self->play(edited, target);
                        self->sendChangeMessage();
                    });
    entry.status = Status::idle;
    if (entry.dirty)
        startTimer(10);
    sendChangeMessage();
}

void AudioEdits::play(const juce::ValueTree& clip, const juce::File& file)
{
    auto edit = audioedits::getEdit(clip);
    const juce::File previous(edit.getProperty(ids::rendered).toString());
    const auto source = audioedits::getSource(clip);
    auto c = clip;
    if (clip::getFile(c) != file)
        c.setProperty(ids::file, file.getFullPathName(), nullptr);
    edit.setProperty(ids::rendered, file == source ? juce::String() : file.getFullPathName(), nullptr);
    if (previous != juce::File() && previous != file)
        deleteIfUnused(previous);
}

void AudioEdits::deleteIfUnused(const juce::File& file)
{
    if (file == juce::File() || !file.isAChildOf(renderFolder_))
        return;
    for (const auto& used : session_.getReferencedFiles())
        if (used == file)
            return;
    bool referenced = false;
    forEachEditedClip(session_.getState(), [&](juce::ValueTree clip) { referenced = referenced || clip::getFile(clip) == file; });
    if (!referenced)
        file.deleteFile();
}

// --- Listening ---------------------------------------------------------------------------------------------

void AudioEdits::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
    if (property == ids::rendered)
        return;
    if (tree.hasType(ids::pitchNote) || tree.hasType(ids::pitchEdit) || tree.hasType(ids::audioEdit) || tree.hasType(ids::spectralEdit)
        || tree.hasType(ids::spectralOp))
        markDirty(clipOf(tree));
}

void AudioEdits::valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child)
{
    if (child.hasType(ids::audioEdit) || child.hasType(ids::pitchEdit) || child.hasType(ids::pitchNote) || child.hasType(ids::spectralEdit)
        || child.hasType(ids::spectralOp))
        markDirty(clipOf(parent));
}

void AudioEdits::valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (child.hasType(ids::audioEdit) && parent.hasType(ids::clip))
    {
        // (undone, or removed): the clip plays its own file again.
        const juce::File rendered(child.getProperty(ids::rendered).toString());
        const juce::File source(child.getProperty(ids::source).toString());
        if (source != juce::File() && clip::getFile(parent) != source)
            parent.setProperty(ids::file, source.getFullPathName(), nullptr);
        if (auto found = entries_.find(clip::getId(parent)); found != entries_.end())
        {
            if (found->second.cancel != nullptr)
                found->second.cancel->store(true);
            found->second.generation += 1;
            found->second.dirty = false;
            found->second.rendering = false;
            found->second.status = Status::idle;
        }
        deleteIfUnused(rendered);
        sendChangeMessage();
        return;
    }
    if (child.hasType(ids::pitchEdit) || child.hasType(ids::pitchNote) || child.hasType(ids::spectralEdit) || child.hasType(ids::spectralOp))
        markDirty(clipOf(parent));
}

// --- The model ---------------------------------------------------------------------------------------------

namespace audioedits
{
juce::ValueTree getEdit(const juce::ValueTree& clip) { return clip.getChildWithName(ids::audioEdit); }
juce::ValueTree getPitch(const juce::ValueTree& clip) { return getEdit(clip).getChildWithName(ids::pitchEdit); }
bool hasPitch(const juce::ValueTree& clip) { return getPitch(clip).isValid(); }

juce::File getSource(const juce::ValueTree& clip)
{
    const auto edit = getEdit(clip);
    if (edit.isValid() && edit.getProperty(ids::source).toString().isNotEmpty())
        return juce::File(edit.getProperty(ids::source).toString());
    return clip::getFile(clip);
}

double getFrom(const juce::ValueTree& clip) { return static_cast<double>(getEdit(clip).getProperty(ids::from, 0.0)); }

pitch::Mode getPitchMode(const juce::ValueTree& clip)
{
    return getPitch(clip).getProperty(ids::mode).toString() == "polyphonic" ? pitch::Mode::polyphonic : pitch::Mode::melodic;
}

bool keepsFormants(const juce::ValueTree& clip) { return static_cast<bool>(getPitch(clip).getProperty(ids::formants, true)); }

void setKeepsFormants(juce::ValueTree clip, bool keep, juce::UndoManager* um)
{
    auto p = getPitch(clip);
    if (p.isValid())
        p.setProperty(ids::formants, keep, um);
}

std::vector<juce::ValueTree> getPitchNotes(const juce::ValueTree& clip)
{
    std::vector<juce::ValueTree> notes;
    for (const auto& n : getPitch(clip))
        if (n.hasType(ids::pitchNote))
            notes.push_back(n);
    return notes;
}

juce::ValueTree getSpectral(const juce::ValueTree& clip) { return getEdit(clip).getChildWithName(ids::spectralEdit); }

std::vector<juce::ValueTree> getSpectralOperations(const juce::ValueTree& clip)
{
    std::vector<juce::ValueTree> ops;
    for (const auto& op : getSpectral(clip))
        if (op.hasType(ids::spectralOp))
            ops.push_back(op);
    return ops;
}

juce::String kindName(spectral::Kind kind)
{
    switch (kind)
    {
        case spectral::Kind::gain: return "gain";
        case spectral::Kind::erase: return "erase";
        case spectral::Kind::repair: return "repair";
        case spectral::Kind::denoise: return "denoise";
        case spectral::Kind::dehum: return "dehum";
    }
    return "gain";
}

spectral::Operation toOperation(const juce::ValueTree& op, double from)
{
    spectral::Operation o;
    const auto kind = op.getProperty(ids::kind).toString();
    for (const auto k : {spectral::Kind::gain, spectral::Kind::erase, spectral::Kind::repair, spectral::Kind::denoise, spectral::Kind::dehum})
        if (kind == kindName(k))
            o.kind = k;
    o.start = static_cast<double>(op.getProperty(ids::start, 0.0)) - from;
    o.end = static_cast<double>(op.getProperty(ids::end, 0.0)) - from;
    o.low = static_cast<double>(op.getProperty(ids::low, 0.0));
    o.high = static_cast<double>(op.getProperty(ids::high, 1.0e6));
    o.gainDb = static_cast<double>(op.getProperty(ids::gain, -12.0));
    o.amount = static_cast<double>(op.getProperty(ids::amount, 1.0));
    o.profile = decodeCurve(op.getProperty(ids::profile));
    o.reductionDb = static_cast<double>(op.getProperty(ids::reduction, 18.0));
    o.sensitivity = static_cast<double>(op.getProperty(ids::sensitivity, 1.5));
    o.humHz = static_cast<double>(op.getProperty(ids::hum, 0.0));
    o.harmonics = static_cast<int>(op.getProperty(ids::harmonics, 8));
    o.depthDb = static_cast<double>(op.getProperty(ids::depth, 40.0));
    o.enabled = static_cast<bool>(op.getProperty(ids::enabled, true));
    return o;
}

juce::ValueTree makeOperation(const spectral::Operation& o)
{
    juce::ValueTree op(ids::spectralOp);
    op.setProperty(ids::kind, kindName(o.kind), nullptr);
    op.setProperty(ids::start, o.start, nullptr);
    op.setProperty(ids::end, o.end, nullptr);
    op.setProperty(ids::low, o.low, nullptr);
    op.setProperty(ids::high, o.high, nullptr);
    op.setProperty(ids::amount, o.amount, nullptr);
    switch (o.kind)
    {
        case spectral::Kind::gain:
            op.setProperty(ids::gain, o.gainDb, nullptr);
            break;
        case spectral::Kind::denoise:
            op.setProperty(ids::profile, encodeCurve(o.profile), nullptr);
            op.setProperty(ids::reduction, o.reductionDb, nullptr);
            op.setProperty(ids::sensitivity, o.sensitivity, nullptr);
            break;
        case spectral::Kind::dehum:
            op.setProperty(ids::hum, o.humHz, nullptr);
            op.setProperty(ids::harmonics, o.harmonics, nullptr);
            op.setProperty(ids::depth, o.depthDb, nullptr);
            break;
        case spectral::Kind::erase:
        case spectral::Kind::repair:
            break;
    }
    if (!o.enabled)
        op.setProperty(ids::enabled, false, nullptr);
    return op;
}

juce::String describe(const juce::ValueTree& op)
{
    const auto o = toOperation(op, 0.0);
    const auto hz = [](double f) { return f >= 1000.0 ? juce::String(f / 1000.0, 1) + "k" : juce::String(juce::roundToInt(f)); };
    juce::String text;
    switch (o.kind)
    {
        case spectral::Kind::gain: text << (o.gainDb >= 0.0 ? "Boost " : "Attenuate ") << juce::String(o.gainDb, 1) << " dB"; break;
        case spectral::Kind::erase: text << "Erase"; break;
        case spectral::Kind::repair: text << "Repair"; break;
        case spectral::Kind::denoise: text << "De-noise " << juce::roundToInt(o.reductionDb) << " dB"; break;
        case spectral::Kind::dehum: text << "De-hum " << (o.humHz > 0.0 ? juce::String(juce::roundToInt(o.humHz)) + " Hz" : juce::String("auto")); break;
    }
    text << "   " << juce::String(o.start, 2) << "-" << juce::String(o.end, 2) << " s";
    if (o.kind != spectral::Kind::dehum && (o.low > 1.0 || o.high < 20000.0))
        text << "   " << hz(o.low) << "-" << hz(std::min(o.high, 99000.0)) << " Hz";
    return text;
}

void setOperationEnabled(juce::ValueTree op, bool enabled, juce::UndoManager* um) { op.setProperty(ids::enabled, enabled, um); }

pitch::Edit toEdit(const juce::ValueTree& note)
{
    pitch::Note n;
    n.start = static_cast<double>(note.getProperty(ids::start, 0.0));
    n.end = static_cast<double>(note.getProperty(ids::end, 0.0));
    n.pitch = static_cast<double>(note.getProperty(ids::pitch, 60.0));
    n.hop = std::max(1.0e-4, static_cast<double>(note.getProperty(ids::hop, 0.005)));
    n.drift = decodeCurve(note.getProperty(ids::driftCurve));
    n.level = decodeCurve(note.getProperty(ids::levelCurve));
    auto e = pitch::Edit::of(n);
    e.newStart = static_cast<double>(note.getProperty(ids::newStart, n.start));
    e.newEnd = static_cast<double>(note.getProperty(ids::newEnd, n.end));
    e.newPitch = static_cast<double>(note.getProperty(ids::newPitch, n.pitch));
    e.drift = static_cast<double>(note.getProperty(ids::drift, 1.0));
    e.gainDb = static_cast<double>(note.getProperty(ids::gain, 0.0));
    return e;
}

juce::ValueTree makeNote(const pitch::Note& note)
{
    juce::ValueTree n(ids::pitchNote);
    n.setProperty(ids::start, note.start, nullptr);
    n.setProperty(ids::end, note.end, nullptr);
    n.setProperty(ids::pitch, note.pitch, nullptr);
    n.setProperty(ids::hop, note.hop, nullptr);
    n.setProperty(ids::driftCurve, encodeCurve(note.drift), nullptr);
    n.setProperty(ids::levelCurve, encodeCurve(note.level), nullptr);
    return n;
}

double clipTimeOf(const juce::ValueTree& clip, double noteTime) { return clip::clipTimeOf(clip, getFrom(clip) + noteTime); }
double noteTimeOf(const juce::ValueTree& clip, double clipTime) { return clip::sourceAt(clip, clipTime) - getFrom(clip); }

void setNotePitch(juce::ValueTree note, double midi, juce::UndoManager* um) { note.setProperty(ids::newPitch, std::clamp(midi, 0.0, 127.0), um); }
void setNoteDrift(juce::ValueTree note, double drift, juce::UndoManager* um) { note.setProperty(ids::drift, std::clamp(drift, 0.0, 2.0), um); }
void setNoteGain(juce::ValueTree note, double gainDb, juce::UndoManager* um) { note.setProperty(ids::gain, std::clamp(gainDb, -36.0, 12.0), um); }

void setNoteTiming(juce::ValueTree note, double newStart, double newEnd, juce::UndoManager* um)
{
    constexpr double kShortest = 0.02;
    constexpr double kTouching = 0.005;
    auto parent = note.getParent();
    const int index = parent.indexOf(note);
    if (index < 0)
        return;
    const auto valueOf = [](const juce::ValueTree& n, const juce::Identifier& id, const juce::Identifier& fallback)
    { return static_cast<double>(n.getProperty(id, n.getProperty(fallback))); };
    const double start = static_cast<double>(note.getProperty(ids::start));
    const double end = static_cast<double>(note.getProperty(ids::end));
    const double currentStart = valueOf(note, ids::newStart, ids::start);
    const double currentEnd = valueOf(note, ids::newEnd, ids::end);
    const auto previous = index > 0 ? parent.getChild(index - 1) : juce::ValueTree();
    const auto next = index + 1 < parent.getNumChildren() ? parent.getChild(index + 1) : juce::ValueTree();
    const bool touchesPrevious = previous.isValid() && std::abs(static_cast<double>(previous.getProperty(ids::end)) - start) < kTouching;
    const bool touchesNext = next.isValid() && std::abs(static_cast<double>(next.getProperty(ids::start)) - end) < kTouching;
    const auto edit = parent.getParent();
    const double total = static_cast<double>(edit.getProperty(ids::to, 1.0e9)) - static_cast<double>(edit.getProperty(ids::from, 0.0));

    double lower = 0.0;
    if (previous.isValid())
        lower = touchesPrevious ? valueOf(previous, ids::newStart, ids::start) + kShortest : valueOf(previous, ids::newEnd, ids::end);
    double upper = total;
    if (next.isValid())
        upper = touchesNext ? valueOf(next, ids::newEnd, ids::end) - kShortest : valueOf(next, ids::newStart, ids::start);

    const bool moving = std::abs((newStart - currentStart) - (newEnd - currentEnd)) < 1.0e-9;
    if (moving)
    {
        const double length = currentEnd - currentStart;
        newStart = std::clamp(newStart, lower, std::max(lower, upper - length));
        newEnd = newStart + length;
    }
    else
    {
        // An edge (or both, apart): each within its neighbour and 20 ms from the other end.
        if (std::abs(newStart - currentStart) > 1.0e-12)
            newStart = std::max(lower, std::min(newStart, (std::abs(newEnd - currentEnd) > 1.0e-12 ? newEnd : currentEnd) - kShortest));
        else
            newStart = currentStart;
        if (std::abs(newEnd - currentEnd) > 1.0e-12)
            newEnd = std::min(std::max(upper, newStart + kShortest), std::max(newEnd, newStart + kShortest));
        else
            newEnd = currentEnd;
    }
    note.setProperty(ids::newStart, newStart, um);
    note.setProperty(ids::newEnd, newEnd, um);
    if (touchesPrevious)
    {
        auto p = previous;
        p.setProperty(ids::newEnd, newStart, um);
    }
    if (touchesNext)
    {
        auto n = next;
        n.setProperty(ids::newStart, newEnd, um);
    }
}

void resetNote(juce::ValueTree note, juce::UndoManager* um)
{
    for (const auto& id : {ids::newStart, ids::newEnd, ids::newPitch, ids::drift, ids::gain})
        note.removeProperty(id, um);
}

int nearestInScale(double midi, const Scale& scale)
{
    static constexpr std::array<std::array<bool, 12>, 6> kScales{{
        {true, true, true, true, true, true, true, true, true, true, true, true},
        {true, false, true, false, true, true, false, true, false, true, false, true},
        {true, false, true, true, false, true, false, true, true, false, true, false},
        {true, false, true, true, false, true, false, true, true, false, false, true},
        {true, false, true, false, true, false, false, true, false, true, false, false},
        {true, false, false, true, false, true, false, true, false, false, true, false},
    }};
    const auto& allowed = kScales[static_cast<std::size_t>(scale.kind)];
    const int centre = static_cast<int>(std::lround(midi));
    int best = centre;
    double bestDistance = 1.0e9;
    for (int candidate = centre - 6; candidate <= centre + 6; ++candidate)
    {
        const int degree = (((candidate - scale.root) % 12) + 12) % 12;
        if (!allowed[static_cast<std::size_t>(degree)])
            continue;
        if (const double d = std::abs(candidate - midi); d < bestDistance - 1.0e-9)
        {
            bestDistance = d;
            best = candidate;
        }
    }
    return std::clamp(best, 0, 127);
}

void correctPitch(const std::vector<juce::ValueTree>& notes, double pitchAmount, double driftAmount, const Scale& scale, juce::UndoManager* um)
{
    for (auto note : notes)
    {
        const auto e = toEdit(note);
        const double target = nearestInScale(e.newPitch, scale);
        setNotePitch(note, e.newPitch + (target - e.newPitch) * std::clamp(pitchAmount, 0.0, 1.0), um);
        setNoteDrift(note, e.drift * (1.0 - std::clamp(driftAmount, 0.0, 1.0)), um);
    }
}
} // namespace audioedits

} // namespace dojo::session
