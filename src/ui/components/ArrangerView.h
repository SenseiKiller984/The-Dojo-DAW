#pragma once

#include "core/Types.h"
#include "engine/EngineController.h"
#include "session/Project.h"
#include "ui/EditorState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace dojo::ui
{

/**
    The arranger: bar/beat ruler, track headers and clip lanes, editing the session directly.

    Mouse
      ruler                click/drag to move the playhead
      empty lane           click: select track + move playhead; double-click below tracks: new track
      clip body            click to select (Ctrl toggles, Shift adds), drag to move (also across tracks)
      clip left/right edge drag to trim (non-destructive: the source is untouched)
      clip top corners     drag the square handles to set fade-in / fade-out
      track header         click to select, drag to reorder, double-click name to rename, M / S buttons
      right-click          context menus for clips, tracks and empty space
      wheel                vertical scroll; Shift+wheel horizontal; Ctrl+wheel zoom
    Holding Alt or Shift while dragging bypasses grid snapping.
    Audio files can be dropped from the desktop or the browser onto a lane (or below the last track).
*/
class ArrangerView final : public juce::Component,
                           public juce::FileDragAndDropTarget,
                           public juce::DragAndDropTarget,
                           private juce::ValueTree::Listener,
                           private juce::ScrollBar::Listener,
                           private EditorState::Listener,
                           private session::AudioAssetCache::Listener
{
public:
    /** Drag-and-drop description used by the browser's file tree. */
    static constexpr const char* kBrowserDragDescription = "DojoBrowserFiles";

    ArrangerView(session::Project& project, engine::EngineController& controller, EditorState& editor);
    ~ArrangerView() override;

    /** Called once per UI frame. */
    void setPlayhead(SampleCount playheadSamples, double sampleRate, bool playing);
    [[nodiscard]] double getPlayheadSeconds() const noexcept { return playheadSeconds_; }

    void zoomBy(double factor);
    void zoomToFit();

    /** Shows the Import Audio chooser (provided by MainComponent). */
    std::function<void()> onImportRequested;

    // juce::Component
    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

    // Files dragged in from the operating system
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray& files, int x, int y) override;
    void fileDragMove(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    // Files dragged from the browser
    bool isInterestedInDragSource(const SourceDetails& details) override;
    void itemDragEnter(const SourceDetails& details) override;
    void itemDragMove(const SourceDetails& details) override;
    void itemDragExit(const SourceDetails& details) override;
    void itemDropped(const SourceDetails& details) override;

private:
    enum class DragMode
    {
        none,
        locate,
        moveClips,
        trimStart,
        trimEnd,
        fadeIn,
        fadeOut,
        reorderTrack,
    };

    enum class ClipZone
    {
        body,
        leftEdge,
        rightEdge,
        fadeInHandle,
        fadeOutHandle,
    };

    struct ClipHit
    {
        juce::ValueTree clip;
        int trackIndex = -1;
        ClipZone zone = ClipZone::body;
    };

    struct ClipSnapshot
    {
        std::uint32_t id = 0;
        int trackIndex = 0;
        double start = 0.0;
        double length = 0.0;
        double offset = 0.0;
        double fadeIn = 0.0;
        double fadeOut = 0.0;
    };

    // Geometry
    [[nodiscard]] juce::Rectangle<int> getRulerArea() const;
    [[nodiscard]] juce::Rectangle<int> getHeaderArea() const;
    [[nodiscard]] juce::Rectangle<int> getLaneArea() const;
    [[nodiscard]] juce::Rectangle<int> getRowBounds(int trackIndex) const; ///< full row, header + lane
    [[nodiscard]] int getTrackIndexAt(int y) const;                        ///< -1 if no track there
    [[nodiscard]] float timeToX(double seconds) const noexcept;
    [[nodiscard]] double xToTime(float x) const noexcept;
    [[nodiscard]] double getVisibleSeconds() const noexcept;
    [[nodiscard]] juce::Rectangle<float> getClipBounds(const juce::ValueTree& clip, int trackIndex) const;
    [[nodiscard]] juce::Rectangle<int> getMuteButtonBounds(int trackIndex) const;
    [[nodiscard]] juce::Rectangle<int> getSoloButtonBounds(int trackIndex) const;
    [[nodiscard]] juce::Rectangle<int> getNameBounds(int trackIndex) const;
    [[nodiscard]] ClipHit hitTestClip(juce::Point<float> position) const;
    [[nodiscard]] double getTempo() const;
    [[nodiscard]] double snap(double seconds, const juce::ModifierKeys& mods) const;

    // Painting
    void paintRuler(juce::Graphics& g) const;
    void paintHeaders(juce::Graphics& g) const;
    void paintLanes(juce::Graphics& g) const;
    void paintClip(juce::Graphics& g, const juce::ValueTree& clip, int trackIndex, juce::Colour colour) const;
    void paintWaveform(juce::Graphics& g, const juce::ValueTree& clip, juce::Rectangle<float> area,
                       juce::Colour colour) const;
    void paintDropIndicator(juce::Graphics& g) const;
    void paintPlayhead(juce::Graphics& g) const;
    void paintEmptyState(juce::Graphics& g) const;

    // Interaction helpers
    void locateTo(double seconds);
    void beginClipDrag(const ClipHit& hit, const juce::MouseEvent& e);
    void dragClips(const juce::MouseEvent& e);
    void showClipMenu(const juce::ValueTree& clip);
    void showTrackMenu(int trackIndex);
    void showLaneMenu(int trackIndex, double seconds);
    void startRename(int trackIndex);
    void finishRename(bool commit);
    void toggleMute(int trackIndex);
    void toggleSolo(int trackIndex);
    void updateDropTarget(int x, int y);
    void dropFiles(const juce::StringArray& files);
    void setScroll(double seconds, int y);
    void updateScrollbars();
    [[nodiscard]] double getContentDuration() const;
    [[nodiscard]] int getContentHeight() const;

    // Listeners
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged(juce::ValueTree&, int, int) override;
    void scrollBarMoved(juce::ScrollBar* bar, double newRangeStart) override;
    void editorStateChanged() override { repaint(); }
    void audioAssetLoaded(const juce::File&) override { repaint(); }
    void sessionChanged();

    session::Project& project_;
    session::Session& session_;
    engine::EngineController& controller_;
    EditorState& editor_;

    double pixelsPerSecond_ = 48.0;
    double scrollSeconds_ = 0.0;
    int scrollY_ = 0;
    double playheadSeconds_ = 0.0;

    DragMode dragMode_ = DragMode::none;
    juce::Point<float> dragStart_;
    bool dragMoved_ = false;
    std::vector<ClipSnapshot> dragOriginals_;
    std::uint32_t dragAnchorId_ = 0;
    double dragSourceLength_ = 0.0;
    int reorderFrom_ = -1;
    int reorderTo_ = -1;

    bool dropActive_ = false;
    int dropTrackIndex_ = -1;
    double dropTime_ = 0.0;

    juce::ScrollBar horizontalScroll_{false};
    juce::ScrollBar verticalScroll_{true};
    juce::TextButton addTrackButton_;
    std::unique_ptr<juce::TextEditor> renameEditor_;
    std::uint32_t renamingTrackId_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ArrangerView)
};

} // namespace dojo::ui
