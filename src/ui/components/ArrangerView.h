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
    The arranger: bar/beat ruler with the loop range, track headers and lanes, editing the session
    directly.

    Rows are tracks (folders indent their contents and can be collapsed) and, below a track whose
    automation is shown, one row per automation lane.

    Mouse
      ruler (top strip)    drag to set the loop range; drag it to move it, its edges to resize;
                           double-click toggles looping
      ruler (bottom)       click/drag to move the playhead
      empty lane           click: select track + move playhead; double-click on an instrument
                           track: new MIDI clip; double-click below the tracks: new track
      clip body            click to select (Ctrl toggles, Shift adds), drag to move (also across
                           tracks of the same kind); double-click opens it in the editor
      clip left/right edge drag to trim (non-destructive)
      clip top corners     drag the square handles to set fade-in / fade-out
      automation lane      click to add a point, drag points, double-click a point to delete it
      track header         click to select, drag to reorder (also into/out of folders), double-click
                           the name to rename; buttons: folder fold, R(ecord arm), M, S, A(utomation)
      right-click          context menus for clips, tracks, lanes and empty space
      wheel                vertical scroll; Shift+wheel horizontal; Ctrl+wheel zoom
    Holding Alt or Shift while dragging bypasses grid snapping.

    Audio files can be dropped from the desktop or the browser onto a lane (or below the last
    track); devices and plug-ins from the browser onto a track.
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
    /** Drag-and-drop description prefixes used by the browser. */
    static constexpr const char* kDeviceDragPrefix = "DojoDevice:";

    ArrangerView(session::Project& project, engine::EngineController& controller, EditorState& editor);
    ~ArrangerView() override;

    /** Called once per UI frame. */
    void setPlayhead(double seconds, bool playing);
    [[nodiscard]] double getPlayheadSeconds() const noexcept { return playheadSeconds_; }

    /** Shows the recording region (from `startSeconds` to the playhead) on armed tracks. */
    void setRecording(bool recording, double startSeconds);

    void zoomBy(double factor);
    void zoomToFit();

    /** Shows the Import Audio chooser (provided by MainComponent). */
    std::function<void()> onImportRequested;
    /** A clip was double-clicked (or "Edit" chosen): open it in the editor. */
    std::function<void(std::uint32_t clipId)> onOpenClip;
    /** A device (browser drag description) was dropped onto a track. */
    std::function<void(const juce::ValueTree& track, const juce::String& description)> onDeviceDropped;

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

    // Files and devices dragged from the browser
    bool isInterestedInDragSource(const SourceDetails& details) override;
    void itemDragEnter(const SourceDetails& details) override;
    void itemDragMove(const SourceDetails& details) override;
    void itemDragExit(const SourceDetails& details) override;
    void itemDropped(const SourceDetails& details) override;

    /** Where a clip is drawn, in this component's coordinates (empty if its track is not shown). */
    [[nodiscard]] juce::Rectangle<int> getClipArea(const juce::ValueTree& clip) const;

private:
    struct Row
    {
        enum class Kind
        {
            track,
            automation,
        };
        Kind kind = Kind::track;
        juce::ValueTree track; ///< the track (for lanes: the lane's owner)
        juce::ValueTree lane;  ///< AUTOMATION tree for automation rows
        int depth = 0;
        int top = 0; ///< content y (before scrolling)
        int height = 0;
    };

    enum class DragMode
    {
        none,
        locate,
        loopCreate,
        loopMove,
        loopStart,
        loopEnd,
        moveClips,
        trimStart,
        trimEnd,
        fadeIn,
        fadeOut,
        reorderTrack,
        automationPoint,
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
        int row = -1;
        ClipZone zone = ClipZone::body;
    };

    struct ClipSnapshot
    {
        std::uint32_t id = 0;
        int trackOrdinal = 0; ///< position among the visible track rows
        double start = 0.0;
        double length = 0.0;
        double offset = 0.0;
        double fadeIn = 0.0;
        double fadeOut = 0.0;
        double stretch = 1.0;
        bool midi = false;
    };

    // Rows
    void ensureRows() const;
    [[nodiscard]] const std::vector<Row>& rows() const
    {
        ensureRows();
        return rows_;
    }
    [[nodiscard]] int rowAt(int y) const; ///< -1 if none
    [[nodiscard]] int rowOfTrack(const juce::ValueTree& track) const;
    [[nodiscard]] std::vector<int> trackRows() const;

    // Geometry
    [[nodiscard]] juce::Rectangle<int> getRulerArea() const;
    [[nodiscard]] juce::Rectangle<int> getLoopStrip() const;
    [[nodiscard]] juce::Rectangle<int> getHeaderArea() const;
    [[nodiscard]] juce::Rectangle<int> getLaneArea() const;
    [[nodiscard]] juce::Rectangle<int> getRowBounds(int row) const; ///< full row, header + lane
    [[nodiscard]] float timeToX(double seconds) const noexcept;
    [[nodiscard]] double xToTime(float x) const noexcept;
    [[nodiscard]] double getVisibleSeconds() const noexcept;
    [[nodiscard]] juce::Rectangle<float> getClipBounds(const juce::ValueTree& clip, int row) const;
    [[nodiscard]] juce::Rectangle<int> getHeaderButton(int row, int indexFromRight) const;
    [[nodiscard]] juce::Rectangle<int> getFoldButton(int row) const;
    [[nodiscard]] juce::Rectangle<int> getNameBounds(int row) const;
    [[nodiscard]] ClipHit hitTestClip(juce::Point<float> position) const;
    [[nodiscard]] double getTempo() const;
    [[nodiscard]] double snap(double seconds, const juce::ModifierKeys& mods) const;

    // Automation geometry and text
    [[nodiscard]] float valueToY(const juce::Rectangle<int>& row, double value) const;
    [[nodiscard]] double yToValue(const juce::Rectangle<int>& row, float y) const;
    [[nodiscard]] juce::ValueTree hitTestPoint(int row, juce::Point<float> position) const;
    [[nodiscard]] juce::String describeTarget(const juce::ValueTree& lane) const;
    [[nodiscard]] juce::String describeValue(const juce::ValueTree& lane, double value) const;
    [[nodiscard]] double currentValueFor(const juce::ValueTree& track, const juce::String& target) const;

    // Painting
    void paintRuler(juce::Graphics& g) const;
    void paintHeaders(juce::Graphics& g) const;
    void paintTrackHeader(juce::Graphics& g, int rowIndex, const Row& row) const;
    void paintLaneHeader(juce::Graphics& g, int rowIndex, const Row& row) const;
    void paintLanes(juce::Graphics& g) const;
    void paintAutomation(juce::Graphics& g, int rowIndex, const Row& row) const;
    void paintClip(juce::Graphics& g, const juce::ValueTree& clip, int row, juce::Colour colour) const;
    void paintWaveform(juce::Graphics& g, const juce::ValueTree& clip, juce::Rectangle<float> area,
                       juce::Colour colour) const;
    void paintMidiNotes(juce::Graphics& g, const juce::ValueTree& clip, juce::Rectangle<float> area,
                        juce::Colour colour) const;
    void paintDropIndicator(juce::Graphics& g) const;
    void paintPlayhead(juce::Graphics& g) const;
    void paintEmptyState(juce::Graphics& g) const;

    // Interaction helpers
    void locateTo(double seconds);
    void beginClipDrag(const ClipHit& hit);
    void dragClips(const juce::MouseEvent& e);
    void dragLoop(const juce::MouseEvent& e);
    void mouseDownOnHeader(const juce::MouseEvent& e, int row);
    void mouseDownOnAutomation(const juce::MouseEvent& e, int row);
    void showClipMenu(const juce::ValueTree& clip);
    void showTrackMenu(const juce::ValueTree& track);
    void showLaneMenu(int row, double seconds);
    void showAutomationMenu(const juce::ValueTree& lane, const juce::ValueTree& point);
    void addAutomationMenuItems(juce::PopupMenu& menu, const juce::ValueTree& track);
    void createMidiClipAt(const juce::ValueTree& track, double seconds);
    void startRename(int row);
    void finishRename(bool commit);
    void updateDropTarget(int x, int y);
    void dropFiles(const juce::StringArray& files);
    void setScroll(double seconds, int y);
    void updateScrollbars();
    void structureChanged();
    [[nodiscard]] double getContentDuration() const;
    [[nodiscard]] int getContentHeight() const;

    // Listeners
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged(juce::ValueTree&, int, int) override;
    void valueTreeRedirected(juce::ValueTree&) override { structureChanged(); }
    void scrollBarMoved(juce::ScrollBar* bar, double newRangeStart) override;
    void editorStateChanged() override { repaint(); }
    void audioAssetLoaded(const juce::File&) override { repaint(); }

    session::Project& project_;
    session::Session& session_;
    engine::EngineController& controller_;
    EditorState& editor_;

    mutable std::vector<Row> rows_;
    mutable bool rowsDirty_ = true;

    juce::ScrollBar horizontalScroll_{false};
    juce::ScrollBar verticalScroll_{true};
    juce::TextButton addTrackButton_;
    std::unique_ptr<juce::TextEditor> renameEditor_;
    std::uint32_t renamingTrackId_ = 0;

    double pixelsPerSecond_ = 40.0;
    double scrollSeconds_ = 0.0;
    int scrollY_ = 0;
    double playheadSeconds_ = 0.0;
    bool recording_ = false;
    double recordStart_ = 0.0;

    DragMode dragMode_ = DragMode::none;
    bool dragMoved_ = false;
    juce::Point<float> dragStart_;
    std::uint32_t dragAnchorId_ = 0;
    std::vector<ClipSnapshot> dragOriginals_;
    double dragSourceLength_ = 0.0;
    juce::ValueTree reorderTrack_;
    int reorderTargetRow_ = -1;
    juce::ValueTree dragPoint_;
    int dragPointRow_ = -1;
    double loopDragStart_ = 0.0;
    double loopDragEnd_ = 0.0;
    double loopAnchor_ = 0.0;

    bool dropActive_ = false;
    int dropRow_ = -1;
    double dropTime_ = 0.0;
    bool dropIsDevice_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ArrangerView)
};

} // namespace dojo::ui
