// DojoTestAra: a small ARA 2 plug-in (VST3) for The Dojo's ARA hosting tests. Not shipped.
//
//  - Analysis: before it first renders a region, it reads the region's whole audio source through
//    the host's audio reader and keeps the source's peak.
//  - Archive: the peaks are stored in the host's document archive (by the sources' persistent IDs)
//    and restored from it; a restored peak is not measured again.
//  - Playback: every playback region's audio, read through the host, normalised to a peak of 0.5
//    with its source's peak. So a host test can tell the plug-in, not the host, played the audio,
//    that it played the right part of the source at the right time, and that a restored archive
//    was used.
//  - Editor: its editor view lists the selected playback regions (and takes the host's selection
//    even with no editor open, unlike a real plug-in, so headless tests can see it). Selecting a
//    region named "play from <seconds>" asks the host to loop two seconds from there and play.
//  - Telemetry for the tests, as read-only parameters: how many regions are selected in the editor
//    view, how many sources it has analysed, how many peaks it has restored (each count / 64).

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <map>

namespace
{
using namespace juce;

constexpr float kCountScale = 64.0f;
constexpr float kTargetPeak = 0.5f;

/** The counts the tests read (set from the message thread). */
struct Telemetry
{
    AudioParameterFloat* selected = nullptr;
    AudioParameterFloat* analysed = nullptr;
    AudioParameterFloat* restored = nullptr;

    static void bump(AudioParameterFloat* parameter, int by = 1)
    {
        if (parameter != nullptr)
            parameter->setValueNotifyingHost(parameter->get() / kCountScale + static_cast<float>(by) / kCountScale);
    }
    static void set(AudioParameterFloat* parameter, int count)
    {
        if (parameter != nullptr)
            parameter->setValueNotifyingHost(static_cast<float>(count) / kCountScale);
    }
};

/** An audio source with the peak the plug-in measured or restored (0: not known yet). */
class TestAudioSource final : public ARAAudioSource
{
public:
    using ARAAudioSource::ARAAudioSource;
    std::atomic<float> peak{0.0f};
};

class TestDocumentController;

class TestPlaybackRenderer final : public ARAPlaybackRenderer
{
public:
    TestPlaybackRenderer(ARA::PlugIn::DocumentController* dc, TestDocumentController& owner)
        : ARAPlaybackRenderer(dc)
        , owner_(owner)
    {
    }

    void prepareToPlay(double sampleRate, int maximumSamplesPerBlock, int numChannels, AudioProcessor::ProcessingPrecision,
                       AlwaysNonRealtime) override;

    void releaseResources() override { readers_.clear(); }

    bool processBlock(AudioBuffer<float>& buffer, AudioProcessor::Realtime, const AudioPlayHead::PositionInfo& position) noexcept override
    {
        buffer.clear();
        if (!position.getIsPlaying())
            return true;
        const auto time = position.getTimeInSamples().orFallback(0);
        const auto block = Range<int64>::withStartAndLength(time, buffer.getNumSamples());
        for (auto* region : getPlaybackRegions())
        {
            const auto played = region->getSampleRange(sampleRate_, ARAPlaybackRegion::IncludeHeadAndTail::no);
            auto range = block.getIntersectionWith(played);
            if (range.isEmpty())
                continue;
            const auto offset = region->getStartInAudioModificationSamples() - played.getStart();
            auto* source = region->getAudioModification()->getAudioSource<TestAudioSource>();
            const auto reader = readers_.find(source);
            if (reader == readers_.end() || source->peak.load() <= 0.0f)
                continue;
            range = range.getIntersectionWith(Range<int64>(-offset, source->getSampleCount() - offset));
            if (range.isEmpty())
                continue;
            const auto count = static_cast<int>(range.getLength());
            const auto at = static_cast<int>(range.getStart() - block.getStart());
            if (!reader->second->read(&scratch_, 0, count, range.getStart() + offset, true, true))
                continue;
            const float gain = kTargetPeak / source->peak.load();
            for (int c = 0; c < buffer.getNumChannels(); ++c)
                buffer.addFrom(c, at, scratch_, std::min(c, scratch_.getNumChannels() - 1), 0, count, gain);
        }
        return true;
    }

    using ARAPlaybackRenderer::processBlock;

private:
    TestDocumentController& owner_;
    std::map<ARAAudioSource*, std::unique_ptr<ARAAudioSourceReader>> readers_;
    AudioBuffer<float> scratch_;
    double sampleRate_ = 48000.0;
};

/** The editor view: tells the editor (and the tests) which regions are selected. */
class TestEditorView final : public ARAEditorView
{
public:
    using ARAEditorView::ARAEditorView;

    void doNotifySelection(const ARA::PlugIn::ViewSelection* selection) noexcept override
    {
        ARAEditorView::doNotifySelection(selection);
        StringArray names;
        for (auto* region : selection->getPlaybackRegions<ARAPlaybackRegion>())
            names.add(String(region->getName() != nullptr ? region->getName() : "(region)"));
        selected = names;
        Telemetry::set(telemetry != nullptr ? telemetry->selected : nullptr, names.size());
        if (onChange)
            onChange();

        // A region named "play from <seconds>" asks the host to loop two seconds from there and play
        // from there (as a plug-in's own transport controls would): the host's transport requests.
        for (const auto& name : names)
            if (name.startsWith("play from "))
                if (auto* controller = getDocumentController()->getHostPlaybackController())
                {
                    const auto at = name.fromFirstOccurrenceOf("play from ", false, false).getDoubleValue();
                    controller->requestSetCycleRange(at, 2.0);
                    controller->requestEnableCycle(true);
                    controller->requestSetPlaybackPosition(at);
                    controller->requestStartPlayback();
                }
    }

    StringArray selected;
    Telemetry* telemetry = nullptr;
    std::function<void()> onChange;
};

class TestDocumentController final : public ARADocumentControllerSpecialisation
{
public:
    using ARADocumentControllerSpecialisation::ARADocumentControllerSpecialisation;

    /** Reads a source through the host and keeps its peak (if not known). */
    void analyse(TestAudioSource& source)
    {
        if (source.peak.load() > 0.0f || !source.isSampleAccessEnabled())
            return;
        ARAAudioSourceReader reader(&source);
        AudioBuffer<float> audio(static_cast<int>(source.getChannelCount()), 65536);
        float peak = 0.0f;
        for (int64 at = 0; at < source.getSampleCount(); at += audio.getNumSamples())
        {
            const auto count = static_cast<int>(std::min<int64>(audio.getNumSamples(), source.getSampleCount() - at));
            if (!reader.read(&audio, 0, count, at, true, true))
                return;
            for (int c = 0; c < audio.getNumChannels(); ++c)
                peak = std::max(peak, audio.getMagnitude(c, 0, count));
        }
        source.peak.store(peak > 0.0f ? peak : 1.0f);
        Telemetry::bump(telemetry != nullptr ? telemetry->analysed : nullptr);
    }

    Telemetry* telemetry = nullptr; ///< (the processor bound to this document; tests have one)

protected:
    ARAAudioSource* doCreateAudioSource(ARADocument* document, ARA::ARAAudioSourceHostRef hostRef) noexcept override
    {
        return new TestAudioSource(document, hostRef);
    }

    ARAPlaybackRenderer* doCreatePlaybackRenderer() noexcept override { return new TestPlaybackRenderer(getDocumentController(), *this); }
    ARAEditorView* doCreateEditorView() noexcept override { return new TestEditorView(getDocumentController()); }

    bool doRestoreObjectsFromStream(ARAInputStream& input, const ARARestoreObjectsFilter* filter) noexcept override
    {
        const auto count = input.readInt();
        int restored = 0;
        for (int i = 0; i < count && !input.failed(); ++i)
        {
            const auto id = input.readString();
            const auto peak = input.readFloat();
            if (auto* source = filter->getAudioSourceToRestoreStateWithID<TestAudioSource>(id.toRawUTF8()))
            {
                source->peak.store(peak);
                ++restored;
            }
        }
        Telemetry::bump(telemetry != nullptr ? telemetry->restored : nullptr, restored);
        return !input.failed();
    }

    bool doStoreObjectsToStream(ARAOutputStream& output, const ARAStoreObjectsFilter* filter) noexcept override
    {
        const auto& sources = filter->getAudioSourcesToStore<TestAudioSource>();
        int count = 0;
        for (auto* source : sources)
            count += source->peak.load() > 0.0f ? 1 : 0;
        if (!output.writeInt(count))
            return false;
        for (auto* source : sources)
            if (source->peak.load() > 0.0f)
                if (!output.writeString(String(source->getPersistentID())) || !output.writeFloat(source->peak.load()))
                    return false;
        return true;
    }
};

void TestPlaybackRenderer::prepareToPlay(double sampleRate, int maximumSamplesPerBlock, int numChannels, AudioProcessor::ProcessingPrecision,
                                         AlwaysNonRealtime)
{
    sampleRate_ = sampleRate;
    scratch_.setSize(std::max(2, numChannels), maximumSamplesPerBlock);
    readers_.clear();
    for (auto* region : getPlaybackRegions())
    {
        auto* source = region->getAudioModification()->getAudioSource<TestAudioSource>();
        owner_.analyse(*source); // (the message thread: prepareToPlay is called there)
        if (readers_.find(source) == readers_.end())
            readers_.emplace(source, std::make_unique<ARAAudioSourceReader>(source));
    }
}

//==================================================================================================

class TestProcessor;

class TestEditor final : public AudioProcessorEditor, private AudioProcessorEditorARAExtension
{
public:
    explicit TestEditor(AudioProcessor& processor);
    ~TestEditor() override;

    void paint(Graphics& g) override
    {
        g.fillAll(Colour(0xff20262e));
        g.setColour(Colours::white);
        g.setFont(FontOptions(16.0f, Font::bold));
        g.drawText("DojoTestAra", 12, 8, getWidth() - 24, 22, Justification::left);
        g.setFont(FontOptions(13.0f));
        auto* view = getARAEditorView<TestEditorView>();
        if (view == nullptr)
        {
            g.drawText("Not bound to an ARA document", 12, 36, getWidth() - 24, 20, Justification::left);
            return;
        }
        g.drawText(String(view->selected.size()) + " selected region(s)", 12, 36, getWidth() - 24, 20, Justification::left);
        int y = 60;
        for (const auto& name : view->selected)
        {
            g.drawText(name, 24, y, getWidth() - 36, 18, Justification::left);
            y += 20;
        }
    }

    AudioProcessorEditorARAExtension* getARAClientExtensions() override { return this; }
};

class TestProcessor final : public AudioProcessor, private AudioProcessorARAExtension
{
public:
    TestProcessor()
        : AudioProcessor(BusesProperties()
                             .withInput("Input", AudioChannelSet::stereo(), true)
                             .withOutput("Output", AudioChannelSet::stereo(), true))
    {
        const auto count = [](const char* id, const char* name)
        {
            return std::make_unique<AudioParameterFloat>(ParameterID{id, 1}, name, NormalisableRange<float>(0.0f, 1.0f), 0.0f,
                                                         AudioParameterFloatAttributes().withAutomatable(false));
        };
        auto selected = count("selected", "Selected regions");
        auto analysed = count("analysed", "Analysed sources");
        auto restored = count("restored", "Restored peaks");
        telemetry.selected = selected.get();
        telemetry.analysed = analysed.get();
        telemetry.restored = restored.get();
        addParameter(selected.release());
        addParameter(analysed.release());
        addParameter(restored.release());
    }

    const String getName() const override { return "DojoTestAra"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const String&) override {}
    void getStateInformation(MemoryBlock&) override {} // (the edits live in the ARA document)
    void setStateInformation(const void*, int) override {}
    bool hasEditor() const override { return true; }
    AudioProcessorEditor* createEditor() override { return new TestEditor(*this); }

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        return layouts.getMainOutputChannelSet() == AudioChannelSet::stereo() || layouts.getMainOutputChannelSet() == AudioChannelSet::mono();
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        prepareToPlayForARA(sampleRate, samplesPerBlock, getMainBusNumOutputChannels(), getProcessingPrecision());
    }

    void didBindToARA() noexcept override
    {
        AudioProcessorARAExtension::didBindToARA();
        if (auto* dc = getDocumentControllerSpecialisation())
            dc->telemetry = &telemetry;
        if (auto* view = getEditorView<TestEditorView>())
        {
            view->telemetry = &telemetry;
            // (a plug-in acts on the host's selection while its editor is open; this one counts it
            //  from the start, so that headless tests can see that it arrived)
            view->setEditorOpen(true);
        }
    }

    void releaseResources() override { releaseResourcesForARA(); }

    void processBlock(AudioBuffer<float>& buffer, MidiBuffer& midi) override
    {
        ScopedNoDenormals noDenormals;
        if (!processBlockForARA(buffer, isRealtime(), getPlayHead()))
            processBlockBypassed(buffer, midi);
    }

    using AudioProcessor::processBlock;

    AudioProcessorARAExtension* getARAClientExtensions() override { return this; }

    TestDocumentController* getDocumentControllerSpecialisation()
    {
        if (!isBoundToARA())
            return nullptr;
        auto* dc = getPlaybackRenderer() != nullptr ? getPlaybackRenderer()->getDocumentController()
                   : getEditorView() != nullptr  ? getEditorView()->getDocumentController()
                                                 : nullptr;
        return dc != nullptr ? ARADocumentControllerSpecialisation::getSpecialisedDocumentController<TestDocumentController>(dc) : nullptr;
    }

    Telemetry telemetry;
};

TestEditor::TestEditor(AudioProcessor& processor)
    : AudioProcessorEditor(&processor)
    , AudioProcessorEditorARAExtension(&processor)
{
    if (auto* view = getARAEditorView<TestEditorView>())
    {
        view->telemetry = &static_cast<TestProcessor&>(processor).telemetry;
        view->onChange = [this] { repaint(); };
    }
    setResizable(true, false);
    setSize(420, 220);
}

TestEditor::~TestEditor()
{
    if (auto* view = getARAEditorView<TestEditorView>())
        view->onChange = nullptr;
}
} // namespace

const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory()
{
    return juce::ARADocumentControllerSpecialisation::createARAFactory<TestDocumentController>();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TestProcessor();
}
