#include "ui/components/ExportDialog.h"

#include "engine/io/LameMp3Format.h"
#include "ui/theme/DojoTheme.h"

namespace dojo::ui
{
namespace
{
void styleLabel(juce::Label& label, const juce::String& text)
{
    label.setText(text, juce::dontSendNotification);
    label.setFont(theme::sans(12.5f));
    label.setColour(juce::Label::textColourId, theme::colour::textSecondary);
}
} // namespace

ExportDialog::ExportDialog(session::Project& project, session::Exporter& exporter)
    : project_(project)
    , exporter_(exporter)
{
    styleLabel(formatLabel_, "Format");
    styleLabel(qualityLabel_, "Quality");
    styleLabel(rangeLabel_, "Range");
    styleLabel(tailLabel_, "Tail");
    styleLabel(modeLabel_, "Render");
    styleLabel(destinationLabel_, "Save as");

    format_.addItem("WAV", 1);
    format_.addItem("FLAC", 2);
    format_.addItem("MP3 (LAME)", 3);
    format_.setSelectedId(1, juce::dontSendNotification);
    format_.onChange = [this]
    {
        updateQualityChoices();
        if (file_ != juce::File())
            file_ = file_.withFileExtension(session::Exporter::getFileExtension(getFormat()));
        destination_.setText(file_.getFullPathName(), juce::dontSendNotification);
    };

    range_.addItem("Whole session", 1);
    range_.addItem("Loop range", 2);
    range_.setSelectedId(project_.getSession().isLoopEnabled() ? 2 : 1, juce::dontSendNotification);

    mode_.addItem("Offline (faster than real time)", 1);
    mode_.addItem("Real time (through the audio device)", 2);
    mode_.setSelectedId(1, juce::dontSendNotification);

    tail_.setSliderStyle(juce::Slider::LinearHorizontal);
    tail_.setRange(0.0, 20.0, 0.1);
    tail_.setValue(2.0, juce::dontSendNotification);
    tail_.setTextValueSuffix(" s");
    tail_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 22);
    tail_.setTooltip("Extra time after the end, so reverbs and delays ring out");

    mix_.setToggleState(true, juce::dontSendNotification);

    const auto base = project_.getFile() != juce::File() ? project_.getFile().getParentDirectory()
                                                          : juce::File::getSpecialLocation(juce::File::userMusicDirectory);
    file_ = base.getChildFile(juce::File::createLegalFileName(project_.getDisplayName()) + ".wav");
    destination_.setText(file_.getFullPathName(), juce::dontSendNotification);
    destination_.setColour(juce::Label::textColourId, theme::colour::textPrimary);
    destination_.setColour(juce::Label::backgroundColourId, theme::colour::meterBackground);
    destination_.setFont(theme::sans(12.0f));

    chooseButton_.onClick = [this] { chooseDestination(); };
    exportButton_.onClick = [this] { startExport(); };
    cancelButton_.onClick = [this]
    {
        if (exporter_.isRunning())
            exporter_.cancel();
        else if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState(0);
    };

    for (auto* c : std::initializer_list<juce::Component*>{&formatLabel_, &qualityLabel_, &rangeLabel_, &tailLabel_, &modeLabel_,
                                                           &destinationLabel_, &format_, &quality_, &range_, &mode_, &tail_, &mix_,
                                                           &stems_, &destination_, &chooseButton_, &exportButton_, &cancelButton_})
        addAndMakeVisible(c);
    addChildComponent(progressBar_);

    updateQualityChoices();
    setSize(520, 420);
}

ExportDialog::~ExportDialog()
{
    stopTimer();
}

void ExportDialog::show(session::Project& project, session::Exporter& exporter, juce::Component* centreAround)
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(new ExportDialog(project, exporter));
    options.dialogTitle = "Export Audio";
    options.dialogBackgroundColour = theme::colour::panel;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = false;
    options.resizable = false;
    options.componentToCentreAround = centreAround;
    options.launchAsync();
}

session::ExportSettings::Format ExportDialog::getFormat() const
{
    switch (format_.getSelectedId())
    {
    case 2: return session::ExportSettings::Format::flac;
    case 3: return session::ExportSettings::Format::mp3;
    default: return session::ExportSettings::Format::wav;
    }
}

void ExportDialog::updateQualityChoices()
{
    quality_.clear(juce::dontSendNotification);
    switch (getFormat())
    {
    case session::ExportSettings::Format::wav:
        quality_.addItem("16-bit", 16);
        quality_.addItem("24-bit", 24);
        quality_.addItem("32-bit float", 32);
        quality_.setSelectedId(24, juce::dontSendNotification);
        break;
    case session::ExportSettings::Format::flac:
        quality_.addItem("16-bit", 16);
        quality_.addItem("24-bit", 24);
        quality_.setSelectedId(24, juce::dontSendNotification);
        break;
    case session::ExportSettings::Format::mp3:
    {
        const auto& rates = engine::LameMp3Format::getBitRates();
        for (int i = 0; i < rates.size(); ++i)
            quality_.addItem(juce::String(rates[i]) + " kbps", 100 + i);
        quality_.setSelectedId(100 + 4, juce::dontSendNotification);
        break;
    }
    }
}

void ExportDialog::chooseDestination()
{
    chooser_ = std::make_unique<juce::FileChooser>("Export to", file_, "*" + session::Exporter::getFileExtension(getFormat()));
    juce::Component::SafePointer<ExportDialog> safe(this);
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                              juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe](const juce::FileChooser& chooser)
                          {
                              if (safe == nullptr || chooser.getResult() == juce::File())
                                  return;
                              safe->file_ = chooser.getResult().withFileExtension(session::Exporter::getFileExtension(safe->getFormat()));
                              safe->destination_.setText(safe->file_.getFullPathName(), juce::dontSendNotification);
                          });
}

void ExportDialog::startExport()
{
    auto& session = project_.getSession();
    session::ExportSettings settings;
    settings.destination = file_;
    settings.format = getFormat();
    if (settings.format == session::ExportSettings::Format::mp3)
        settings.mp3QualityIndex = quality_.getSelectedId() - 100;
    else
        settings.bitDepth = quality_.getSelectedId();
    if (range_.getSelectedId() == 2)
    {
        settings.startSeconds = session.getLoopStart();
        settings.endSeconds = session.getLoopEnd();
    }
    else
    {
        settings.startSeconds = 0.0;
        settings.endSeconds = std::max(1.0, session.getEndTime());
    }
    settings.tailSeconds = tail_.getValue();
    settings.realtime = mode_.getSelectedId() == 2;
    settings.includeMix = mix_.getToggleState();
    settings.includeStems = stems_.getToggleState();

    if (!file_.getParentDirectory().createDirectory())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Export", "Could not create " + file_.getParentDirectory().getFullPathName());
        return;
    }

    juce::Component::SafePointer<ExportDialog> safe(this);
    exporter_.onFinished = [safe](const juce::Result& result, const juce::Array<juce::File>& files)
    {
        if (safe != nullptr)
        {
            safe->stopTimer();
            safe->progressBar_.setVisible(false);
            safe->exportButton_.setEnabled(true);
            safe->cancelButton_.setButtonText("Close");
        }

        if (result.failed())
        {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Export failed", result.getErrorMessage());
            return;
        }
        juce::StringArray names;
        for (const auto& f : files)
            names.add(f.getFullPathName());
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Export finished",
                                               "Wrote " + juce::String(files.size()) + (files.size() == 1 ? " file:\n" : " files:\n") +
                                                   names.joinIntoString("\n"));
    };

    if (const auto error = exporter_.start(settings); error.isNotEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Export", error);
        return;
    }

    exportButton_.setEnabled(false);
    cancelButton_.setButtonText("Cancel");
    progressBar_.setVisible(true);
    startTimerHz(20);
}

void ExportDialog::timerCallback()
{
    progress_ = exporter_.getProgress();
}

void ExportDialog::resized()
{
    auto area = getLocalBounds().reduced(18, 16);
    const auto row = [&area](juce::Label& label, juce::Component& control)
    {
        auto r = area.removeFromTop(28);
        label.setBounds(r.removeFromLeft(90));
        control.setBounds(r);
        area.removeFromTop(8);
    };
    row(formatLabel_, format_);
    row(qualityLabel_, quality_);
    row(rangeLabel_, range_);
    row(tailLabel_, tail_);
    row(modeLabel_, mode_);
    auto toggles = area.removeFromTop(26);
    mix_.setBounds(toggles.removeFromLeft(200));
    stems_.setBounds(toggles);
    area.removeFromTop(10);
    auto dest = area.removeFromTop(28);
    destinationLabel_.setBounds(dest.removeFromLeft(90));
    chooseButton_.setBounds(dest.removeFromRight(90));
    dest.removeFromRight(6);
    destination_.setBounds(dest);
    area.removeFromTop(14);
    progressBar_.setBounds(area.removeFromTop(22));
    auto buttons = area.removeFromBottom(32);
    exportButton_.setBounds(buttons.removeFromRight(110));
    buttons.removeFromRight(8);
    cancelButton_.setBounds(buttons.removeFromRight(110));
}

void ExportDialog::paint(juce::Graphics& g)
{
    g.fillAll(theme::colour::panel);
}

} // namespace dojo::ui
