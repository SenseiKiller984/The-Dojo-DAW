#pragma once

#include "ui/devices/DeviceEditors.h"
#include "ui/devices/ParameterControl.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace dojo::devices
{
class LfoDevice;
}

namespace dojo::ui
{

/**
    Editors of the second set of native devices: Gate, Multiband Dynamics, Auto Filter, Channel EQ,
    Pedal, Overdrive, Redux, Roar, Shifter, Chorus-Ensemble, Auto Pan, Analog, Drift, Meld and the
    LFO. Null for any other processor (DeviceEditors falls back to its own).
*/
std::unique_ptr<juce::Component> createMoreDeviceEditor(juce::AudioProcessor& processor, const DeviceEditorContext& context,
                                                        int& preferredWidth);

/**
    The LFO's editor: a scope of its cycle, its controls, and the list of what it moves.
    - Map... lists every track (and the master) with its fader, pan, sends and every parameter of
      every device on it; picking one maps it.
    - Learn: every device control in the project is outlined; click one to map it.
    - Each mapping has its own range (min and max of the target, in %) and a remove button, which
      puts the target back where it was before it was mapped.
*/
class LfoEditor final : public juce::Component, private juce::ValueTree::Listener, private juce::Timer
{
public:
    static constexpr int kScopeWidth = 170;
    static constexpr int kMappingWidth = 330;
    static constexpr int kRowHeight = 20;

    LfoEditor(devices::LfoDevice& lfo, const DeviceEditorContext& context);
    ~LfoEditor() override;

    [[nodiscard]] static int preferredWidth();

    /** A target the Map menu offers. */
    struct Candidate
    {
        std::uint32_t trackId = 0; ///< 0 = the master
        juce::String target;       ///< "volume", "pan", "send:<id>", "param:<device>:<index>"
        double base = 0.5;         ///< its value now (normalised)
        juce::String description;
    };
    /** Everything that can be mapped, in menu order (the LFO's own controls excluded). */
    [[nodiscard]] std::vector<Candidate> getCandidates() const;
    /** Maps a candidate (one undo step). False if it is mapped already or the list is full. */
    bool map(const Candidate& candidate);
    /** Maps a parameter of any device in the project (what Learn does on a click). */
    bool mapParameter(juce::AudioProcessorParameter& parameter);
    /** Removes mapping `index`, giving its target back its value from before (one undo step). */
    void unmap(int index);
    /** "Track > Device > Parameter" (or "Track > Volume", "Master > Volume", "Track > Send to Bus"). */
    [[nodiscard]] juce::StringArray getMappingDescriptions() const;
    [[nodiscard]] juce::String describe(const juce::ValueTree& mapping) const;

    void beginLearn();
    void endLearn();
    [[nodiscard]] bool isLearning() const noexcept { return learning_; }

    void showMapMenu();
    juce::TextButton mapButton{"Map..."}, learnButton{"Learn"};

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    class Scope;
    class Row;

    void rebuildRows();
    void timerCallback() override;
    void valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree&) override;
    void valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree&, int) override;
    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier&) override;

    devices::LfoDevice& lfo_;
    session::Project& project_;
    juce::ValueTree device_;
    std::unique_ptr<Scope> scope_;
    ParameterGrid grid_;
    juce::Viewport rowsViewport_;
    juce::Component rowsHolder_;
    std::vector<std::unique_ptr<Row>> rows_;
    juce::Label status_;
    bool learning_ = false;
    bool rebuildPending_ = false;
};

} // namespace dojo::ui
