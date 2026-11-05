#include "session/Project.h"

namespace dojo::session
{

Project::Project(GraphCompiler::Publisher publishGraph, DeviceManager::Creator createDevice)
    : devices_(session_, std::move(createDevice))
    , compiler_(session_, assets_, &devices_, std::move(publishGraph))
{
    session_.getState().addListener(this);
    devices_.onParameterChanged = [this] { setDirty(true); };
}

Project::~Project()
{
    session_.getState().removeListener(this);
}

juce::String Project::getDisplayName() const
{
    return file_ == juce::File() ? juce::String("Untitled") : file_.getFileNameWithoutExtension();
}

void Project::setDirty(bool dirty)
{
    if (loading_ || dirty_ == dirty)
        return;

    dirty_ = dirty;
    if (onDocumentStateChanged)
        onDocumentStateChanged();
}

void Project::newSession()
{
    imports_.clear();
    loading_ = true;
    session_.reset();
    loading_ = false;
    file_ = juce::File();
    dirty_ = false;

    if (onDocumentStateChanged)
        onDocumentStateChanged();
}

juce::Result Project::open(const juce::File& file)
{
    imports_.clear();
    loading_ = true;
    const auto result = session_.load(file);
    loading_ = false;

    if (result.failed())
        return result;

    file_ = file;
    dirty_ = false;

    // Start decoding every referenced file now; clips appear in the mix as their audio arrives.
    juce::StringArray missing;
    for (const auto& audioFile : session_.getReferencedFiles())
    {
        if (!audioFile.existsAsFile())
            missing.addIfNotAlreadyThere(audioFile.getFullPathName());
        else
            assets_.request(audioFile, {});
    }

    if (onDocumentStateChanged)
        onDocumentStateChanged();

    if (!missing.isEmpty() && onImportProblem)
        onImportProblem("These audio files could not be found, so their clips are silent:\n\n" +
                        missing.joinIntoString("\n"));

    return juce::Result::ok();
}

juce::Result Project::save()
{
    if (file_ == juce::File())
        return juce::Result::fail("The session has no file yet; use Save As.");

    return saveAs(file_);
}

juce::Result Project::saveAs(const juce::File& file)
{
    const auto target = file.hasFileExtension(".dojo") ? file : file.withFileExtension(".dojo");
    devices_.storeStates();
    const auto result = session_.save(target);

    if (result.wasOk())
    {
        file_ = target;
        dirty_ = false;
        if (onDocumentStateChanged)
            onDocumentStateChanged();
    }

    return result;
}

void Project::importFiles(const juce::StringArray& paths, const juce::ValueTree& targetTrack,
                          double startSeconds)
{
    std::uint32_t target = targetTrack.isValid() ? track::getId(targetTrack) : 0u;
    int insertAfter = targetTrack.isValid() ? static_cast<int>(track::getId(targetTrack)) : 0;

    juce::WeakReference<Project> weakThis(this);

    for (const auto& path : paths)
    {
        auto item = std::make_shared<PendingImport>();
        item->file = juce::File(path);
        item->targetTrackId = target;
        item->insertAfterTrackId = insertAfter;
        item->start = std::max(0.0, startSeconds);
        imports_.push_back(item);
        target = 0; // only the first file goes onto the target track

        assets_.request(item->file,
                        [weakThis, item](AudioAssetPtr asset, const juce::String& error)
                        {
                            item->finished = true;
                            item->asset = std::move(asset);
                            item->error = error;

                            if (auto* self = weakThis.get())
                                self->processFinishedImports();
                        });
    }
}

void Project::processFinishedImports()
{
    juce::StringArray problems;

    // Strictly in request order, so that tracks appear in the order the files were given.
    while (!imports_.empty() && imports_.front()->finished)
    {
        const auto item = imports_.front();
        imports_.pop_front();

        if (item->asset == nullptr)
        {
            problems.add(item->error.isNotEmpty() ? item->error
                                                  : "Could not import " + item->file.getFileName());
            continue;
        }

        session_.beginTransaction("Import " + item->file.getFileName());

        auto t = session_.findTrack(item->targetTrackId);
        if (t.isValid() && track::getKind(t) != TrackKind::audio)
            t = {};
        if (!t.isValid())
        {
            const auto after = session_.findTrack(static_cast<std::uint32_t>(item->insertAfterTrackId));
            const int index = after.isValid() ? session_.indexOf(after) + 1 : -1;
            t = session_.addTrack(item->file.getFileNameWithoutExtension(), index);

            // Later files in the same batch go below this one.
            for (auto& next : imports_)
                if (next->targetTrackId == 0 && next->insertAfterTrackId == item->insertAfterTrackId)
                    next->insertAfterTrackId = static_cast<int>(track::getId(t));
        }

        session_.addClip(t, item->file, item->start, item->asset->getLengthSeconds());
    }

    if (!problems.isEmpty() && onImportProblem)
        onImportProblem(problems.joinIntoString("\n"));
}

} // namespace dojo::session
