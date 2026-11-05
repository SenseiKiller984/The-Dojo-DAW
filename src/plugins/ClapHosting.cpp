#include "plugins/ClapHosting.h"

#include <clap/clap.h>

#include <juce_gui_extra/juce_gui_extra.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#if JUCE_LINUX || JUCE_BSD
#include <poll.h>
#endif

namespace dojo::plugins
{
namespace
{
thread_local bool tlsIsAudioThread = false;

bool isMessageThread()
{
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    return mm != nullptr && mm->isThisTheMessageThread();
}

//==================================================================================================
/** A loaded .clap binary: its entry point and plug-in factory. Shared by all instances from it. */
class ClapModule final
{
public:
    static std::shared_ptr<ClapModule> load(const juce::File& file, juce::String& error)
    {
        static std::mutex mutex;
        static std::map<juce::String, std::weak_ptr<ClapModule>> cache;

        const std::lock_guard lock(mutex);
        const auto key = file.getFullPathName();
        if (auto existing = cache[key].lock())
            return existing;

        auto module = std::shared_ptr<ClapModule>(new ClapModule());
        if (!module->library_.open(key))
        {
            error = "Could not load " + file.getFileName();
            return nullptr;
        }

        module->entry_ = static_cast<const clap_plugin_entry_t*>(module->library_.getFunction("clap_entry"));
        if (module->entry_ == nullptr || !clap_version_is_compatible(module->entry_->clap_version))
        {
            error = file.getFileName() + " is not a compatible CLAP plug-in.";
            module->entry_ = nullptr;
            return nullptr;
        }

        if (!module->entry_->init(key.toRawUTF8()))
        {
            error = file.getFileName() + " failed to initialise.";
            module->entry_ = nullptr;
            return nullptr;
        }
        module->initialised_ = true;

        module->factory_ = static_cast<const clap_plugin_factory_t*>(module->entry_->get_factory(CLAP_PLUGIN_FACTORY_ID));
        if (module->factory_ == nullptr)
        {
            error = file.getFileName() + " has no plug-in factory.";
            return nullptr;
        }

        module->file_ = file;
        cache[key] = module;
        return module;
    }

    ~ClapModule()
    {
        if (initialised_ && entry_ != nullptr)
            entry_->deinit();
        library_.close();
    }

    [[nodiscard]] const clap_plugin_factory_t* getFactory() const noexcept { return factory_; }
    [[nodiscard]] const juce::File& getFile() const noexcept { return file_; }

    [[nodiscard]] const clap_plugin_descriptor_t* findDescriptor(int uniqueId) const
    {
        const auto count = factory_->get_plugin_count(factory_);
        for (std::uint32_t i = 0; i < count; ++i)
            if (const auto* d = factory_->get_plugin_descriptor(factory_, i); d != nullptr && d->id != nullptr)
                if (ClapPluginFormat::hashPluginId(d->id) == uniqueId)
                    return d;
        return nullptr;
    }

private:
    ClapModule() = default;

    juce::DynamicLibrary library_;
    const clap_plugin_entry_t* entry_ = nullptr;
    const clap_plugin_factory_t* factory_ = nullptr;
    juce::File file_;
    bool initialised_ = false;
};

bool hasFeature(const clap_plugin_descriptor_t& d, const char* feature)
{
    if (d.features == nullptr)
        return false;
    for (auto f = d.features; *f != nullptr; ++f)
        if (std::strcmp(*f, feature) == 0)
            return true;
    return false;
}

juce::String categoryOf(const clap_plugin_descriptor_t& d)
{
    if (hasFeature(d, CLAP_PLUGIN_FEATURE_INSTRUMENT))
        return "Instrument";
    if (d.features != nullptr)
        for (auto f = d.features; *f != nullptr; ++f)
            if (std::strcmp(*f, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT) != 0)
                return juce::String(*f).substring(0, 1).toUpperCase() + juce::String(*f).substring(1);
    return "Effect";
}

void describe(const ClapModule& module, const clap_plugin_descriptor_t& d, juce::PluginDescription& out)
{
    out.name = juce::String::fromUTF8(d.name != nullptr ? d.name : d.id);
    out.descriptiveName = juce::String::fromUTF8(d.description != nullptr ? d.description : "");
    out.pluginFormatName = ClapPluginFormat::kFormatName;
    out.category = categoryOf(d);
    out.manufacturerName = juce::String::fromUTF8(d.vendor != nullptr ? d.vendor : "");
    out.version = juce::String::fromUTF8(d.version != nullptr ? d.version : "");
    out.fileOrIdentifier = module.getFile().getFullPathName();
    out.lastFileModTime = module.getFile().getLastModificationTime();
    out.lastInfoUpdateTime = juce::Time::getCurrentTime();
    out.uniqueId = ClapPluginFormat::hashPluginId(d.id);
    out.deprecatedUid = out.uniqueId;
    out.isInstrument = hasFeature(d, CLAP_PLUGIN_FEATURE_INSTRUMENT);
    out.numInputChannels = out.isInstrument ? 0 : 2;
    out.numOutputChannels = 2;
    out.hasSharedContainer = module.getFactory()->get_plugin_count(module.getFactory()) > 1;
}

//==================================================================================================
// Event lists (pre-allocated: built and read on the audio thread)

class InputEventList
{
public:
    explicit InputEventList(std::size_t capacityBytes = 64 * 1024, std::size_t maxEvents = 4096)
    {
        storage_.resize(capacityBytes);
        offsets_.reserve(maxEvents);
        list_.ctx = this;
        list_.size = [](const clap_input_events_t* l) -> std::uint32_t
        { return static_cast<std::uint32_t>(static_cast<const InputEventList*>(l->ctx)->offsets_.size()); };
        list_.get = [](const clap_input_events_t* l, std::uint32_t index) -> const clap_event_header_t*
        {
            const auto* self = static_cast<const InputEventList*>(l->ctx);
            return index < self->offsets_.size()
                       ? reinterpret_cast<const clap_event_header_t*>(self->storage_.data() + self->offsets_[index])
                       : nullptr;
        };
    }

    void clear() noexcept
    {
        offsets_.clear();
        used_ = 0;
    }

    bool push(const clap_event_header_t& event) noexcept
    {
        const std::size_t size = (event.size + 7u) & ~std::size_t{7};
        if (used_ + size > storage_.size() || offsets_.size() == offsets_.capacity())
            return false;
        std::memcpy(storage_.data() + used_, &event, event.size);
        offsets_.push_back(used_);
        used_ += size;
        return true;
    }

    [[nodiscard]] const clap_input_events_t* get() const noexcept { return &list_; }

private:
    std::vector<std::uint8_t> storage_; // new[] storage is aligned for every event struct
    std::vector<std::size_t> offsets_;
    std::size_t used_ = 0;
    clap_input_events_t list_{};
};

class ClapPluginInstance;

//==================================================================================================
/** One CLAP parameter, exposed to JUCE (normalised 0..1) and queued to the plug-in when changed. */
class ClapParameter final : public juce::AudioPluginInstance::HostedParameter
{
public:
    ClapParameter(ClapPluginInstance& owner, const clap_param_info_t& info, double value)
        : owner_(owner)
        , info_(info)
        , plain_(value)
    {
    }

    float getValue() const override { return static_cast<float>(toNormalised(plain_.load(std::memory_order_relaxed))); }
    void setValue(float newValue) override;
    float getDefaultValue() const override { return static_cast<float>(toNormalised(info_.default_value)); }
    juce::String getName(int maximumLength) const override
    {
        return juce::String::fromUTF8(info_.name).substring(0, maximumLength);
    }
    juce::String getLabel() const override { return {}; }
    juce::String getText(float normalisedValue, int maximumLength) const override;
    float getValueForText(const juce::String& text) const override;
    bool isDiscrete() const override { return (info_.flags & CLAP_PARAM_IS_STEPPED) != 0; }
    int getNumSteps() const override
    {
        return isDiscrete() ? std::max(2, static_cast<int>(info_.max_value - info_.min_value) + 1)
                            : juce::AudioProcessorParameter::getNumSteps();
    }
    bool isAutomatable() const override { return (info_.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0; }
    bool isBoolean() const override { return isDiscrete() && std::abs(info_.max_value - info_.min_value - 1.0) < 1.0e-9; }
    juce::String getParameterID() const override { return juce::String(static_cast<juce::int64>(info_.id)); }

    [[nodiscard]] const clap_param_info_t& getInfo() const noexcept { return info_; }

    /** Audio thread: if the host changed the value, writes a PARAM_VALUE event. */
    void appendPendingEvent(InputEventList& events) noexcept
    {
        if (!pending_.exchange(false, std::memory_order_acq_rel))
            return;
        clap_event_param_value_t event{};
        event.header.size = sizeof(event);
        event.header.time = 0;
        event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        event.header.type = CLAP_EVENT_PARAM_VALUE;
        event.header.flags = 0;
        event.param_id = info_.id;
        event.cookie = info_.cookie;
        event.note_id = -1;
        event.port_index = -1;
        event.channel = -1;
        event.key = -1;
        event.value = plain_.load(std::memory_order_relaxed);
        if (!events.push(event.header))
            pending_.store(true, std::memory_order_release); // try again next block
    }

    /** Any thread: the plug-in reported a new value (UI is notified later, on the message thread). */
    void valueFromPlugin(double plain) noexcept
    {
        plain_.store(plain, std::memory_order_relaxed);
        notify_.store(true, std::memory_order_release);
    }

    /** Message thread: tells JUCE listeners about values the plug-in changed. */
    void deliverNotification()
    {
        if (notify_.exchange(false, std::memory_order_acq_rel))
            sendValueChangedMessageToListeners(getValue());
    }

    [[nodiscard]] bool hasPendingEvent() const noexcept { return pending_.load(std::memory_order_acquire); }

private:
    [[nodiscard]] double range() const noexcept { return std::max(1.0e-12, info_.max_value - info_.min_value); }
    [[nodiscard]] double toNormalised(double plain) const noexcept
    {
        return std::clamp((plain - info_.min_value) / range(), 0.0, 1.0);
    }
    [[nodiscard]] double toPlain(double normalised) const noexcept
    {
        double plain = info_.min_value + std::clamp(normalised, 0.0, 1.0) * range();
        if (isDiscrete())
            plain = std::round(plain);
        return plain;
    }

    ClapPluginInstance& owner_;
    clap_param_info_t info_;
    std::atomic<double> plain_;
    std::atomic<bool> pending_{false};
    std::atomic<bool> notify_{false};
};

//==================================================================================================
/** The clap_host given to one plug-in. Created before the instance (the plug-in is created and
    initialised first, to learn its ports), then adopted by it. */
struct HostContext
{
    clap_host_t host{};
    ClapPluginInstance* owner = nullptr;
    std::atomic<bool> restartRequested{false};
    std::atomic<bool> callbackRequested{false};
    std::atomic<bool> flushRequested{false};
    std::atomic<bool> dirty{false};
    std::atomic<bool> latencyChanged{false};
};

//==================================================================================================
class ClapEditor;

class ClapPluginInstance final : public juce::AudioPluginInstance, private juce::Timer
{
public:
    static std::unique_ptr<ClapPluginInstance> create(std::shared_ptr<ClapModule> module, const clap_plugin_descriptor_t& descriptor,
                                                      juce::String& error);
    ~ClapPluginInstance() override;

    // juce::AudioPluginInstance
    const juce::String getName() const override { return name_; }
    void fillInPluginDescription(juce::PluginDescription& description) const override { describe(*module_, *descriptor_, description); }
    void prepareToPlay(double sampleRate, int maximumBlockSize) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    using juce::AudioPluginInstance::processBlock;
    bool isBusesLayoutSupported(const BusesLayout& layout) const override { return layout == getBusesLayout(); }
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return noteInputs_ > 0; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& destination) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Used by the parameter and editor classes.
    void parameterChangedByHost() noexcept { paramsPending_.store(true, std::memory_order_release); }
    [[nodiscard]] const clap_plugin_t* getPlugin() const noexcept { return plugin_; }
    [[nodiscard]] const clap_plugin_params_t* getParamsExtension() const noexcept { return params_; }
    [[nodiscard]] const clap_plugin_gui_t* getGuiExtension() const noexcept { return gui_; }
    void setEditor(ClapEditor* editor) noexcept { editor_ = editor; }
    [[nodiscard]] ClapEditor* getEditor() const noexcept { return editor_; }

    // Host callbacks (dispatched from the static C functions).
    bool registerTimer(std::uint32_t periodMs, clap_id* id);
    bool unregisterTimer(clap_id id);
    bool registerFd(int fd, clap_posix_fd_flags_t flags);
    bool unregisterFd(int fd);
    bool requestResize(std::uint32_t width, std::uint32_t height);

private:
    ClapPluginInstance(std::shared_ptr<ClapModule> module, std::unique_ptr<HostContext> context, const clap_plugin_t* plugin,
                       const clap_plugin_descriptor_t& descriptor, BusesProperties buses, int noteInputs, bool clapNotes);

    void timerCallback() override;
    void activate(double sampleRate, int maximumBlockSize);
    void deactivate();
    void updateLatency();
    void flushParametersOffline();
    void handleOutputEvent(const clap_event_header_t& event) noexcept;

    /** Holds the audio thread off (lock-free hand-over) for main-thread work on an active plug-in. */
    class ScopedSuspend
    {
    public:
        explicit ScopedSuspend(ClapPluginInstance& owner)
            : owner_(owner)
        {
            owner_.suspended_.store(true);
            while (owner_.inProcess_.load())
                std::this_thread::yield();
        }
        ~ScopedSuspend() { owner_.suspended_.store(false); }

    private:
        ClapPluginInstance& owner_;
    };

    struct TimerEntry
    {
        std::unique_ptr<juce::Timer> timer;
    };

    std::shared_ptr<ClapModule> module_;
    std::unique_ptr<HostContext> context_;
    const clap_plugin_t* plugin_ = nullptr;
    const clap_plugin_descriptor_t* descriptor_ = nullptr;
    const clap_plugin_params_t* params_ = nullptr;
    const clap_plugin_state_t* state_ = nullptr;
    const clap_plugin_latency_t* latency_ = nullptr;
    const clap_plugin_gui_t* gui_ = nullptr;
    const clap_plugin_timer_support_t* timerSupport_ = nullptr;
    const clap_plugin_posix_fd_support_t* fdSupport_ = nullptr;
    juce::String name_;
    int noteInputs_ = 0;
    bool clapNotes_ = false;

    std::vector<ClapParameter*> parameters_;
    std::map<clap_id, ClapParameter*> parameterById_;

    // Activation / processing
    bool active_ = false;
    std::atomic<bool> processing_{false};
    std::atomic<bool> suspended_{false};
    std::atomic<bool> inProcess_{false};
    std::atomic<bool> paramsPending_{false};
    double sampleRate_ = 48000.0;
    int maxBlock_ = 512;
    std::int64_t steadyTime_ = 0;

    // Audio-thread buffers (allocated in prepareToPlay)
    InputEventList inEvents_;
    clap_output_events_t outEvents_{};
    juce::AudioBuffer<float> inputScratch_;
    std::vector<std::vector<float*>> inputPointers_;
    std::vector<std::vector<float*>> outputPointers_;
    std::vector<clap_audio_buffer_t> inputBuffers_;
    std::vector<clap_audio_buffer_t> outputBuffers_;
    clap_event_transport_t transport_{};

    // Main-thread services
    std::map<clap_id, TimerEntry> timers_;
    clap_id nextTimerId_ = 1;
    std::map<int, clap_posix_fd_flags_t> fds_;
    ClapEditor* editor_ = nullptr;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ClapPluginInstance)
};

//==================================================================================================
// Host extension tables (static; `host->host_data` leads back to the HostContext)

HostContext& contextOf(const clap_host_t* host) { return *static_cast<HostContext*>(host->host_data); }

const clap_host_log_t hostLog{[](const clap_host_t*, clap_log_severity severity, const char* message)
                              {
                                  if (severity >= CLAP_LOG_WARNING)
                                      juce::Logger::writeToLog("CLAP: " + juce::String::fromUTF8(message != nullptr ? message : ""));
                              }};

const clap_host_thread_check_t hostThreadCheck{[](const clap_host_t*) { return isMessageThread(); },
                                               [](const clap_host_t*) { return tlsIsAudioThread; }};

const clap_host_params_t hostParams{
    [](const clap_host_t*, clap_param_rescan_flags) {}, // values are re-read on demand; the list is fixed
    [](const clap_host_t*, clap_id, clap_param_clear_flags) {},
    [](const clap_host_t* host) { contextOf(host).flushRequested.store(true); }};

const clap_host_state_t hostState{[](const clap_host_t* host) { contextOf(host).dirty.store(true); }};

const clap_host_latency_t hostLatency{[](const clap_host_t* host) { contextOf(host).latencyChanged.store(true); }};

const clap_host_audio_ports_t hostAudioPorts{[](const clap_host_t*, std::uint32_t) { return false; },
                                             [](const clap_host_t*, std::uint32_t) {}};

const clap_host_note_ports_t hostNotePorts{[](const clap_host_t*) -> std::uint32_t
                                           { return CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI; },
                                           [](const clap_host_t*, std::uint32_t) {}};

const clap_host_timer_support_t hostTimers{
    [](const clap_host_t* host, std::uint32_t period, clap_id* id)
    {
        auto* owner = contextOf(host).owner;
        return owner != nullptr && owner->registerTimer(period, id);
    },
    [](const clap_host_t* host, clap_id id)
    {
        auto* owner = contextOf(host).owner;
        return owner != nullptr && owner->unregisterTimer(id);
    }};

const clap_host_posix_fd_support_t hostFds{
    [](const clap_host_t* host, int fd, clap_posix_fd_flags_t flags)
    {
        auto* owner = contextOf(host).owner;
        return owner != nullptr && owner->registerFd(fd, flags);
    },
    [](const clap_host_t* host, int fd, clap_posix_fd_flags_t flags)
    {
        auto* owner = contextOf(host).owner;
        return owner != nullptr && owner->registerFd(fd, flags);
    },
    [](const clap_host_t* host, int fd)
    {
        auto* owner = contextOf(host).owner;
        return owner != nullptr && owner->unregisterFd(fd);
    }};

const clap_host_gui_t hostGui{
    [](const clap_host_t*) {},
    [](const clap_host_t* host, std::uint32_t width, std::uint32_t height)
    {
        auto* owner = contextOf(host).owner;
        return owner != nullptr && owner->requestResize(width, height);
    },
    [](const clap_host_t*) { return false; },
    [](const clap_host_t*) { return false; },
    [](const clap_host_t*, bool) {}};

const void* getHostExtension(const clap_host_t*, const char* id)
{
    if (std::strcmp(id, CLAP_EXT_LOG) == 0)
        return &hostLog;
    if (std::strcmp(id, CLAP_EXT_THREAD_CHECK) == 0)
        return &hostThreadCheck;
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
        return &hostParams;
    if (std::strcmp(id, CLAP_EXT_STATE) == 0)
        return &hostState;
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0)
        return &hostLatency;
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
        return &hostAudioPorts;
    if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0)
        return &hostNotePorts;
    if (std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0)
        return &hostTimers;
    if (std::strcmp(id, CLAP_EXT_GUI) == 0)
        return &hostGui;
#if JUCE_LINUX || JUCE_BSD
    if (std::strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT) == 0)
        return &hostFds;
#endif
    return nullptr;
}

//==================================================================================================
/** Embeds the plug-in's own window (X11 on Linux, Win32 on Windows). */
class ClapEditor final : public juce::AudioProcessorEditor, private juce::ComponentListener
{
public:
    ClapEditor(ClapPluginInstance& owner, const char* api)
        : juce::AudioProcessorEditor(owner)
        , owner_(owner)
        , api_(api)
    {
        const auto* gui = owner_.getGuiExtension();
        gui->create(owner_.getPlugin(), api_, false);
        created_ = true;

        std::uint32_t width = 400;
        std::uint32_t height = 300;
        gui->get_size(owner_.getPlugin(), &width, &height);
        resizable_ = gui->can_resize != nullptr && gui->can_resize(owner_.getPlugin());

#if JUCE_LINUX || JUCE_BSD
        embed_ = std::make_unique<juce::XEmbedComponent>(true, false);
        addAndMakeVisible(*embed_);
#endif
        setResizable(resizable_, false);
        setSize(static_cast<int>(std::max<std::uint32_t>(width, 50)), static_cast<int>(std::max<std::uint32_t>(height, 50)));
        owner_.setEditor(this);
    }

    ~ClapEditor() override
    {
        owner_.setEditor(nullptr);
        if (created_)
            owner_.getGuiExtension()->destroy(owner_.getPlugin());
    }

    void paint(juce::Graphics& g) override { g.fillAll(juce::Colours::black); }

    void resized() override
    {
#if JUCE_LINUX || JUCE_BSD
        if (embed_ != nullptr)
            embed_->setBounds(getLocalBounds());
#endif
        if (attached_ && resizable_ && !resizingFromPlugin_)
        {
            auto width = static_cast<std::uint32_t>(getWidth());
            auto height = static_cast<std::uint32_t>(getHeight());
            const auto* gui = owner_.getGuiExtension();
            if (gui->adjust_size != nullptr)
                gui->adjust_size(owner_.getPlugin(), &width, &height);
            gui->set_size(owner_.getPlugin(), width, height);
        }
    }

    void parentHierarchyChanged() override { attachIfPossible(); }
    void visibilityChanged() override { attachIfPossible(); }

    void resizeFromPlugin(std::uint32_t width, std::uint32_t height)
    {
        const juce::ScopedValueSetter<bool> guard(resizingFromPlugin_, true);
        setSize(static_cast<int>(width), static_cast<int>(height));
    }

private:
    void attachIfPossible()
    {
        if (attached_ || !isShowing() || getPeer() == nullptr)
            return;

        clap_window_t window{};
        window.api = api_;
#if JUCE_LINUX || JUCE_BSD
        window.x11 = static_cast<clap_xwnd>(embed_->getHostWindowID());
#elif JUCE_WINDOWS
        window.win32 = getPeer()->getNativeHandle();
#endif
        const auto* gui = owner_.getGuiExtension();
        if (gui->set_parent(owner_.getPlugin(), &window))
        {
            attached_ = true;
            gui->show(owner_.getPlugin());
        }
    }

    ClapPluginInstance& owner_;
    const char* api_;
    bool created_ = false;
    bool attached_ = false;
    bool resizable_ = false;
    bool resizingFromPlugin_ = false;
#if JUCE_LINUX || JUCE_BSD
    std::unique_ptr<juce::XEmbedComponent> embed_;
#endif
};

const char* nativeWindowApi()
{
#if JUCE_WINDOWS
    return CLAP_WINDOW_API_WIN32;
#elif JUCE_LINUX || JUCE_BSD
    return CLAP_WINDOW_API_X11;
#else
    return nullptr;
#endif
}

//==================================================================================================
// ClapParameter

void ClapParameter::setValue(float newValue)
{
    plain_.store(toPlain(newValue), std::memory_order_relaxed);
    pending_.store(true, std::memory_order_release);
    owner_.parameterChangedByHost();
}

juce::String ClapParameter::getText(float normalisedValue, int maximumLength) const
{
    const double plain = toPlain(normalisedValue);
    if (const auto* params = owner_.getParamsExtension(); params != nullptr && params->value_to_text != nullptr && isMessageThread())
    {
        char buffer[256] = {};
        if (params->value_to_text(owner_.getPlugin(), info_.id, plain, buffer, sizeof(buffer)))
            return juce::String::fromUTF8(buffer).substring(0, maximumLength);
    }
    return juce::String(plain, 2).substring(0, maximumLength);
}

float ClapParameter::getValueForText(const juce::String& text) const
{
    if (const auto* params = owner_.getParamsExtension(); params != nullptr && params->text_to_value != nullptr)
    {
        double plain = 0.0;
        if (params->text_to_value(owner_.getPlugin(), info_.id, text.toRawUTF8(), &plain))
            return static_cast<float>(toNormalised(plain));
    }
    return static_cast<float>(toNormalised(text.getDoubleValue()));
}

//==================================================================================================
// ClapPluginInstance

std::unique_ptr<ClapPluginInstance> ClapPluginInstance::create(std::shared_ptr<ClapModule> module,
                                                               const clap_plugin_descriptor_t& descriptor, juce::String& error)
{
    auto context = std::make_unique<HostContext>();
    context->host.clap_version = CLAP_VERSION;
    context->host.host_data = context.get();
    context->host.name = "The Dojo";
    context->host.vendor = "The Dojo Project";
    context->host.url = "https://github.com/SenseiKiller984/The-Dojo-DAW";
    context->host.version = "1.0";
    context->host.get_extension = getHostExtension;
    context->host.request_restart = [](const clap_host_t* host) { contextOf(host).restartRequested.store(true); };
    context->host.request_process = [](const clap_host_t*) {};
    context->host.request_callback = [](const clap_host_t* host) { contextOf(host).callbackRequested.store(true); };

    const auto* factory = module->getFactory();
    const auto* plugin = factory->create_plugin(factory, &context->host, descriptor.id);
    if (plugin == nullptr)
    {
        error = "The plug-in could not be created.";
        return nullptr;
    }

    if (!plugin->init(plugin))
    {
        plugin->destroy(plugin);
        error = "The plug-in failed to initialise.";
        return nullptr;
    }

    // Audio ports -> JUCE buses: the first port of each direction is the main bus.
    BusesProperties buses;
    const auto* ports = static_cast<const clap_plugin_audio_ports_t*>(plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
    if (ports != nullptr)
    {
        for (const bool isInput : {true, false})
        {
            const auto count = ports->count(plugin, isInput);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                clap_audio_port_info_t info{};
                if (!ports->get(plugin, i, isInput, &info))
                    continue;
                const auto channels = info.channel_count == 1 ? juce::AudioChannelSet::mono()
                                      : info.channel_count == 2 ? juce::AudioChannelSet::stereo()
                                                                : juce::AudioChannelSet::discreteChannels(static_cast<int>(info.channel_count));
                const auto name = juce::String::fromUTF8(info.name).isNotEmpty() ? juce::String::fromUTF8(info.name)
                                                                                 : juce::String(isInput ? "Input " : "Output ") + juce::String(i + 1);
                buses.addBus(isInput, name, channels, true);
            }
        }
    }

    int noteInputs = 0;
    bool clapNotes = false;
    if (const auto* notePorts = static_cast<const clap_plugin_note_ports_t*>(plugin->get_extension(plugin, CLAP_EXT_NOTE_PORTS)))
    {
        noteInputs = static_cast<int>(notePorts->count(plugin, true));
        if (noteInputs > 0)
        {
            clap_note_port_info_t info{};
            if (notePorts->get(plugin, 0, true, &info))
                clapNotes = (info.supported_dialects & CLAP_NOTE_DIALECT_CLAP) != 0 &&
                            (info.preferred_dialect == CLAP_NOTE_DIALECT_CLAP || (info.supported_dialects & CLAP_NOTE_DIALECT_MIDI) == 0);
        }
    }

    return std::unique_ptr<ClapPluginInstance>(
        new ClapPluginInstance(std::move(module), std::move(context), plugin, descriptor, buses, noteInputs, clapNotes));
}

ClapPluginInstance::ClapPluginInstance(std::shared_ptr<ClapModule> module, std::unique_ptr<HostContext> context,
                                       const clap_plugin_t* plugin, const clap_plugin_descriptor_t& descriptor, BusesProperties buses,
                                       int noteInputs, bool clapNotes)
    : juce::AudioPluginInstance(buses)
    , module_(std::move(module))
    , context_(std::move(context))
    , plugin_(plugin)
    , descriptor_(&descriptor)
    , name_(juce::String::fromUTF8(descriptor.name != nullptr ? descriptor.name : descriptor.id))
    , noteInputs_(noteInputs)
    , clapNotes_(clapNotes)
{
    context_->owner = this;

    params_ = static_cast<const clap_plugin_params_t*>(plugin_->get_extension(plugin_, CLAP_EXT_PARAMS));
    state_ = static_cast<const clap_plugin_state_t*>(plugin_->get_extension(plugin_, CLAP_EXT_STATE));
    latency_ = static_cast<const clap_plugin_latency_t*>(plugin_->get_extension(plugin_, CLAP_EXT_LATENCY));
    gui_ = static_cast<const clap_plugin_gui_t*>(plugin_->get_extension(plugin_, CLAP_EXT_GUI));
    timerSupport_ = static_cast<const clap_plugin_timer_support_t*>(plugin_->get_extension(plugin_, CLAP_EXT_TIMER_SUPPORT));
    fdSupport_ = static_cast<const clap_plugin_posix_fd_support_t*>(plugin_->get_extension(plugin_, CLAP_EXT_POSIX_FD_SUPPORT));

    if (params_ != nullptr)
    {
        const auto count = params_->count(plugin_);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            clap_param_info_t info{};
            if (!params_->get_info(plugin_, i, &info) || (info.flags & CLAP_PARAM_IS_HIDDEN) != 0)
                continue;
            double value = info.default_value;
            params_->get_value(plugin_, info.id, &value);
            auto parameter = std::make_unique<ClapParameter>(*this, info, value);
            parameters_.push_back(parameter.get());
            parameterById_[info.id] = parameter.get();
            addHostedParameter(std::move(parameter));
        }
    }

    outEvents_.ctx = this;
    outEvents_.try_push = [](const clap_output_events_t* list, const clap_event_header_t* event)
    {
        static_cast<ClapPluginInstance*>(list->ctx)->handleOutputEvent(*event);
        return true;
    };

    updateLatency();
    startTimerHz(30);
}

ClapPluginInstance::~ClapPluginInstance()
{
    stopTimer();
    jassert(editor_ == nullptr); // editors must be deleted before their processor
    timers_.clear();
    deactivate();
    plugin_->destroy(plugin_);
}

void ClapPluginInstance::updateLatency()
{
    if (latency_ != nullptr && latency_->get != nullptr)
        setLatencySamples(static_cast<int>(latency_->get(plugin_)));
}

void ClapPluginInstance::activate(double sampleRate, int maximumBlockSize)
{
    sampleRate_ = sampleRate;
    maxBlock_ = std::max(1, maximumBlockSize);
    if (!plugin_->activate(plugin_, sampleRate_, 1, static_cast<std::uint32_t>(maxBlock_)))
    {
        juce::Logger::writeToLog("CLAP: " + name_ + " refused to activate");
        return;
    }
    active_ = true;
    updateLatency();
}

void ClapPluginInstance::deactivate()
{
    if (!active_)
        return;
    if (processing_.exchange(false))
        plugin_->stop_processing(plugin_);
    plugin_->deactivate(plugin_);
    active_ = false;
}

void ClapPluginInstance::prepareToPlay(double sampleRate, int maximumBlockSize)
{
    const ScopedSuspend suspend(*this);
    deactivate();

    // Buffers for the audio thread.
    const int inputChannels = std::max(1, getTotalNumInputChannels());
    inputScratch_.setSize(inputChannels, std::max(1, maximumBlockSize), false, true, false);
    inputPointers_.assign(static_cast<std::size_t>(getBusCount(true)), {});
    outputPointers_.assign(static_cast<std::size_t>(getBusCount(false)), {});
    inputBuffers_.assign(inputPointers_.size(), clap_audio_buffer_t{});
    outputBuffers_.assign(outputPointers_.size(), clap_audio_buffer_t{});
    for (int b = 0; b < getBusCount(true); ++b)
        inputPointers_[static_cast<std::size_t>(b)].assign(static_cast<std::size_t>(std::max(1, getChannelCountOfBus(true, b))), nullptr);
    for (int b = 0; b < getBusCount(false); ++b)
        outputPointers_[static_cast<std::size_t>(b)].assign(static_cast<std::size_t>(std::max(1, getChannelCountOfBus(false, b))), nullptr);

    activate(sampleRate, maximumBlockSize);
}

void ClapPluginInstance::releaseResources()
{
    const ScopedSuspend suspend(*this);
    deactivate();
}

void ClapPluginInstance::handleOutputEvent(const clap_event_header_t& event) noexcept
{
    if (event.space_id != CLAP_CORE_EVENT_SPACE_ID || event.type != CLAP_EVENT_PARAM_VALUE)
        return;
    const auto& value = reinterpret_cast<const clap_event_param_value_t&>(event);
    if (const auto it = parameterById_.find(value.param_id); it != parameterById_.end())
        it->second->valueFromPlugin(value.value);
}

void ClapPluginInstance::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    const juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();

    inProcess_.store(true);
    if (suspended_.load() || !active_ || numSamples > maxBlock_)
    {
        inProcess_.store(false);
        for (int ch = getTotalNumInputChannels(); ch < buffer.getNumChannels(); ++ch)
            buffer.clear(ch, 0, numSamples);
        if (getTotalNumInputChannels() == 0)
            buffer.clear();
        midi.clear();
        return;
    }

    tlsIsAudioThread = true;

    if (!processing_.load(std::memory_order_relaxed))
        processing_.store(plugin_->start_processing(plugin_), std::memory_order_relaxed);

    // Events: parameter changes first (time 0), then MIDI in time order.
    inEvents_.clear();
    if (paramsPending_.exchange(false, std::memory_order_acq_rel))
        for (auto* parameter : parameters_)
            parameter->appendPendingEvent(inEvents_);

    for (const auto metadata : midi)
    {
        const auto* data = metadata.data;
        const int size = metadata.numBytes;
        const auto time = static_cast<std::uint32_t>(std::clamp(metadata.samplePosition, 0, numSamples - 1));
        if (size < 1 || size > 3)
            continue;

        const int status = data[0] & 0xf0;
        const bool noteOn = status == 0x90 && size == 3 && data[2] > 0;
        const bool noteOff = status == 0x80 || (status == 0x90 && size == 3 && data[2] == 0);

        if (clapNotes_ && (noteOn || noteOff))
        {
            clap_event_note_t note{};
            note.header.size = sizeof(note);
            note.header.time = time;
            note.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            note.header.type = static_cast<std::uint16_t>(noteOn ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF);
            note.note_id = -1;
            note.port_index = 0;
            note.channel = static_cast<std::int16_t>(data[0] & 0x0f);
            note.key = static_cast<std::int16_t>(data[1]);
            note.velocity = noteOn ? data[2] / 127.0 : 0.0;
            inEvents_.push(note.header);
        }
        else
        {
            clap_event_midi_t event{};
            event.header.size = sizeof(event);
            event.header.time = time;
            event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            event.header.type = CLAP_EVENT_MIDI;
            event.port_index = 0;
            for (int i = 0; i < size; ++i)
                event.data[i] = data[i];
            inEvents_.push(event.header);
        }
    }

    // Transport.
    transport_ = {};
    transport_.header.size = sizeof(transport_);
    transport_.header.type = CLAP_EVENT_TRANSPORT;
    if (auto* head = getPlayHead())
    {
        if (const auto position = head->getPosition())
        {
            if (const auto bpm = position->getBpm())
            {
                transport_.flags |= CLAP_TRANSPORT_HAS_TEMPO;
                transport_.tempo = *bpm;
            }
            if (const auto ppq = position->getPpqPosition())
            {
                transport_.flags |= CLAP_TRANSPORT_HAS_BEATS_TIMELINE;
                transport_.song_pos_beats = static_cast<clap_beattime>(std::llround(*ppq * static_cast<double>(CLAP_BEATTIME_FACTOR)));
            }
            if (const auto seconds = position->getTimeInSeconds())
            {
                transport_.flags |= CLAP_TRANSPORT_HAS_SECONDS_TIMELINE;
                transport_.song_pos_seconds = static_cast<clap_sectime>(std::llround(*seconds * static_cast<double>(CLAP_SECTIME_FACTOR)));
            }
            if (const auto signature = position->getTimeSignature())
            {
                transport_.flags |= CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
                transport_.tsig_num = static_cast<std::uint16_t>(signature->numerator);
                transport_.tsig_denom = static_cast<std::uint16_t>(signature->denominator);
            }
            if (position->getIsPlaying())
                transport_.flags |= CLAP_TRANSPORT_IS_PLAYING;
            if (position->getIsRecording())
                transport_.flags |= CLAP_TRANSPORT_IS_RECORDING;
            if (position->getIsLooping())
                transport_.flags |= CLAP_TRANSPORT_IS_LOOP_ACTIVE;
        }
    }

    // Audio: inputs get their own copy (the plug-in may not support in-place processing).
    for (int b = 0; b < getBusCount(true); ++b)
    {
        auto& pointers = inputPointers_[static_cast<std::size_t>(b)];
        const int channels = getChannelCountOfBus(true, b);
        for (int ch = 0; ch < channels; ++ch)
        {
            const int index = getChannelIndexInProcessBlockBuffer(true, b, ch);
            inputScratch_.copyFrom(index, 0, buffer, index, 0, numSamples);
            pointers[static_cast<std::size_t>(ch)] = inputScratch_.getWritePointer(index);
        }
        auto& clapBuffer = inputBuffers_[static_cast<std::size_t>(b)];
        clapBuffer.data32 = pointers.data();
        clapBuffer.data64 = nullptr;
        clapBuffer.channel_count = static_cast<std::uint32_t>(channels);
        clapBuffer.latency = 0;
        clapBuffer.constant_mask = 0;
    }

    for (int b = 0; b < getBusCount(false); ++b)
    {
        auto& pointers = outputPointers_[static_cast<std::size_t>(b)];
        const int channels = getChannelCountOfBus(false, b);
        for (int ch = 0; ch < channels; ++ch)
        {
            const int index = getChannelIndexInProcessBlockBuffer(false, b, ch);
            pointers[static_cast<std::size_t>(ch)] = buffer.getWritePointer(index);
        }
        auto& clapBuffer = outputBuffers_[static_cast<std::size_t>(b)];
        clapBuffer.data32 = pointers.data();
        clapBuffer.data64 = nullptr;
        clapBuffer.channel_count = static_cast<std::uint32_t>(channels);
        clapBuffer.latency = 0;
        clapBuffer.constant_mask = 0;
    }

    clap_process_t process{};
    process.steady_time = steadyTime_;
    process.frames_count = static_cast<std::uint32_t>(numSamples);
    process.transport = &transport_;
    process.audio_inputs = inputBuffers_.data();
    process.audio_outputs = outputBuffers_.data();
    process.audio_inputs_count = static_cast<std::uint32_t>(inputBuffers_.size());
    process.audio_outputs_count = static_cast<std::uint32_t>(outputBuffers_.size());
    process.in_events = inEvents_.get();
    process.out_events = &outEvents_;

    if (processing_.load(std::memory_order_relaxed))
    {
        if (plugin_->process(plugin_, &process) == CLAP_PROCESS_ERROR)
            buffer.clear();
    }
    else
    {
        buffer.clear();
    }

    steadyTime_ += numSamples;
    midi.clear();
    tlsIsAudioThread = false;
    inProcess_.store(false);
}

void ClapPluginInstance::flushParametersOffline()
{
    if (params_ == nullptr || params_->flush == nullptr)
        return;

    InputEventList events(16 * 1024, 1024);
    if (paramsPending_.exchange(false))
        for (auto* parameter : parameters_)
            parameter->appendPendingEvent(events);
    params_->flush(plugin_, events.get(), &outEvents_);
}

void ClapPluginInstance::timerCallback()
{
    auto& context = *context_;
    const std::weak_ptr<int> alive = alive_;

    if (context.callbackRequested.exchange(false))
        plugin_->on_main_thread(plugin_);
    if (alive.expired())
        return;

    if (context.restartRequested.exchange(false) && active_)
    {
        // Re-activate with the audio thread held off; picks up latency or port changes.
        const ScopedSuspend suspend(*this);
        const double rate = sampleRate_;
        const int block = maxBlock_;
        deactivate();
        activate(rate, block);
        updateHostDisplay(ChangeDetails().withLatencyChanged(true));
    }

    if (context.latencyChanged.exchange(false))
    {
        const int before = getLatencySamples();
        updateLatency();
        if (getLatencySamples() != before)
            updateHostDisplay(ChangeDetails().withLatencyChanged(true));
    }

    // While not processing, parameter changes reach the plug-in through params.flush().
    if ((context.flushRequested.exchange(false) || paramsPending_.load()) && !processing_.load())
        flushParametersOffline();

    if (context.dirty.exchange(false))
        updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));

    for (auto* parameter : parameters_)
        parameter->deliverNotification();

#if JUCE_LINUX || JUCE_BSD
    if (fdSupport_ != nullptr && !fds_.empty())
    {
        std::vector<pollfd> polls;
        for (const auto& [fd, flags] : fds_)
        {
            pollfd p{};
            p.fd = fd;
            p.events = static_cast<short>(((flags & CLAP_POSIX_FD_READ) != 0 ? POLLIN : 0) |
                                          ((flags & CLAP_POSIX_FD_WRITE) != 0 ? POLLOUT : 0));
            polls.push_back(p);
        }
        if (::poll(polls.data(), static_cast<nfds_t>(polls.size()), 0) > 0)
            for (const auto& p : polls)
            {
                clap_posix_fd_flags_t flags = 0;
                if ((p.revents & POLLIN) != 0)
                    flags |= CLAP_POSIX_FD_READ;
                if ((p.revents & POLLOUT) != 0)
                    flags |= CLAP_POSIX_FD_WRITE;
                if ((p.revents & (POLLERR | POLLHUP)) != 0)
                    flags |= CLAP_POSIX_FD_ERROR;
                if (flags != 0)
                    fdSupport_->on_fd(plugin_, p.fd, flags);
            }
    }
#endif
}

bool ClapPluginInstance::registerTimer(std::uint32_t periodMs, clap_id* id)
{
    if (timerSupport_ == nullptr || id == nullptr)
        return false;

    const clap_id timerId = nextTimerId_++;
    struct PluginTimer final : juce::Timer
    {
        std::function<void()> fn;
        void timerCallback() override { fn(); }
    };
    auto timer = std::make_unique<PluginTimer>();
    timer->fn = [this, timerId] { timerSupport_->on_timer(plugin_, timerId); };
    timer->startTimer(static_cast<int>(std::max<std::uint32_t>(periodMs, 10)));
    timers_[timerId].timer = std::move(timer);
    *id = timerId;
    return true;
}

bool ClapPluginInstance::unregisterTimer(clap_id id)
{
    return timers_.erase(id) > 0;
}

bool ClapPluginInstance::registerFd(int fd, clap_posix_fd_flags_t flags)
{
    fds_[fd] = flags;
    return true;
}

bool ClapPluginInstance::unregisterFd(int fd)
{
    return fds_.erase(fd) > 0;
}

bool ClapPluginInstance::requestResize(std::uint32_t width, std::uint32_t height)
{
    if (editor_ == nullptr)
        return false;
    if (isMessageThread())
        editor_->resizeFromPlugin(width, height);
    else
        juce::MessageManager::callAsync([alive = std::weak_ptr<int>(alive_), this, width, height]
                                        {
                                            if (!alive.expired() && editor_ != nullptr)
                                                editor_->resizeFromPlugin(width, height);
                                        });
    return true;
}

bool ClapPluginInstance::hasEditor() const
{
    const auto* api = nativeWindowApi();
    return gui_ != nullptr && api != nullptr && gui_->is_api_supported(plugin_, api, false);
}

juce::AudioProcessorEditor* ClapPluginInstance::createEditor()
{
    if (!hasEditor() || editor_ != nullptr)
        return nullptr;
    return new ClapEditor(*this, nativeWindowApi());
}

void ClapPluginInstance::getStateInformation(juce::MemoryBlock& destination)
{
    destination.reset();
    if (state_ == nullptr)
        return;

    juce::MemoryOutputStream out(destination, false);
    clap_ostream_t stream{};
    stream.ctx = &out;
    stream.write = [](const clap_ostream_t* s, const void* buffer, std::uint64_t size) -> std::int64_t
    {
        auto* o = static_cast<juce::MemoryOutputStream*>(s->ctx);
        return o->write(buffer, static_cast<std::size_t>(size)) ? static_cast<std::int64_t>(size) : -1;
    };
    state_->save(plugin_, &stream);
    out.flush();
}

void ClapPluginInstance::setStateInformation(const void* data, int sizeInBytes)
{
    if (state_ == nullptr || data == nullptr || sizeInBytes <= 0)
        return;

    juce::MemoryInputStream in(data, static_cast<std::size_t>(sizeInBytes), false);
    clap_istream_t stream{};
    stream.ctx = &in;
    stream.read = [](const clap_istream_t* s, void* buffer, std::uint64_t size) -> std::int64_t
    {
        auto* i = static_cast<juce::MemoryInputStream*>(s->ctx);
        return static_cast<std::int64_t>(i->read(buffer, static_cast<int>(std::min<std::uint64_t>(size, 1u << 30))));
    };

    if (state_->load(plugin_, &stream) && params_ != nullptr)
    {
        for (auto* parameter : parameters_)
        {
            double value = 0.0;
            if (params_->get_value(plugin_, parameter->getInfo().id, &value))
            {
                parameter->valueFromPlugin(value);
                parameter->deliverNotification();
            }
        }
    }
}

} // namespace

//==================================================================================================
// ClapPluginFormat

int ClapPluginFormat::hashPluginId(const juce::String& clapId)
{
    // FNV-1a, stable across runs and platforms.
    std::uint32_t hash = 2166136261u;
    for (const auto c : clapId.toStdString())
    {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 16777619u;
    }
    return static_cast<int>(hash & 0x7fffffffu);
}

bool ClapPluginFormat::fileMightContainThisPluginType(const juce::String& fileOrIdentifier)
{
    const juce::File file(fileOrIdentifier);
    return file.hasFileExtension(".clap") && file.exists();
}

juce::String ClapPluginFormat::getNameOfPluginFromIdentifier(const juce::String& fileOrIdentifier)
{
    return juce::File(fileOrIdentifier).getFileNameWithoutExtension();
}

bool ClapPluginFormat::pluginNeedsRescanning(const juce::PluginDescription& description)
{
    return juce::File(description.fileOrIdentifier).getLastModificationTime() != description.lastFileModTime;
}

bool ClapPluginFormat::doesPluginStillExist(const juce::PluginDescription& description)
{
    return juce::File(description.fileOrIdentifier).exists();
}

void ClapPluginFormat::findAllTypesForFile(juce::OwnedArray<juce::PluginDescription>& results, const juce::String& fileOrIdentifier)
{
    if (!fileMightContainThisPluginType(fileOrIdentifier))
        return;

    juce::String error;
    const auto module = ClapModule::load(juce::File(fileOrIdentifier), error);
    if (module == nullptr)
        return;

    const auto* factory = module->getFactory();
    const auto count = factory->get_plugin_count(factory);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const auto* descriptor = factory->get_plugin_descriptor(factory, i);
        if (descriptor == nullptr || descriptor->id == nullptr || !clap_version_is_compatible(descriptor->clap_version))
            continue;
        auto description = std::make_unique<juce::PluginDescription>();
        describe(*module, *descriptor, *description);
        results.add(description.release());
    }
}

juce::StringArray ClapPluginFormat::searchPathsForPlugins(const juce::FileSearchPath& directoriesToSearch, bool recursive, bool)
{
    juce::StringArray found;
    for (int i = 0; i < directoriesToSearch.getNumPaths(); ++i)
    {
        const auto directory = directoriesToSearch[i];
        if (!directory.isDirectory())
            continue;
        for (const auto& entry : juce::RangedDirectoryIterator(directory, recursive, "*.clap",
                                                               juce::File::findFilesAndDirectories))
            if (entry.getFile().hasFileExtension(".clap"))
                found.addIfNotAlreadyThere(entry.getFile().getFullPathName());
    }
    return found;
}

juce::FileSearchPath ClapPluginFormat::getDefaultLocationsToSearch()
{
    juce::FileSearchPath paths;
#if JUCE_WINDOWS
    paths.add(juce::File::getSpecialLocation(juce::File::globalApplicationsDirectory).getChildFile("Common Files").getChildFile("CLAP"));
    paths.add(juce::File(juce::SystemStats::getEnvironmentVariable("LOCALAPPDATA", {})).getChildFile("Programs").getChildFile("Common").getChildFile("CLAP"));
#else
    paths.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(".clap"));
    paths.add(juce::File("/usr/lib/clap"));
    paths.add(juce::File("/usr/local/lib/clap"));
#endif
    // CLAP_PATH: extra folders, as defined by the CLAP specification.
    const auto extra = juce::SystemStats::getEnvironmentVariable("CLAP_PATH", {});
#if JUCE_WINDOWS
    const auto separator = ";";
#else
    const auto separator = ":";
#endif
    for (const auto& path : juce::StringArray::fromTokens(extra, separator, {}))
        if (path.isNotEmpty())
            paths.add(juce::File(path));
    return paths;
}

void ClapPluginFormat::createPluginInstance(const juce::PluginDescription& description, double initialSampleRate,
                                           int initialBufferSize, PluginCreationCallback callback)
{
    juce::String error;
    auto module = ClapModule::load(juce::File(description.fileOrIdentifier), error);
    if (module == nullptr)
    {
        callback(nullptr, error);
        return;
    }

    const auto* descriptor = module->findDescriptor(description.uniqueId);
    if (descriptor == nullptr)
    {
        callback(nullptr, description.name + " is no longer in " + juce::File(description.fileOrIdentifier).getFileName());
        return;
    }

    auto instance = ClapPluginInstance::create(std::move(module), *descriptor, error);
    if (instance == nullptr)
    {
        callback(nullptr, error);
        return;
    }

    instance->setRateAndBufferSizeDetails(initialSampleRate, initialBufferSize);
    callback(std::move(instance), {});
}

} // namespace dojo::plugins
