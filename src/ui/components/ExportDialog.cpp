#include "ui/components/ExportDialog.h"
#include "licensing/Features.h"

#include "engine/io/LameMp3Format.h"
#include "ui/DeviceActions.h"
#include "ui/theme/DojoTheme.h"

#include <algorithm>
#include <cmath>

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

constexpr int kVideoFormat = 5;
constexpr int kAac = 200;
constexpr int kPcm = 201;

/** m:ss.mmm */
juce::String clockText(double seconds)
{
    const auto minutes = static_cast<int>(std::floor(seconds / 60.0));
    const double rest = seconds - minutes * 60.0;
    return juce::String(minutes) + ":" + (rest < 10.0 ? "0" : "") + juce::String(rest, 3);
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
    format_.addItem("ADM BWF (Dolby Atmos: bed + objects)", 4);
    format_.addItem("Video (the mix in the movie)", kVideoFormat);
    format_.setSelectedId(1, juce::dontSendNotification);
    videoClip_ = session::Exporter::findVideoClip(project_.getSession());
    if (videoClip_.isValid())
        videoSupport_ = video::muxSupport(session::clip::getFile(videoClip_));
    format_.setItemEnabled(kVideoFormat, videoSupport_.canMux());
    format_.setItemEnabled(4, licensing::gate::allows(licensing::Feature::immersive)); // (ADM BWF: Dolby Atmos)
    format_.setTooltip(!videoClip_.isValid() ? "Video needs a video clip (File > Import Video...)"
                       : !videoSupport_.canMux() ? "Nothing here can write " + session::clip::getFile(videoClip_).getFileName() + " with a new sound"
                                                 : juce::String());
    format_.onChange = [this]
    {
        updateQualityChoices();
        updateForFormat();
        if (file_ != juce::File())
            file_ = file_.withFileExtension(getExtension());
        destination_.setText(file_.getFullPathName(), juce::dontSendNotification);
    };
    quality_.onChange = [this]
    {
        updateForFormat();
        if (file_ != juce::File())
            file_ = file_.withFileExtension(getExtension()); // (a video's PCM: a MOV)
        destination_.setText(file_.getFullPathName(), juce::dontSendNotification);
    };

    range_.addItem("Whole session", 1);
    range_.addItem("Loop range", 2);
    range_.setSelectedId(project_.getSession().isLoopEnabled() ? 2 : 1, juce::dontSendNotification);

    mode_.addItem("Offline (faster than real time)", 1);
    mode_.addItem("Real time (through the audio device)", 2);
    mode_.setSelectedId(1, juce::dontSendNotification);
    if (deviceActions::usesHardwareInserts(project_.getSession()))
    {
        // Outboard gear is only there in real time.
        mode_.setSelectedId(2, juce::dontSendNotification);
        mode_.setItemEnabled(1, false);
        mode_.setTooltip("The project plays through a HW Insert: it can only be bounced in real time, through the sound card");
    }

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
    styleLabel(hint_, {});
    hint_.setFont(theme::sans(11.5f));
    hint_.setJustificationType(juce::Justification::topLeft);
    addChildComponent(hint_);

    updateQualityChoices();
    updateForFormat();
    setSize(520, 452);
}

ExportDialog::~ExportDialog()
{
    stopTimer();
}

void ExportDialog::show(session::Project& project, session::Exporter& exporter, juce::Component* centreAround)
{
    if (!licensing::gate::allows(licensing::Feature::exporting))
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Export Audio",
                                               licensing::gate::why(licensing::Feature::exporting));
        return;
    }
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
    case 4: return session::ExportSettings::Format::admBwf;
    case kVideoFormat: return session::ExportSettings::Format::video;
    default: return session::ExportSettings::Format::wav;
    }
}

juce::String ExportDialog::getExtension() const
{
    if (getFormat() == session::ExportSettings::Format::video && videoSupport_.extension.isNotEmpty())
        return quality_.getSelectedId() == kPcm ? videoSupport_.pcmExtension : videoSupport_.extension;
    return session::Exporter::getFileExtension(getFormat());
}

void ExportDialog::updateForFormat()
{
    const bool adm = getFormat() == session::ExportSettings::Format::admBwf;
    const bool video = getFormat() == session::ExportSettings::Format::video;
    mix_.setVisible(!adm && !video);
    stems_.setVisible(!adm && !video);
    hint_.setVisible(adm || video);
    // A video is the clip's stretch of the timeline, with no tail (the movie ends with it).
    range_.setEnabled(!video);
    tail_.setEnabled(!video);
    if (video)
    {
        mode_.setSelectedId(1, juce::dontSendNotification);
        const auto& session = project_.getSession();
        const double start = session::clip::getStart(videoClip_);
        juce::String text = "A copy of " + session::clip::getFile(videoClip_).getFileName()
                            + " with the mix as its sound (the picture is copied, not re-encoded), for the clip's " + clockText(start) + " - "
                            + clockText(start + session::clip::getLength(videoClip_)) + " on the timeline. " + videoSupport_.backend + ", offline.";
        const auto master = session.getMasterFormat();
        const int channels = session::isMultichannel(master) ? session::getLayout(master).numChannels : session::getNumChannels(master);
        if (quality_.getSelectedId() == kAac && !videoSupport_.aacHolds(channels))
            text << " Its AAC holds " << (videoSupport_.aacSurround ? "mono, stereo or 5.1" : "up to 7.1") << ": the "
                 << session::getDisplayName(master) << " mix goes in as stereo" << (videoSupport_.pcm ? " (PCM keeps every channel)." : ".");
        if (session::clip::isLooped(videoClip_))
            text = "The video clip is looped, and a copy of the movie cannot repeat a stretch of it: turn its loop off (Clip > Loop) first.";
        hint_.setText(text, juce::dontSendNotification);
    }
    if (adm)
    {
        // An ADM BWF file is rendered offline: each object needs its own channels.
        mode_.setSelectedId(1, juce::dontSendNotification);
        int objects = 0;
        for (const auto& t : project_.getSession().getAllTracks())
            if (session::track::isObject(t) && session::track::isPan3d(t) && session::Session::isEffectivelyActive(t))
                ++objects;
        const auto master = project_.getSession().getMasterFormat();
        hint_.setText("One BW64 file (ITU-R BS.2076): the bed (" + session::getDisplayName(master) + ", every track that is not an object) and "
                             + (objects == 0 ? juce::String("no objects yet (Track > ADM Object on 3D-panned tracks)")
                                             : juce::String(objects) + (objects == 1 ? " object track" : " object tracks"))
                             + ", each with its movement. Rendered offline.",
                         juce::dontSendNotification);
    }
    mode_.setItemEnabled(2, !adm && !video && !deviceActions::usesHardwareInserts(project_.getSession()));
}

void ExportDialog::updateQualityChoices()
{
    quality_.clear(juce::dontSendNotification);
    switch (getFormat())
    {
    case session::ExportSettings::Format::wav:
    case session::ExportSettings::Format::admBwf:
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
    case session::ExportSettings::Format::video:
        if (videoSupport_.aacChannels > 0)
        {
            // 320 kbps for each pair of channels the AAC carries (Media Foundation's encoder: up to
            // 192 kbps in all).
            const auto master = project_.getSession().getMasterFormat();
            int channels = session::isMultichannel(master) ? session::getLayout(master).numChannels : 2;
            if (!videoSupport_.aacHolds(channels))
                channels = 2;
            int bitrate = session::Exporter::videoAacBitrate(session::ExportSettings{}, channels);
            if (videoSupport_.aacMaxBitrate > 0)
                bitrate = std::min(bitrate, videoSupport_.aacMaxBitrate);
            const int kbps = bitrate / 1000;
            quality_.addItem("AAC " + juce::String(kbps) + " kbps", kAac);
        }
        if (videoSupport_.pcm)
            quality_.addItem("PCM 24-bit (lossless)", kPcm);
        quality_.setSelectedItemIndex(0, juce::dontSendNotification);
        break;
    }
}

void ExportDialog::chooseDestination()
{
    chooser_ = std::make_unique<juce::FileChooser>("Export to", file_, "*" + getExtension());
    juce::Component::SafePointer<ExportDialog> safe(this);
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                              juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe](const juce::FileChooser& chooser)
                          {
                              if (safe == nullptr || chooser.getResult() == juce::File())
                                  return;
                              safe->file_ = chooser.getResult().withFileExtension(safe->getExtension());
                              safe->destination_.setText(safe->file_.getFullPathName(), juce::dontSendNotification);
                          });
}

void ExportDialog::startExport()
{
    auto& session = project_.getSession();
    // Pitch edits still rendering are waited for (the export hears what the clips will play).
    if (!project_.getAudioEdits().waitUntilIdle(120'000))
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Export",
                                               "Some audio edits are still rendering; their clips are exported as they were.");
    session::ExportSettings settings;
    settings.destination = file_;
    settings.format = getFormat();
    if (settings.format == session::ExportSettings::Format::mp3)
        settings.mp3QualityIndex = quality_.getSelectedId() - 100;
    else if (settings.format == session::ExportSettings::Format::video)
        settings.videoAudio = quality_.getSelectedId() == kPcm ? video::MuxAudioCodec::pcm : video::MuxAudioCodec::aac;
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
    auto toggles = area.removeFromTop(48);
    hint_.setBounds(toggles.withTrimmedLeft(90));
    toggles = toggles.removeFromTop(26);
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
