#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace dojo::ui
{

/**
    The user's preferences, stored in the application settings file. Changing one sends a change
    message (asynchronously) to listeners: the application applies the engine options, the main
    window re-themes itself.
*/
class Preferences final : public juce::ChangeBroadcaster
{
public:
    /** The tabs of the Preferences window. */
    enum class Tab
    {
        general,
        themes,
        audio,
        surround,
        keyboard,
        controlSurfaces,
        renderer,
    };

    /** How a surround master (3.1, 5.1, 7.1, Dolby Digital, DTS) plays (the Surround tab). */
    enum class SurroundMode
    {
        standard,    ///< as it is: an output per channel; a stereo device plays the front pair
        foldDown,    ///< an output per channel if there are enough, else the ITU stereo fold-down
        virtualizer, ///< the advanced virtualizer (headphones or speakers)
        device,      ///< the device's own 3D virtualizer, sent the surround channels
    };

    explicit Preferences(juce::PropertiesFile& settings);

    /** Returning the playhead to the start (Stop twice, the return button, Home) also scrolls the
        arrangement back to the start. Off by default. */
    [[nodiscard]] bool getReturnViewToStart() const;
    void setReturnViewToStart(bool enabled);

    /** Playback stops at the end of the last clip. Off by default. */
    [[nodiscard]] bool getStopAtEnd() const;
    void setStopAtEnd(bool enabled);

    /** Beat Maker Mode: the computer keyboard's keys A to P play the selected track's Drum Rack
        (its 16 pads), and record into the selected MIDI clip while recording (ui/BeatMaker.h). Off
        by default. */
    [[nodiscard]] bool getBeatMakerMode() const;
    void setBeatMakerMode(bool enabled);

    /** MPE controllers: notes played one per MIDI channel 2..16 (an MPE controller's lower zone)
        record their own pitch bend, pressure and slide as the notes' expression curves. On by
        default. Off: those channels' messages record as the clip's controllers. */
    [[nodiscard]] bool getMpeInput() const;
    void setMpeInput(bool enabled);
    /** The MPE controller's per-note pitch bend range in semitones (2, 12, 24 or 48, the MPE default). */
    [[nodiscard]] double getMpeBendRange() const;
    void setMpeBendRange(double semitones);

    /** Surround monitoring. Classic fold-down by default (what earlier versions did). */
    [[nodiscard]] SurroundMode getSurroundMode() const;
    void setSurroundMode(SurroundMode mode);
    /** The virtualizer renders for speakers (crosstalk cancelled) instead of headphones. */
    [[nodiscard]] bool getVirtualizerSpeakers() const;
    void setVirtualizerSpeakers(bool speakers);
    /** The virtualizer's room (0..1): reflections and reverberation. 0.5 by default. */
    [[nodiscard]] double getVirtualizerRoom() const;
    void setVirtualizerRoom(double room);

    /** An external renderer (Dolby Atmos Renderer, SPAT Revolution, L-ISA...): the object tracks on
        outputs of their own after the bed's, and/or their positions sent as ADM-OSC. */
    struct ExternalRenderer
    {
        bool objectOutputs = false;
        bool osc = false;
        juce::String host = "127.0.0.1";
        int port = 9000;
        int rate = 50;          ///< updates a second
        bool cartesian = false; ///< /xyz (the Cartesian room) instead of /aed (polar)
    };
    [[nodiscard]] ExternalRenderer getExternalRenderer() const;
    void setExternalRenderer(const ExternalRenderer& renderer);

    /** The theme's name (see theme::getThemes()). */
    [[nodiscard]] juce::String getThemeName() const;
    void setThemeName(const juce::String& name);

    /** Customised keyboard shortcuts (differences from the defaults), or null. */
    [[nodiscard]] std::unique_ptr<juce::XmlElement> getKeyMappings() const;
    void setKeyMappings(const juce::XmlElement& mappings);

    /** The control surfaces (models and MIDI ports), as surfaces::toString() writes them. */
    [[nodiscard]] juce::String getControlSurfaces() const;
    void setControlSurfaces(const juce::String& surfaces);

    [[nodiscard]] juce::PropertiesFile& getSettings() noexcept { return settings_; }

private:
    void changed();

    juce::PropertiesFile& settings_;
};

} // namespace dojo::ui
