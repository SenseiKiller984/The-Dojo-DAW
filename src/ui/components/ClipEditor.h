#pragma once

#include "engine/EngineController.h"
#include "session/Project.h"
#include "ui/EditorState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace dojo::ui
{

/**
    Piano roll for one MIDI clip.

      grid        click empty space to draw a note (drag right to set its length), drag a note to
                  move it (time and pitch), drag its right edge to resize, double-click to delete,
                  Shift-click to add to the selection, right-click for more
      keyboard    Delete removes the selection, Ctrl+A selects all, Q quantises, Up/Down transpose
                  (Shift: an octave), Left/Right move by one grid step
      lower lane  velocity bars (drag to set), or a controller lane (CC 1/7/10/11/64, pitch bend):
                  click/drag to draw values, right-click to delete a point
    Time snaps to the editor grid (hold Alt to bypass). Every gesture is one undo step.
*/
class PianoRoll final : public juce::Component, private juce::ValueTree::Listener
{
public:
    PianoRoll(session::Session& session, EditorState& editor);
    ~PianoRoll() override;

    void setClip(const juce::ValueTree& midiClip);
    void setPlayhead(double seconds);
    /** 0..127 = CC number, 128 = pitch bend, -1 = velocity. */
    void setLowerLane(int controller);
    void quantiseSelection(double strength, bool ends);
    void zoomToFit();
    /** Grid of the piano roll in beats (also the length of drawn notes). */
    void setGridBeats(double beats);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    enum class Drag
    {
        none,
        move,
        resize,
        lane,
    };

    struct NoteSnapshot
    {
        juce::ValueTree note;
        double start;
        double length;
        int pitch;
    };

    [[nodiscard]] bool isLocked() const; ///< the clip is edit-locked (read-only)
    [[nodiscard]] juce::Rectangle<int> gridArea() const;
    [[nodiscard]] juce::Rectangle<int> laneArea() const;
    [[nodiscard]] juce::Rectangle<int> keyboardArea() const;
    [[nodiscard]] float beatToX(double beat) const;
    [[nodiscard]] double xToBeat(float x) const;
    [[nodiscard]] float pitchToY(int pitch) const;
    [[nodiscard]] int yToPitch(float y) const;
    [[nodiscard]] double snapBeat(double beat, bool bypass) const;
    [[nodiscard]] double gridBeats() const;
    [[nodiscard]] double contentStart() const;
    [[nodiscard]] double contentEnd() const;
    [[nodiscard]] juce::ValueTree noteAt(juce::Point<float> p, bool& onRightEdge) const;
    [[nodiscard]] bool isSelected(const juce::ValueTree& note) const;
    void select(const juce::ValueTree& note, bool add);
    void editLane(const juce::MouseEvent& e, bool erase);
    void deleteSelection();
    void moveSelection(double beats, int semitones);

    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override { repaint(); }
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override { repaint(); }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree& child, int) override;
    void valueTreeChildOrderChanged(juce::ValueTree&, int, int) override { repaint(); }

    session::Session& session_;
    EditorState& editor_;
    juce::ValueTree clip_;
    std::vector<juce::ValueTree> selection_;
    int lowerLane_ = -1;
    double gridBeats_ = 0.25;
    double pixelsPerBeat_ = 80.0;
    double scrollBeats_ = 0.0;
    float keyHeight_ = 12.0f;
    int scrollPitch_ = 84; ///< pitch at the top of the grid
    double playheadSeconds_ = 0.0;

    Drag drag_ = Drag::none;
    bool dragChanged_ = false;
    juce::Point<float> dragStart_;
    std::vector<NoteSnapshot> dragNotes_;
};

/**
    Step sequencer view of a MIDI clip: one row per pitch (the 16 Drum Rack pads when the track plays
    a Drum Rack, otherwise 16 consecutive notes), one column per step. Click a cell to toggle a note;
    drag across cells to paint or erase; right-click a cell to cycle its velocity.
*/
class StepSequencer final : public juce::Component, private juce::ValueTree::Listener
{
public:
    StepSequencer(session::Project& project, EditorState& editor);
    ~StepSequencer() override;

    void setClip(const juce::ValueTree& midiClip);
    void setStepBeats(double beats);
    void setBaseNote(int note);
    void setPlayhead(double seconds);
    /** True when the clip's track plays a Drum Rack (rows are its pads). */
    [[nodiscard]] bool usesDrumRack() const;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

private:
    [[nodiscard]] int rowPitch(int row) const;
    [[nodiscard]] juce::String rowName(int row) const;
    [[nodiscard]] int numSteps() const;
    [[nodiscard]] juce::Rectangle<int> cellBounds(int row, int step) const;
    [[nodiscard]] bool cellAt(juce::Point<int> p, int& row, int& step) const;
    [[nodiscard]] juce::ValueTree noteInCell(int row, int step) const;
    void setCell(int row, int step, bool on);

    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override { repaint(); }
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override { repaint(); }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override { repaint(); }

    session::Project& project_;
    EditorState& editor_;
    juce::ValueTree clip_;
    double stepBeats_ = 0.25;
    int baseNote_ = 60;
    double playheadSeconds_ = 0.0;
    bool painting_ = false;
    bool paintOn_ = true;
    int lastRow_ = -1;
    int lastStep_ = -1;
};

/** Properties of an audio clip (gain, time-stretch, pitch, fades) and of its track (speed, pitch). */
class AudioClipInspector final : public juce::Component, private juce::ValueTree::Listener
{
public:
    explicit AudioClipInspector(session::Session& session);
    ~AudioClipInspector() override;

    void setClip(const juce::ValueTree& clip);
    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    struct Field
    {
        juce::Label label;
        juce::Slider slider;
    };

    void sync();
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override { sync(); }

    session::Session& session_;
    juce::ValueTree clip_;
    juce::ValueTree track_;
    std::array<Field, 7> fields_;
    juce::TextButton matchTempo_;
};

/**
    The bottom "Editor" tab: the clip chosen in the arranger (double-click) in a piano roll or step
    sequencer (MIDI) or the clip inspector (audio), with a toolbar for grid, quantise and lanes.
*/
class ClipEditor final : public juce::Component, private EditorState::Listener, private juce::ValueTree::Listener
{
public:
    ClipEditor(session::Project& project, EditorState& editor);
    ~ClipEditor() override;

    void setPlayhead(double seconds);
    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void editorStateChanged() override;
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree& child, int) override;
    void showClip();

    session::Project& project_;
    EditorState& editor_;
    juce::ValueTree clip_;

    juce::TextButton notesButton_;
    juce::TextButton stepsButton_;
    juce::ComboBox gridBox_;
    juce::ComboBox laneBox_;
    juce::TextButton quantiseButton_;
    juce::Slider strengthSlider_;
    juce::ToggleButton endsToggle_;
    juce::ComboBox baseNoteBox_;

    PianoRoll pianoRoll_;
    StepSequencer steps_;
    AudioClipInspector inspector_;
    bool showSteps_ = false;
};

} // namespace dojo::ui
