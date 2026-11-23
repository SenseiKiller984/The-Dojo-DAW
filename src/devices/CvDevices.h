#pragma once

#include "core/HardwareIo.h"
#include "core/ModulationSource.h"
#include "devices/BuiltinProcessor.h"

#include <array>
#include <atomic>
#include <vector>

namespace dojo::devices
{

/**
    Control voltages for modular synthesizers (Eurorack and the like), through a DC-coupled sound
    card's outputs (an Expert Sleepers ES-8 / ES-9, a MOTU, an RME...): each device sends voltages
    straight to the outputs chosen for its ports, beside the mix, and can take audio or voltages back
    in from the card's inputs.

    Voltages become samples by the card's full scale (the volts at a sample of 1.0: 10 V on most
    DC-coupled Eurorack interfaces, set per device); Delay holds the voltages back to meet audio that
    the session's delay compensation and the card's output path delay. Offline renders have no sound
    card: the voltages go nowhere (and CV In reads 0).

      CV Instrument  MIDI to pitch (1 V/octave from the 0 V note, with tracking and offset trims and
                     glide), gate, velocity and mod wheel voltages; the modular's audio back in.
                     Calibrate: steps of 0..3 V are sent and the oscillator's pitch measured on the
                     audio input, and tracking and offset set so that every note plays in tune.
      CV Triggers    eight trigger or gate outputs, each for a MIDI note (drum modules).
      CV Clock       a clock at the session's tempo (1 to 48 pulses a quarter note, with swing) and
                     run and reset outputs, following the transport.
      CV Utility     a voltage from an automatable value (and an LFO), slewed: automation lanes and
                     the LFO device drive the modular's controls.
      CV In          a voltage from an input as a modulation source, mapped (like the LFO device) to
                     any control in the session.
*/
namespace cv
{
/** "None", then the sound card's channels "1" .. "64". */
[[nodiscard]] const juce::StringArray& channelChoices();
/** A choice's 0-based channel (-1: none). */
[[nodiscard]] inline int channelOf(int choice) noexcept { return choice - 1; }
/** Pitch voltage of a (fractional) MIDI note: 1 V an octave from `zeroNote`, times `tracking`, plus `offset` volts. */
[[nodiscard]] inline double pitchVolts(double note, double zeroNote, double tracking, double offset) noexcept
{
    return (note - zeroNote) / 12.0 * tracking + offset;
}
/** The pitch (Hz) of a steady tone (autocorrelation, refined between lags); 0 if none. */
[[nodiscard]] double measurePitch(const float* samples, int numSamples, double sampleRate);
} // namespace cv

/** What the CV devices share: their ports (outputs), full scale, delay, and the sound card. */
class CvDevice : public BuiltinProcessor, public HardwareIoDevice
{
public:
    static constexpr int kMaxPorts = 8;
    static constexpr double kMaxDelayMs = 250.0;

    // --- HardwareIoDevice --------------------------------------------------------------------------
    void setHardwarePorts(const HardwarePorts& ports) noexcept override;
    void addSends(float* const* outputs, int numOutputs, int offset, int numSamples) noexcept override;

    [[nodiscard]] int getNumPorts() const noexcept { return numPorts_; }
    [[nodiscard]] const juce::String& getPortName(int port) const { return portNames_[static_cast<std::size_t>(port)]; }
    [[nodiscard]] juce::AudioParameterChoice* getPortChoice(int port) const noexcept { return portChoices_[static_cast<std::size_t>(port)]; }
    /** The voltage a port sent last (after the delay), for meters. */
    [[nodiscard]] float getPortVolts(int port) const noexcept { return portVolts_[static_cast<std::size_t>(port)].load(std::memory_order_relaxed); }
    /** The sound card as last seen (outputs 0 and not live offline). */
    [[nodiscard]] bool isLive() const noexcept { return live_.load(std::memory_order_relaxed); }
    [[nodiscard]] int getSeenOutputs() const noexcept { return seenOutputs_.load(std::memory_order_relaxed); }
    [[nodiscard]] int getSeenInputs() const noexcept { return seenInputs_.load(std::memory_order_relaxed); }
    /** Ports set to channels the sound card lacks (for the editor). */
    [[nodiscard]] juce::String getProblems() const;

    juce::AudioParameterFloat* fullScale_ = nullptr; ///< volts at a sample of 1.0
    juce::AudioParameterFloat* delay_ = nullptr;     ///< ms
    juce::AudioParameterChoice* audioIn_ = nullptr;  ///< (devices with an audio input) the modular's audio back
    juce::AudioParameterFloat* inputGain_ = nullptr;

protected:
    CvDevice(juce::String typeId, juce::String name, bool instrument);
    [[nodiscard]] static BusesProperties busesFor(bool instrument);

    /** Adds port `name` (its output choice parameter "out<index>"). */
    void addPort(const juce::String& name, int defaultChoice);
    /** Adds the Audio In choice: the card's channels the device's output takes (passing nothing without them). */
    void addAudioIn();
    void addCommon();

    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept final;
    /** Fills `volts` (one buffer per port, `numSamples` each) for the block; `ports` holds this
        sub-block's sound card (or not, offline). */
    virtual void generate(double* const* volts, int numSamples, juce::MidiBuffer& midi, const HardwarePorts& ports) noexcept = 0;
    virtual void prepareCv(double sampleRate, int maximumBlockSize) = 0;

    /** The transport at the start of the block: playing, quarter notes, tempo. */
    struct Transport
    {
        bool playing = false;
        double ppq = 0.0;
        double bpm = 120.0;
    };
    [[nodiscard]] Transport transport() const noexcept;

private:
    int numPorts_ = 0;
    std::array<juce::String, kMaxPorts> portNames_;
    std::array<juce::AudioParameterChoice*, kMaxPorts> portChoices_{};
    std::array<std::atomic<float>, kMaxPorts> portVolts_{};
    juce::AudioBuffer<double> volts_;   ///< this block, per port
    juce::AudioBuffer<double> line_;    ///< the delay line, per port (samples)
    juce::AudioBuffer<double> send_;    ///< what goes out after the graph, per port (samples)
    std::array<int, kMaxPorts> sentTo_{};
    int write_ = 0;
    int sendSamples_ = 0;
    HardwarePorts ports_;
    std::atomic<bool> live_{false};
    std::atomic<int> seenOutputs_{0}, seenInputs_{0};
};

/** MIDI to pitch, gate, velocity and modulation voltages (monophonic), and the modular's audio back. */
class CvInstrument final : public CvDevice
{
public:
    static constexpr const char* kTypeId = "builtin:cvinstrument";
    enum Port
    {
        pitchPort,
        gatePort,
        velocityPort,
        modPort,
    };
    CvInstrument();

    juce::AudioParameterFloat* zeroNote_ = nullptr;
    juce::AudioParameterFloat* transpose_ = nullptr;
    juce::AudioParameterFloat* fine_ = nullptr;
    juce::AudioParameterFloat* tracking_ = nullptr; ///< % of 1 V/octave
    juce::AudioParameterFloat* offset_ = nullptr;   ///< mV
    juce::AudioParameterFloat* bendRange_ = nullptr;
    juce::AudioParameterFloat* glide_ = nullptr;    ///< ms
    juce::AudioParameterChoice* glideMode_ = nullptr;
    juce::AudioParameterChoice* priority_ = nullptr;
    juce::AudioParameterFloat* gateVolts_ = nullptr;
    juce::AudioParameterBool* retrigger_ = nullptr;
    juce::AudioParameterFloat* velocityVolts_ = nullptr;
    juce::AudioParameterFloat* modVolts_ = nullptr;

    // --- Calibration (Calibrate in the editor) -------------------------------------------------------
    enum class Calibration
    {
        idle,
        waiting,   ///< for the sound card
        running,   ///< sending the steps and listening
        analysing, ///< service() measures
        done,
        failed,
    };
    static constexpr int kCalibrationSteps = 4; ///< 0, 1, 2 and 3 V
    static constexpr double kStepSeconds = 0.4; ///< each held this long (the last half measured)
    /** Starts calibrating (needs Pitch Out and Audio In on the sound card), or cancels. */
    void calibrate();
    [[nodiscard]] Calibration getCalibration() const noexcept { return static_cast<Calibration>(calibration_.load(std::memory_order_acquire)); }
    [[nodiscard]] juce::String getCalibrationMessage() const;
    /** The pitches measured at each step (Hz), after a calibration. */
    [[nodiscard]] std::vector<double> getMeasuredPitches() const;
    void service() override;
    /** Sets tracking and offset from measured pitches (Hz at 0, 1, 2, 3 V); false if they do not fit. */
    bool applyCalibration(const std::vector<double>& hzPerVolt);

    /** The note sounding (or -1) and the pitch it is at now (fractional, after glide and bend). */
    [[nodiscard]] int getCurrentNote() const noexcept { return currentNote_.load(std::memory_order_relaxed); }

protected:
    void prepareCv(double sampleRate, int maximumBlockSize) override;
    void generate(double* const* volts, int numSamples, juce::MidiBuffer& midi, const HardwarePorts& ports) noexcept override;

private:
    void noteOn(int note, float velocity) noexcept;
    void noteOff(int note) noexcept;
    [[nodiscard]] int chooseNote() const noexcept;

    std::array<bool, 128> held_{};
    std::array<float, 128> velocities_{};
    std::array<int, 128> order_{}; ///< when each held note started (a counter)
    int counter_ = 0;
    int sounding_ = -1;
    double pitch_ = 60.0;      ///< glided pitch (semitones)
    double glideFrom_ = 60.0;
    double glideTarget_ = 60.0;
    double glideLeft_ = 0.0;   ///< samples
    double glideTotal_ = 0.0;
    double bend_ = 0.0;        ///< -1..1
    double mod_ = 0.0;         ///< 0..1
    double velocity_ = 0.0;
    int retriggerLeft_ = 0;    ///< samples of gate low
    std::atomic<int> currentNote_{-1};

    std::atomic<int> calibration_{0};
    int calibrationPosition_ = 0;
    std::vector<float> capture_;
    std::vector<double> measured_;
    juce::String calibrationMessage_;
};

/** Eight triggers (or gates), each for a MIDI note. */
class CvTriggers final : public CvDevice
{
public:
    static constexpr const char* kTypeId = "builtin:cvtriggers";
    static constexpr int kSlots = 8;
    CvTriggers();

    std::array<juce::AudioParameterFloat*, kSlots> notes_{};
    juce::AudioParameterChoice* mode_ = nullptr; ///< Trigger, Gate
    juce::AudioParameterFloat* length_ = nullptr; ///< ms (triggers)
    juce::AudioParameterFloat* volts_ = nullptr;
    juce::AudioParameterBool* velocity_ = nullptr; ///< level from velocity

protected:
    void prepareCv(double sampleRate, int maximumBlockSize) override;
    void generate(double* const* volts, int numSamples, juce::MidiBuffer& midi, const HardwarePorts& ports) noexcept override;

private:
    std::array<int, kSlots> left_{};      ///< trigger samples to go
    std::array<bool, kSlots> holding_{};  ///< (gate mode)
    std::array<double, kSlots> level_{};
};

/** A clock at the session's tempo, with run and reset. */
class CvClock final : public CvDevice
{
public:
    static constexpr const char* kTypeId = "builtin:cvclock";
    enum Port
    {
        clockPort,
        runPort,
        resetPort,
    };
    CvClock();

    juce::AudioParameterChoice* rate_ = nullptr; ///< pulses a quarter note
    juce::AudioParameterFloat* width_ = nullptr; ///< % of a pulse
    juce::AudioParameterFloat* swing_ = nullptr; ///< % (every second pulse later)
    juce::AudioParameterFloat* volts_ = nullptr;
    [[nodiscard]] static int pulsesPerQuarter(int choice) noexcept;
    /** Pulses sent since the transport started (for the editor). */
    [[nodiscard]] int getPulses() const noexcept { return pulses_.load(std::memory_order_relaxed); }

protected:
    void prepareCv(double sampleRate, int maximumBlockSize) override;
    void generate(double* const* volts, int numSamples, juce::MidiBuffer& midi, const HardwarePorts& ports) noexcept override;

private:
    bool wasPlaying_ = false;
    int resetLeft_ = 0;
    std::atomic<int> pulses_{0};
    double lastPulse_ = -1.0; ///< the last pulse's number (to count each once)
};

/** A voltage from a value and an LFO, slewed. */
class CvUtility final : public CvDevice
{
public:
    static constexpr const char* kTypeId = "builtin:cvutility";
    CvUtility();

    juce::AudioParameterFloat* value_ = nullptr;   ///< 0..1 between Min and Max
    juce::AudioParameterFloat* minimum_ = nullptr; ///< V
    juce::AudioParameterFloat* maximum_ = nullptr; ///< V
    juce::AudioParameterChoice* shape_ = nullptr;  ///< LFO: Off, Sine, Triangle, Saw, Square, Random
    juce::AudioParameterFloat* rate_ = nullptr;    ///< Hz
    juce::AudioParameterFloat* depth_ = nullptr;   ///< V
    juce::AudioParameterFloat* slew_ = nullptr;    ///< ms

protected:
    void prepareCv(double sampleRate, int maximumBlockSize) override;
    void generate(double* const* volts, int numSamples, juce::MidiBuffer& midi, const HardwarePorts& ports) noexcept override;

private:
    double phase_ = 0.0;
    double held_ = 0.0;
    double out_ = 0.0;
    std::uint32_t random_ = 22222;
};

/** A voltage from an input as a modulation source (mapped like the LFO device). */
class CvIn final : public BuiltinProcessor, public HardwareIoDevice, public ModulationSource
{
public:
    static constexpr const char* kTypeId = "builtin:cvin";
    CvIn();

    juce::AudioParameterChoice* input_ = nullptr;
    juce::AudioParameterFloat* minimum_ = nullptr; ///< V at the bottom of the range
    juce::AudioParameterFloat* maximum_ = nullptr; ///< V at the top
    juce::AudioParameterFloat* fullScale_ = nullptr;
    juce::AudioParameterFloat* smooth_ = nullptr;  ///< ms
    juce::AudioParameterBool* invert_ = nullptr;

    void setHardwarePorts(const HardwarePorts& ports) noexcept override;
    void addSends(float* const*, int, int, int) noexcept override {}
    double advanceModulation(int numSamples, double sampleRate, double bpm, double ppq, bool playing) noexcept override;
    /** The voltage read last, and the output (0..1). */
    [[nodiscard]] float getVolts() const noexcept { return volts_.load(std::memory_order_relaxed); }
    [[nodiscard]] float getOutput() const noexcept { return output_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool isLive() const noexcept { return live_.load(std::memory_order_relaxed); }

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept override;

private:
    HardwarePorts ports_;
    double reading_ = 0.0; ///< volts, the latest sub-block's mean
    double smoothed_ = 0.0;
    std::atomic<float> volts_{0.0f}, output_{0.0f};
    std::atomic<bool> live_{false};
};

} // namespace dojo::devices
