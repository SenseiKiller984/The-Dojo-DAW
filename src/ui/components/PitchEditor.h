#pragma once

#include "session/AudioEdits.h"
#include "session/Project.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

namespace dojo::ui
{

/**
    The pitch editor (Clip Editor > PITCH): an audio clip's notes as blobs on a pitch grid, edited in
    place - the native pitch correction.

    Analyse (Melodic for one voice, Polyphonic for chords) finds the notes; each is drawn where it is
    heard: its centre pitch (the blob, as thick as it is loud), its pitch drift (the line through it)
    and, once moved, where it was (a faint outline).

    Drag a blob up or down to move its pitch (in semitones; Alt: freely, in cents) and, in melodic
    material, sideways to move it in time; drag its ends to change its timing. Double-click snaps a
    note to the nearest note of the scale. Correct moves the selected notes (or all) towards the
    scale by the Pitch amount and straightens their drift by the Drift amount. Below, the selected
    notes' drift and level. Keys: Up/Down a semitone (Shift: 10 cents), Delete resets the selection,
    Ctrl+A selects all, Escape none. Ctrl+wheel zooms, the wheel scrolls the pitch (Shift: time).
    Formants keeps a voice's character when it moves; Original plays the clip as it was (A/B); every
    edit is one undo step, rendered in the background.
*/
class PitchEditor final : public juce::Component,
                          private juce::ChangeListener,
                          private juce::ValueTree::Listener,
                          private juce::Timer,
                          private juce::ScrollBar::Listener
{
public:
    explicit PitchEditor(session::Project& project);
    ~PitchEditor() override;

    void setClip(const juce::ValueTree& clip);
    [[nodiscard]] const juce::ValueTree& getClip() const noexcept { return clip_; }
    void setPlayhead(double timelineSeconds);

    /** Analyses the clip (Melodic or Polyphonic). */
    void analyse(audioedit::pitch::Mode mode);
    /** The Correct button: the selection (or all notes) towards the scale. */
    void correct(double pitchAmount, double driftAmount);
    void setDriftOfSelection(double drift);
    void setGainOfSelection(double gainDb);
    void setScale(const session::audioedits::Scale& scale);

    // --- Geometry and selection (public for tests) ---------------------------------------------------
    [[nodiscard]] juce::Rectangle<int> getNoteArea() const;
    [[nodiscard]] float timeToX(double clipSeconds) const;
    [[nodiscard]] double xToTime(float x) const;
    [[nodiscard]] float pitchToY(double midi) const;
    [[nodiscard]] double yToPitch(float y) const;
    /** A note's blob as drawn (where it is now). */
    [[nodiscard]] juce::Rectangle<float> getNoteBounds(const juce::ValueTree& note) const;
    [[nodiscard]] juce::ValueTree noteAt(juce::Point<float> p) const;
    [[nodiscard]] const std::vector<juce::ValueTree>& getSelection() const noexcept { return selection_; }
    void setSelection(std::vector<juce::ValueTree> notes);
    [[nodiscard]] juce::Button& getAnalyseButton() noexcept { return analyseButton_; }
    [[nodiscard]] juce::Button& getOriginalButton() noexcept { return originalButton_; }
    [[nodiscard]] juce::Button& getFormantsButton() noexcept { return formantsButton_; }
    [[nodiscard]] juce::ComboBox& getModeBox() noexcept { return modeBox_; }

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    enum class Drag
    {
        none,
        move,
        start,
        end,
        lasso,
    };
    struct Original
    {
        juce::ValueTree note;
        double pitch = 60.0;
        double start = 0.0;
        double end = 0.0;
    };

    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier&) override;
    void valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree&) override;
    void valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int) override;
    void timerCallback() override;
    void scrollBarMoved(juce::ScrollBar*, double start) override;

    [[nodiscard]] bool concerns(const juce::ValueTree& tree) const;
    [[nodiscard]] std::vector<juce::ValueTree> notes() const;
    [[nodiscard]] std::vector<juce::ValueTree> targets() const; ///< the selection, or every note
    [[nodiscard]] double clipLength() const;
    [[nodiscard]] Drag dragAt(const juce::ValueTree& note, juce::Point<float> p) const;
    void fitView();
    void updateControls();
    void updateScrollBars();
    void paintNote(juce::Graphics& g, const juce::ValueTree& note, bool selected) const;

    session::Session& session_;
    session::AudioEdits& edits_;
    juce::ValueTree clip_;
    std::vector<juce::ValueTree> selection_;
    session::audioedits::Scale scale_;
    double playhead_ = -1.0;

    // The view: seconds of the clip and MIDI pitches.
    double viewStart_ = 0.0;
    double pixelsPerSecond_ = 120.0;
    double topPitch_ = 84.0;
    float semitoneHeight_ = 12.0f;

    Drag drag_ = Drag::none;
    std::vector<Original> originals_;
    juce::Point<float> dragStart_;
    bool moved_ = false;
    juce::Rectangle<float> lasso_;
    std::vector<juce::ValueTree> lassoBase_;

    juce::ComboBox modeBox_;
    juce::TextButton analyseButton_{"Analyse"};
    juce::TextButton correctButton_{"Correct"};
    juce::Slider pitchAmount_, driftAmount_;
    juce::ComboBox rootBox_, scaleBox_;
    juce::ToggleButton formantsButton_{"Formants"};
    juce::TextButton originalButton_{"Original"};
    juce::TextButton removeButton_{"Remove"};
    juce::Label status_;
    juce::Label selectionInfo_;
    juce::Slider driftSlider_, gainSlider_;
    juce::ScrollBar hScroll_{false}, vScroll_{true};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PitchEditor)
};

} // namespace dojo::ui
