#pragma once

#include "session/Exporter.h"
#include "session/Project.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace dojo::ui
{

/**
    Export Audio dialog: format (WAV 16/24/32-bit float, FLAC 16/24-bit, MP3 128..320 kbps), range
    (whole session or loop), tail, offline (faster than real time) or real-time bounce, the mix
    and/or one stem per track, and the destination file. Shows progress and can cancel.
*/
class ExportDialog final : public juce::Component, private juce::Timer
{
public:
    ExportDialog(session::Project& project, session::Exporter& exporter);
    ~ExportDialog() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    /** Opens the dialog in a window. */
    static void show(session::Project& project, session::Exporter& exporter, juce::Component* centreAround);

private:
    void updateQualityChoices();
    void chooseDestination();
    void startExport();
    void timerCallback() override;
    [[nodiscard]] session::ExportSettings::Format getFormat() const;

    session::Project& project_;
    session::Exporter& exporter_;

    juce::Label formatLabel_, qualityLabel_, rangeLabel_, tailLabel_, modeLabel_, destinationLabel_;
    juce::ComboBox format_, quality_, range_, mode_;
    juce::Slider tail_;
    juce::ToggleButton mix_{"Mix (master output)"};
    juce::ToggleButton stems_{"Stems (one file per track)"};
    juce::Label destination_;
    juce::TextButton chooseButton_{"Choose..."};
    juce::TextButton exportButton_{"Export"};
    juce::TextButton cancelButton_{"Cancel"};
    double progress_ = 0.0;
    juce::ProgressBar progressBar_{progress_};
    juce::File file_;
    std::unique_ptr<juce::FileChooser> chooser_;
};

} // namespace dojo::ui
