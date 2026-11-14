#pragma once

#include "core/Types.h"
#include "engine/EngineController.h"
#include "session/Project.h"
#include "ui/EditorState.h"
#include "video/FrameServer.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
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
                           private session::AudioAssetCache::Listener,
                           private juce::ChangeListener
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
    /** A device or a preset (browser drag description) was dropped onto a track (or below them). */
    std::function<void(const juce::ValueTree& track, const juce::String& description)> onDeviceDropped;
    /** Runs an application command (the track menu's Duplicate..., Split into Mono, ...). */
    std::function<void(int commandId)> onCommand;
    /** A video was imported (dropped): show the video window. */
    std::function<void()> onVideoImported;

    /** The decoder for video clip thumbnails (none: video clips show their name only). */
    void setVideoFrames(video::FrameServer* frames);

    /** Track > Scroll to Track: brings `track` as close to the top as possible (opening the folders
        around it). */
    void scrollToTrack(const juce::ValueTree& track);

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

    /** Zoom and scroll position (kept when the window is rebuilt, e.g. for a new theme). */
    struct ViewState
    {
        double pixelsPerSecond = 40.0;
        double scrollSeconds = 0.0;
        int scrollY = 0;
    };
    [[nodiscard]] ViewState getViewState() const noexcept { return {pixelsPerSecond_, scrollSeconds_, scrollY_}; }
    void setViewState(const ViewState& state);

    /** Scrolls the view so that `seconds` is at its left edge (vertical position unchanged). */
    void scrollToTime(double seconds);
    /** The time at the left edge of the view. */
    [[nodiscard]] double getScrollSeconds() const noexcept { return scrollSeconds_; }

    // --- Clip selection (what the mouse does; public for tests) -------------------------------------
    /** A click on a clip: plain selects it, Ctrl toggles it, Shift selects every clip on its track
        between the anchor (the last clip clicked without Shift) and it; Ctrl+Shift adds that range. */
    void selectClipWithModifiers(const juce::ValueTree& clip, juce::ModifierKeys mods);
    /** The rubber band: selects every clip that `area` (component coordinates, may extend beyond the
        visible part) touches, plus `base` (the selection a Ctrl/Shift drag adds to). */
    void selectClipsInArea(juce::Rectangle<int> area, const std::set<std::uint32_t>& base);
    /** The rubber band being dragged, in component coordinates (empty when none). */
    [[nodiscard]] juce::Rectangle<int> getMarqueeArea() const;

    /** Where a clip is drawn, in this component's coordinates (empty if its track is not shown). */
    [[nodiscard]] juce::Rectangle<int> getClipArea(const juce::ValueTree& clip) const;

    // --- Track selection (what clicks on track headers do; public for tests) -------------------------
    /** A click on a track's header: plain selects only it, Ctrl adds or removes it, Shift selects every
        track shown between the anchor (the last track clicked without Shift) and it; Ctrl+Shift adds
        that range. */
    void selectTrackWithModifiers(const juce::ValueTree& track, juce::ModifierKeys mods);
    /** Where a track's header is drawn, in component coordinates (empty if the track is not shown). */
    [[nodiscard]] juce::Rectangle<int> getTrackHeaderArea(const juce::ValueTree& track) const;
    /** A header's toggle, counted from the right: 0 automation (A), 1 solo (S), 2 mute (M), 3 arm (R). */
    [[nodiscard]] juce::Rectangle<int> getTrackButtonArea(const juce::ValueTree& track, int indexFromRight) const;

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
        marquee,
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
        bool looped = false;
        std::optional<double> sync; ///< sync point, seconds from the start
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
    /** The ids of the tracks shown, top to bottom. */
    [[nodiscard]] std::vector<std::uint32_t> shownTrackOrder() const;
    /** The selected tracks in arranger order (all of them, or only the outermost: a selected folder's
        selected contents left out). */
    [[nodiscard]] std::vector<juce::ValueTree> selectedTracksInOrder(bool outermostOnly) const;
    /** Inactive tracks are dimmed and labelled. */
    void paintInactiveTracks(juce::Graphics& g) const;

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
    /** An audio or video clip whose file cannot be found. */
    [[nodiscard]] bool isOffline(const juce::ValueTree& clip) const;
    /** Clip groups on a row: one frame and title bar over each group's clips. */
    void paintClipGroups(juce::Graphics& g, int row, const juce::ValueTree& track) const;
    /** A video clip's body: a strip of frames along it. */
    void paintVideoFrames(juce::Graphics& g, const juce::ValueTree& clip, juce::Rectangle<float> area, juce::Colour colour) const;
    void changeListenerCallback(juce::ChangeBroadcaster*) override { repaint(getLaneArea()); }
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
    void dragMarquee(const juce::MouseEvent& e);
    void paintMarquee(juce::Graphics& g) const;
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
    video::FrameServer* videoFrames_ = nullptr;

    mutable std::vector<Row> rows_;
    mutable bool rowsDirty_ = true;

    juce::ScrollBar horizontalScroll_{false};
    juce::ScrollBar verticalScroll_{true};
    juce::TextButton addTrackButton_;
    std::unique_ptr<juce::TextEditor> renameEditor_;
    std::uint32_t renamingTrackId_ = 0;

    double pixelsPerSecond_ = 40.0;
    // Rubber-band selection: corners in timeline coordinates (seconds, content y), so the band stays
    // anchored while the view scrolls under it.
    struct TimelinePoint
    {
        double seconds = 0.0;
        int contentY = 0;
    };
    TimelinePoint marqueeAnchor_, marqueeEnd_;
    mutable std::map<juce::String, std::pair<bool, juce::uint32>> fileExists_; ///< path -> (exists, checked at)
    bool marqueeActive_ = false;
    std::set<std::uint32_t> marqueeBase_;
    double pendingLocate_ = 0.0;      ///< a click (no drag) in empty space moves the playhead here
    std::uint32_t selectionAnchor_ = 0; ///< Shift+click selects from this clip
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
    std::uint32_t pendingTrackClick_ = 0; ///< a click (no drag) on a track of a multi-selection selects only it
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
