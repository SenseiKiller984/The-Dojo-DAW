#pragma once

#include "session/AudioEdits.h"
#include "session/Project.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace dojo::ui
{

/**
    The spectral editor (Clip Editor > SPECTRAL): an audio clip's spectrogram (time across, frequency
    up on a log scale from 20 Hz), edited by regions.

    Drag to select a time-frequency box (Shift: all frequencies). On the selection: Attenuate (or
    boost) by the gain shown, Erase, Repair (rebuild it from what surrounds it: clicks, coughs,
    squeaks, a phone); Learn Noise takes the selection's sound as a noise profile and De-noise removes
    that noise from the selection's time (or the whole clip), by up to the reduction shown; De-hum
    notches mains hum (found, or 50/60 Hz) and its harmonics. Each is one undo step, listed on the
    right (tick to bypass, cross to remove), drawn on the spectrogram, and rendered in the background;
    the spectrogram shows the result. Original plays the clip as it was (A/B). Ctrl+wheel zooms, the
    wheel scrolls; Escape clears the selection.
*/
class SpectralEditor final : public juce::Component,
                             private juce::ChangeListener,
                             private juce::ValueTree::Listener,
                             private juce::ScrollBar::Listener,
                             private juce::Timer
{
public:
    explicit SpectralEditor(session::Project& project);
    ~SpectralEditor() override;

    void setClip(const juce::ValueTree& clip);
    [[nodiscard]] const juce::ValueTree& getClip() const noexcept { return clip_; }
    void setPlayhead(double timelineSeconds);

    /** Seconds of the clip and Hz. */
    struct Selection
    {
        double start = 0.0;
        double end = 0.0;
        double low = 0.0;
        double high = 1.0e6;
        [[nodiscard]] bool isEmpty() const noexcept { return end <= start; }
    };
    void setSelection(const Selection& selection);
    [[nodiscard]] const Selection& getSelection() const noexcept { return selection_; }

    // --- Actions on the selection (the buttons) --------------------------------------------------------
    void applyGain(double gainDb);
    void erase();
    void repair();
    /** Learns the selection's sound as the noise profile; false (and why, in the status) if it cannot. */
    bool learnNoise();
    [[nodiscard]] bool hasNoiseProfile() const noexcept { return !noiseProfile_.empty(); }
    void denoise(double reductionDb);
    void dehum(double humHz);

    // --- Geometry (public for tests) -------------------------------------------------------------------
    [[nodiscard]] juce::Rectangle<int> getSpectrumArea() const;
    [[nodiscard]] juce::Rectangle<int> getOperationsArea() const;
    [[nodiscard]] float timeToX(double clipSeconds) const;
    [[nodiscard]] double xToTime(float x) const;
    [[nodiscard]] float hzToY(double hz) const;
    [[nodiscard]] double yToHz(float y) const;
    /** The row of the operations list at a point (-1: none), and where its tick and cross are. */
    [[nodiscard]] int operationRowAt(juce::Point<int> p) const;
    [[nodiscard]] juce::Rectangle<int> getOperationTick(int row) const;
    [[nodiscard]] juce::Rectangle<int> getOperationCross(int row) const;
    [[nodiscard]] std::shared_ptr<const audioedit::spectral::Spectrogram> getShownSpectrogram() const noexcept { return shown_; }
    [[nodiscard]] juce::Button& getOriginalButton() noexcept { return originalButton_; }
    [[nodiscard]] juce::String getStatus() const { return status_.getText(); }

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier&) override;
    void valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree&) override;
    void valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree&, int) override;
    void scrollBarMoved(juce::ScrollBar*, double start) override;
    void timerCallback() override;

    [[nodiscard]] bool concerns(const juce::ValueTree& tree) const;
    void refresh();
    void rebuildImage();
    void updateScrollBar();
    void add(audioedit::spectral::Operation op, const juce::String& undoName);
    [[nodiscard]] double topHz() const;
    [[nodiscard]] double clipLength() const;
    /** The selection as an operation's region (the whole clip when nothing is selected, if `wholeIfNone`). */
    [[nodiscard]] audioedit::spectral::Operation fromSelection(audioedit::spectral::Kind kind, bool wholeIfNone) const;

    session::Session& session_;
    session::AudioEdits& edits_;
    juce::ValueTree clip_;
    Selection selection_;
    juce::ValueTree selectedOperation_;
    std::vector<float> noiseProfile_;
    double playhead_ = -1.0;

    std::shared_ptr<const audioedit::spectral::Spectrogram> shown_;
    double shownStart_ = 0.0;
    juce::Image image_;

    double viewStart_ = 0.0;
    double pixelsPerSecond_ = 100.0;
    bool selecting_ = false;
    juce::Point<float> dragStart_;

    juce::TextButton gainButton_{"Attenuate"}, eraseButton_{"Erase"}, repairButton_{"Repair"};
    juce::Slider gainSlider_;
    juce::TextButton learnButton_{"Learn Noise"}, denoiseButton_{"De-noise"};
    juce::Slider reductionSlider_;
    juce::TextButton dehumButton_{"De-hum"};
    juce::ComboBox humBox_;
    juce::TextButton originalButton_{"Original"};
    juce::Label status_;
    juce::ScrollBar hScroll_{false};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectralEditor)
};

} // namespace dojo::ui
