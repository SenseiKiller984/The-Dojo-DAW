#pragma once

#include "engine/RenderGraph.h"
#include "session/AudioAssetCache.h"
#include "session/DeviceManager.h"
#include "session/GraphCompiler.h"
#include "session/Session.h"

#include <juce_events/juce_events.h>

#include <deque>
#include <functional>
#include <memory>

namespace dojo::session
{

/**
    The open project: the session document, its decoded audio, the compiler that keeps the engine in
    sync, and document state (file on disk, unsaved changes). Message thread only.
*/
class Project final : private juce::ValueTree::Listener
{
public:
    /** `createDevice` instantiates built-in devices and plug-ins for DEVICE trees. */
    Project(GraphCompiler::Publisher publishGraph, DeviceManager::Creator createDevice);
    ~Project() override;

    [[nodiscard]] Session& getSession() noexcept { return session_; }
    [[nodiscard]] AudioAssetCache& getAssets() noexcept { return assets_; }
    [[nodiscard]] DeviceManager& getDevices() noexcept { return devices_; }
    [[nodiscard]] GraphCompiler& getCompiler() noexcept { return compiler_; }

    // --- Document --------------------------------------------------------------------------------
    [[nodiscard]] const juce::File& getFile() const noexcept { return file_; }
    [[nodiscard]] juce::String getDisplayName() const;
    [[nodiscard]] bool hasUnsavedChanges() const noexcept { return dirty_; }

    void newSession();
    juce::Result open(const juce::File& file);
    juce::Result save();
    juce::Result saveAs(const juce::File& file);

    /** Called whenever the file name or the unsaved-changes flag changes (e.g. to update the title). */
    std::function<void()> onDocumentStateChanged;

    // --- Import ----------------------------------------------------------------------------------
    /**
        Imports audio files as clips, decoding in the background. The first file lands on
        `targetTrack` (if valid) at `startSeconds`; every further file — or every file when there is
        no target — gets a new track, in the order given. Each file becomes its own undo step.
    */
    void importFiles(const juce::StringArray& paths, const juce::ValueTree& targetTrack, double startSeconds);

    /** Called with a human-readable message when some files could not be imported. */
    std::function<void(const juce::String&)> onImportProblem;

    /** Number of files still being decoded for import. */
    [[nodiscard]] int getPendingImportCount() const noexcept { return static_cast<int>(imports_.size()); }

private:
    struct PendingImport
    {
        juce::File file;
        std::uint32_t targetTrackId = 0; ///< 0 = create a new track
        int insertAfterTrackId = 0;      ///< new tracks go after this track (0 = at the end)
        double start = 0.0;
        bool finished = false;
        AudioAssetPtr asset;
        juce::String error;
    };

    void processFinishedImports();
    void addMissingWholeFileClips();
    void setDirty(bool dirty);

    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier& property) override
    {
        if (property != ids::state) // device state is written back right before saving
            setDirty(true);
    }
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override { setDirty(true); }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override { setDirty(true); }
    void valueTreeChildOrderChanged(juce::ValueTree&, int, int) override { setDirty(true); }

    Session session_;
    AudioAssetCache assets_;
    DeviceManager devices_;
    GraphCompiler compiler_;

    juce::File file_;
    bool dirty_ = false;
    bool loading_ = false;

    std::deque<std::shared_ptr<PendingImport>> imports_;

    JUCE_DECLARE_WEAK_REFERENCEABLE(Project)
};

} // namespace dojo::session
