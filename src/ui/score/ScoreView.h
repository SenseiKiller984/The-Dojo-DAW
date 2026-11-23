#pragma once

#include "session/Session.h"
#include "ui/score/Notation.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <vector>

namespace dojo::ui
{

/**
    The score editor: a MIDI clip in staff notation (Clip Editor > SCORE).

    Engraved from score::layout(): treble, bass or grand staff, the key signature, the time
    signatures of the tempo map, bars numbered as in the arrangement, notes and rests with dots,
    ties, accidentals, ledger lines, stems, flags and beams. Spaced by time with room for what each
    column needs, on one horizontally scrolling line.

    Editing: the Note tool (N) puts the chosen value (whole to 32nd, dotted) where the mouse is, on
    the grid, at the staff position's pitch in the key (Shift: a semitone up, Alt: down). The Select
    tool (S) picks notes (Shift adds); dragging moves them along the grid and up or down the staff
    (in the key). Keys: Delete removes, Up/Down a semitone (Ctrl: an octave), Left/Right a grid
    step, 1-6 set whole to 32nd (the selection's length, or the Note tool's value), "." dots.
    Right-click deletes a note (either tool); Ctrl+A selects all, Escape none; Ctrl+wheel zooms.
    Every edit is one undo step on the clip's notes; the clef, key and grid are the clip's
    (scoreClef, scoreKey, scoreGrid). MusicXML... exports the score. Notation shows the clip's
    content once (a looped clip is not repeated); triplets and other tuplets are written as the
    nearest grid values (the grid offers straight values only).
*/
class ScoreView final : public juce::Component,
                        private juce::ValueTree::Listener,
                        private juce::ScrollBar::Listener,
                        private juce::AsyncUpdater
{
public:
    explicit ScoreView(session::Session& session);
    ~ScoreView() override;

    /** A note put in or clicked plays while the mouse is held (as the piano roll's keys). */
    std::function<void(const juce::MidiMessage&)> onPreview;

    void setClip(const juce::ValueTree& clip);
    [[nodiscard]] const juce::ValueTree& getClip() const noexcept { return clip_; }
    void setPlayhead(double seconds);

    enum class Tool
    {
        select,
        note,
    };
    void setTool(Tool tool);
    [[nodiscard]] Tool getTool() const noexcept { return tool_; }
    /** The Note tool's value: type 0 whole ... 5 32nd. */
    void setNoteValue(int type, bool dotted);
    [[nodiscard]] int getNoteType() const noexcept { return noteType_; }
    [[nodiscard]] bool isDotted() const noexcept { return dotted_; }

    /** The clip's score settings (undoable). Key: none for detected. */
    void setClefMode(score::ClefMode mode);
    void setKey(std::optional<int> fifths);
    void setGrid(double beats);
    [[nodiscard]] score::Options getOptions() const;
    [[nodiscard]] const score::Score& getScore() const;

    /** Writes the clip's score as MusicXML. */
    bool exportMusicXml(const juce::File& file) const;

    // --- Geometry and selection (public for tests) ---------------------------------------------------
    [[nodiscard]] float lineSpacing() const noexcept { return 8.0f; }
    [[nodiscard]] float staffTop(int staff) const;
    [[nodiscard]] float diatonicToY(int staff, int diatonic) const;
    [[nodiscard]] int yToDiatonic(int staff, float y) const;
    [[nodiscard]] int staffAt(float y) const;
    /** A timeline beat's x (and back), along the spaced columns. */
    [[nodiscard]] float beatToX(double beat) const;
    [[nodiscard]] double xToBeat(float x) const;
    /** Where a note's (first) head is drawn; {-1,-1} if it is not shown. */
    [[nodiscard]] juce::Point<float> headPosition(const juce::ValueTree& note) const;
    /** The timeline beat of the clip's content beat 0. */
    [[nodiscard]] double contentOrigin() const;
    [[nodiscard]] const std::vector<juce::ValueTree>& getSelection() const noexcept { return selection_; }
    void setSelection(std::vector<juce::ValueTree> notes);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier&) override;
    void valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree&) override;
    void valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged(juce::ValueTree&, int, int) override {}
    void scrollBarMoved(juce::ScrollBar*, double start) override;
    void handleAsyncUpdate() override;
    [[nodiscard]] bool concerns(const juce::ValueTree& tree) const;

    void invalidate();
    void relayout() const;
    void updateToolbar();
    [[nodiscard]] juce::Rectangle<int> scoreArea() const;
    [[nodiscard]] juce::ValueTree noteAt(juce::Point<float> p) const;
    [[nodiscard]] int pitchAt(int staff, float y, juce::ModifierKeys mods) const;
    [[nodiscard]] double snapBeat(double beat) const;
    void insertNoteAt(juce::Point<float> p, juce::ModifierKeys mods);
    void preview(int pitch);
    void stopPreview();
    void updateScrollBar();
    void transposeSelection(int semitones);
    void moveSelection(double beats);
    void setSelectionValue(int type, std::optional<bool> dotted);
    void paintStaff(juce::Graphics& g, int staffIndex) const;
    [[nodiscard]] float contentX(double beat) const;
    [[nodiscard]] double contentBeat(float x) const;
    [[nodiscard]] bool isSelected(std::uint32_t id) const;
    /** Where an event's heads go (x, y), their accidentals' columns and the stem's x. */
    struct Placed
    {
        std::vector<juce::Point<float>> heads;
        std::vector<int> accidentalColumn;
        float stemX = 0.0f;
    };
    [[nodiscard]] Placed place(int staffIndex, const score::Event& event) const;

    session::Session& session_;
    juce::ValueTree clip_;
    std::vector<juce::ValueTree> selection_;
    Tool tool_ = Tool::note;
    int noteType_ = 2;
    bool dotted_ = false;
    double playheadBeat_ = -1.0;
    float scrollX_ = 0.0f;
    float pixelsPerBeat_ = 64.0f;

    // The layout, rebuilt when the clip, its notes or the tempo map change.
    struct Column
    {
        double beat = 0.0;
        float x = 0.0f;
    };
    mutable bool dirty_ = true;
    mutable score::Score score_;
    mutable std::vector<Column> columns_;  ///< note columns and bar ends, in order (content x)
    mutable std::vector<float> barLineX_;  ///< each bar's start (and the end)
    mutable std::vector<float> signatureX_; ///< where a bar's time signature is (or -1)
    mutable double origin_ = 0.0;           ///< timeline beat of the content's beat 0
    mutable double clipFrom_ = 0.0, clipTo_ = 0.0; ///< the clip's span (timeline beats)
    mutable float headerWidth_ = 0.0f;
    mutable float leftMargin_ = 0.0f; ///< where the staves start (room for a grand staff's brace)
    mutable float contentWidth_ = 0.0f;

    // A drag of the selection.
    struct Original
    {
        juce::ValueTree note;
        double start = 0.0;
        int pitch = 60;
    };
    std::vector<Original> dragOriginals_;
    std::vector<Column> dragColumns_; ///< the layout when the drag began (it moves with the notes)
    juce::Point<float> dragStart_;
    bool dragging_ = false;
    bool inserted_ = false; ///< this click put a note in (its drag is part of that step)
    bool moved_ = false;
    double dragBeats_ = 0.0;
    int dragSteps_ = 0;
    bool lassoing_ = false;
    juce::Rectangle<float> lasso_;
    std::vector<juce::ValueTree> lassoBase_;
    juce::Point<float> hover_{-1.0f, -1.0f};
    juce::ModifierKeys hoverMods_;
    int previewNote_ = -1;

    juce::TextButton selectTool_{"Select"}, noteTool_{"Note"};
    std::array<juce::TextButton, 6> valueButtons_;
    juce::TextButton dotButton_{"."};
    juce::ComboBox clefBox_, keyBox_, gridBox_;
    juce::TextButton exportButton_{"MusicXML..."};
    juce::ScrollBar hScroll_{false};
    std::unique_ptr<juce::FileChooser> chooser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScoreView)
};

} // namespace dojo::ui
