#pragma once

#include "engine/EngineController.h"
#include "session/Project.h"
#include "ui/components/LevelMeter.h"
#include "ui/EditorState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace dojo::ui
{

/**
    One track's channel strip: name, pan, mute/solo/phase, fader and meter. Every control edits the
    session (undoably) and follows external changes such as undo or the arranger's M/S buttons.
*/
class ChannelStrip final : public juce::Component,
                           private juce::ValueTree::Listener,
                           private EditorState::Listener
{
public:
    ChannelStrip(session::Session& session, EditorState& editor, juce::ValueTree track);
    ~ChannelStrip() override;

    [[nodiscard]] const juce::ValueTree& getTrack() const noexcept { return track_; }
    void pushLevels(float peakLeft, float peakRight, double elapsedSeconds);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;

private:
    void syncFromModel();
    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier&) override;
    void editorStateChanged() override { repaint(); }

    session::Session& session_;
    EditorState& editor_;
    juce::ValueTree track_;

    juce::Slider pan_;
    juce::TextButton mute_;
    juce::TextButton solo_;
    juce::TextButton phase_;
    juce::Slider fader_;
    LevelMeter meter_;
    juce::Rectangle<int> nameArea_;
    juce::Rectangle<int> readoutArea_;
    bool panDragging_ = false;
    bool faderDragging_ = false;
};

/** The master strip: fader (session master volume, undoable), meter with scale, gain/peak readouts. */
class MasterStrip final : public juce::Component, private juce::ValueTree::Listener
{
public:
    explicit MasterStrip(session::Session& session);
    ~MasterStrip() override;

    void pushLevels(float peakLeft, float peakRight, double elapsedSeconds);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property) override;
    void valueTreeRedirected(juce::ValueTree&) override;

    session::Session& session_;
    juce::Slider fader_;
    LevelMeter meter_;
    juce::Rectangle<int> scaleArea_;
    juce::Rectangle<int> readoutArea_;
    float heldPeakDb_ = LevelMeter::kMinDb;
    double peakHoldAge_ = 0.0;
    bool dragging_ = false;
};

/** Bottom mixer panel: a scrollable row of channel strips and the master strip. */
class MixerPanel final : public juce::Component, private juce::ValueTree::Listener
{
public:
    MixerPanel(session::Project& project, engine::EngineController& controller, EditorState& editor);
    ~MixerPanel() override;

    /** Pulls every track's post-fader peaks from the engine and feeds the meters. Once per UI frame. */
    void updateMeters(const engine::TelemetrySnapshot& telemetry, double elapsedSeconds);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void rebuildStrips();
    void layoutStrips();
    void valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree&) override;
    void valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged(juce::ValueTree& parent, int, int) override;
    void valueTreeRedirected(juce::ValueTree&) override { rebuildStrips(); }

    session::Project& project_;
    engine::EngineController& controller_;
    EditorState& editor_;

    juce::Viewport viewport_;
    juce::Component stripHolder_;
    std::vector<std::unique_ptr<ChannelStrip>> strips_;
    MasterStrip master_;
};

} // namespace dojo::ui
