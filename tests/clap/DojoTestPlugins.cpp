// A tiny CLAP binary used by the test-suite to exercise The Dojo's CLAP host for real:
//   dojo.test.gain   stereo effect: gain parameter (id 7), 64 samples of latency, saves its state
//   dojo.test.synth  instrument: a sine per held note (CLAP note events or raw MIDI)
// With DOJO_TEST_CLAP_CRASH=1 in the environment, loading it crashes (to test the scanner blacklist).

#include <clap/clap.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
constexpr int kLatency = 64;
constexpr clap_id kGainParam = 7;

const char* const gainFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
const char* const synthFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, nullptr};

const clap_plugin_descriptor_t gainDescriptor{CLAP_VERSION_INIT, "dojo.test.gain", "Dojo Test Gain", "The Dojo Tests", "", "", "",
                                              "1.0.0", "Gain with latency", gainFeatures};
const clap_plugin_descriptor_t synthDescriptor{CLAP_VERSION_INIT, "dojo.test.synth", "Dojo Test Synth", "The Dojo Tests", "", "", "",
                                               "1.0.0", "Sine per note", synthFeatures};

struct Plugin
{
    clap_plugin_t clap{};
    const clap_host_t* host = nullptr;
    bool synth = false;
    double sampleRate = 48000.0;
    double gain = 1.0;
    std::vector<float> delay[2];
    int delayIndex = 0;
    std::array<double, 128> phase{};
    std::array<bool, 128> held{};
};

Plugin& self(const clap_plugin_t* p) { return *static_cast<Plugin*>(p->plugin_data); }

void applyEvent(Plugin& plugin, const clap_event_header_t* event)
{
    if (event->space_id != CLAP_CORE_EVENT_SPACE_ID)
        return;
    switch (event->type)
    {
    case CLAP_EVENT_PARAM_VALUE:
    {
        const auto* e = reinterpret_cast<const clap_event_param_value_t*>(event);
        if (e->param_id == kGainParam)
            plugin.gain = e->value;
        break;
    }
    case CLAP_EVENT_NOTE_ON:
    {
        const auto* e = reinterpret_cast<const clap_event_note_t*>(event);
        if (e->key >= 0 && e->key < 128)
            plugin.held[static_cast<std::size_t>(e->key)] = true;
        break;
    }
    case CLAP_EVENT_NOTE_OFF:
    {
        const auto* e = reinterpret_cast<const clap_event_note_t*>(event);
        if (e->key >= 0 && e->key < 128)
            plugin.held[static_cast<std::size_t>(e->key)] = false;
        break;
    }
    case CLAP_EVENT_MIDI:
    {
        const auto* e = reinterpret_cast<const clap_event_midi_t*>(event);
        const int status = e->data[0] & 0xf0;
        if (status == 0x90 && e->data[2] > 0)
            plugin.held[e->data[1] & 0x7f] = true;
        else if (status == 0x80 || status == 0x90)
            plugin.held[e->data[1] & 0x7f] = false;
        break;
    }
    default:
        break;
    }
}

// --- params ----------------------------------------------------------------------------------------
const clap_plugin_params_t params{
    [](const clap_plugin_t* p) -> std::uint32_t { return self(p).synth ? 0u : 1u; },
    [](const clap_plugin_t*, std::uint32_t index, clap_param_info_t* info)
    {
        if (index != 0)
            return false;
        std::memset(info, 0, sizeof(*info));
        info->id = kGainParam;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE;
        std::strcpy(info->name, "Gain");
        info->min_value = 0.0;
        info->max_value = 2.0;
        info->default_value = 1.0;
        return true;
    },
    [](const clap_plugin_t* p, clap_id id, double* value)
    {
        if (id != kGainParam)
            return false;
        *value = self(p).gain;
        return true;
    },
    [](const clap_plugin_t*, clap_id, double value, char* out, std::uint32_t capacity)
    {
        std::snprintf(out, capacity, "%.2fx", value);
        return true;
    },
    [](const clap_plugin_t*, clap_id, const char* text, double* value)
    {
        *value = std::atof(text);
        return true;
    },
    [](const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t*)
    {
        for (std::uint32_t i = 0; i < in->size(in); ++i)
            applyEvent(self(p), in->get(in, i));
    }};

// --- state ---------------------------------------------------------------------------------------
const clap_plugin_state_t state{[](const clap_plugin_t* p, const clap_ostream_t* stream)
                                { return stream->write(stream, &self(p).gain, sizeof(double)) == sizeof(double); },
                                [](const clap_plugin_t* p, const clap_istream_t* stream)
                                {
                                    double value = 0.0;
                                    if (stream->read(stream, &value, sizeof(double)) != sizeof(double))
                                        return false;
                                    self(p).gain = value;
                                    return true;
                                }};

// --- latency, ports --------------------------------------------------------------------------------
const clap_plugin_latency_t latency{[](const clap_plugin_t* p) -> std::uint32_t { return self(p).synth ? 0u : kLatency; }};

const clap_plugin_audio_ports_t audioPorts{
    [](const clap_plugin_t* p, bool isInput) -> std::uint32_t { return isInput && self(p).synth ? 0u : 1u; },
    [](const clap_plugin_t* p, std::uint32_t index, bool isInput, clap_audio_port_info_t* info)
    {
        if (index != 0 || (isInput && self(p).synth))
            return false;
        std::memset(info, 0, sizeof(*info));
        info->id = isInput ? 0 : 1;
        std::strcpy(info->name, isInput ? "In" : "Out");
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        return true;
    }};

const clap_plugin_note_ports_t notePorts{
    [](const clap_plugin_t* p, bool isInput) -> std::uint32_t { return isInput && self(p).synth ? 1u : 0u; },
    [](const clap_plugin_t* p, std::uint32_t index, bool isInput, clap_note_port_info_t* info)
    {
        if (index != 0 || !isInput || !self(p).synth)
            return false;
        std::memset(info, 0, sizeof(*info));
        info->id = 0;
        info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
        info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
        std::strcpy(info->name, "Notes");
        return true;
    }};

// --- plug-in ---------------------------------------------------------------------------------------
clap_process_status process(const clap_plugin_t* p, const clap_process_t* process)
{
    auto& plugin = self(p);
    const auto events = process->in_events;
    for (std::uint32_t i = 0; i < events->size(events); ++i)
        applyEvent(plugin, events->get(events, i));

    float** out = process->audio_outputs[0].data32;
    const std::uint32_t frames = process->frames_count;

    if (plugin.synth)
    {
        for (std::uint32_t n = 0; n < frames; ++n)
        {
            double sample = 0.0;
            for (std::size_t key = 0; key < 128; ++key)
            {
                if (!plugin.held[key])
                    continue;
                const double frequency = 440.0 * std::pow(2.0, (static_cast<double>(key) - 69.0) / 12.0);
                sample += 0.25 * std::sin(plugin.phase[key]);
                plugin.phase[key] += 2.0 * 3.14159265358979323846 * frequency / plugin.sampleRate;
            }
            out[0][n] = out[1][n] = static_cast<float>(sample);
        }
        return CLAP_PROCESS_CONTINUE;
    }

    float** in = process->audio_inputs[0].data32;
    for (std::uint32_t n = 0; n < frames; ++n)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            auto& line = plugin.delay[ch];
            const float delayed = line[static_cast<std::size_t>(plugin.delayIndex)];
            line[static_cast<std::size_t>(plugin.delayIndex)] = in[ch][n];
            out[ch][n] = static_cast<float>(delayed * plugin.gain);
        }
        plugin.delayIndex = (plugin.delayIndex + 1) % kLatency;
    }
    return CLAP_PROCESS_CONTINUE;
}

const clap_plugin_t* createPlugin(const clap_host_t* host, bool synth)
{
    auto* plugin = new Plugin();
    plugin->host = host;
    plugin->synth = synth;
    plugin->clap.desc = synth ? &synthDescriptor : &gainDescriptor;
    plugin->clap.plugin_data = plugin;
    plugin->clap.init = [](const clap_plugin_t*) { return true; };
    plugin->clap.destroy = [](const clap_plugin_t* p) { delete &self(p); };
    plugin->clap.activate = [](const clap_plugin_t* p, double rate, std::uint32_t, std::uint32_t)
    {
        auto& plugin = self(p);
        plugin.sampleRate = rate;
        for (auto& line : plugin.delay)
            line.assign(kLatency, 0.0f);
        plugin.delayIndex = 0;
        return true;
    };
    plugin->clap.deactivate = [](const clap_plugin_t*) {};
    plugin->clap.start_processing = [](const clap_plugin_t*) { return true; };
    plugin->clap.stop_processing = [](const clap_plugin_t*) {};
    plugin->clap.reset = [](const clap_plugin_t*) {};
    plugin->clap.process = process;
    plugin->clap.get_extension = [](const clap_plugin_t*, const char* id) -> const void*
    {
        if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
            return &params;
        if (std::strcmp(id, CLAP_EXT_STATE) == 0)
            return &state;
        if (std::strcmp(id, CLAP_EXT_LATENCY) == 0)
            return &latency;
        if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
            return &audioPorts;
        if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0)
            return &notePorts;
        return nullptr;
    };
    plugin->clap.on_main_thread = [](const clap_plugin_t*) {};
    return &plugin->clap;
}

const clap_plugin_factory_t factory{
    [](const clap_plugin_factory_t*) -> std::uint32_t { return 2; },
    [](const clap_plugin_factory_t*, std::uint32_t index) -> const clap_plugin_descriptor_t*
    { return index == 0 ? &gainDescriptor : index == 1 ? &synthDescriptor : nullptr; },
    [](const clap_plugin_factory_t*, const clap_host_t* host, const char* id) -> const clap_plugin_t*
    {
        if (std::strcmp(id, gainDescriptor.id) == 0)
            return createPlugin(host, false);
        if (std::strcmp(id, synthDescriptor.id) == 0)
            return createPlugin(host, true);
        return nullptr;
    }};
bool entryInit(const char*)
{
    const char* crash = std::getenv("DOJO_TEST_CLAP_CRASH");
    if (crash != nullptr && crash[0] == '1')
        std::abort();
    return true;
}

void entryDeinit() {}

const void* entryGetFactory(const char* id)
{
    return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : nullptr;
}
} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry{CLAP_VERSION_INIT, entryInit, entryDeinit, entryGetFactory};
