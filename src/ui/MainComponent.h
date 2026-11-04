#pragma once

#include "engine/EngineController.h"
#include "ui/components/ArrangerView.h"
#include "ui/components/BrowserPanel.h"
#include "ui/components/MixerPanel.h"
#include "ui/components/StatusBar.h"
#include "ui/components/TransportBar.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>

namespace dojo::ui
{

/**
    Root content component of the main window.

        +-------------------------------------------------------------+
        | TransportBar                                                |
        +-----------+-------------------------------------------------+
        |           | ArrangerView (ruler / track headers / lanes)    |
        | Browser   |                                                 |
        |           +-------------------------------------------------+
        |           | MixerPanel (channel strips ............ master) |
        +-----------+-------------------------------------------------+
        | StatusBar                                                   |
        +-------------------------------------------------------------+

    UI refresh is driven by a juce::VBlankAttachment: once per display refresh the component polls
    the engine's lock-free telemetry and pushes it into the children. The audio thread never calls
    into the UI — the UI pulls.
*/
class MainComponent final : public juce::Component
{
public:
    explicit MainComponent(engine::EngineController& controller);
    ~MainComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

    /** Number of UI frames processed so far (used by the start-up smoke test). */
    [[nodiscard]] std::uint64_t getFrameCount() const noexcept { return frameCount_; }

private:
    void onDisplayRefresh(double timestampSeconds);
    void refreshStatusBar();

    engine::EngineController& controller_;

    TransportBar transport_;
    BrowserPanel browser_;
    ArrangerView arranger_;
    MixerPanel mixer_;
    StatusBar statusBar_;

    double lastFrameTimestamp_ = 0.0;
    double lastStatusRefresh_ = 0.0;
    float loadPeakSinceStatusRefresh_ = 0.0f;
    std::uint64_t frameCount_ = 0;

    juce::VBlankAttachment vblank_; // last: must not fire before the children exist

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace dojo::ui
