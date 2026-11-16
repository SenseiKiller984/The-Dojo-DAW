#pragma once

#include "devices/BuiltinProcessor.h"
#include "core/InsertLoop.h"
#include "devices/dsp/Biquad.h"

#include <array>
#include <atomic>
#include <complex>
#include <vector>

namespace dojo::devices
{

/**
    The suitcase's lower half and its vibrato: the stereo auto-panner, the two power amplifiers and
    the speakers. Used by the Rhodes Suitcase itself and by the Suitcase Base device (the return of
    its accessory loop).

    - Vibrato (the auto-panner): a dual op-amp LFO (a smoothed triangle, 1 to 10 Hz on Speed) drives
      two attenuators 180 degrees apart, so the sound swings between the left and right amplifiers
      by Intensity. Opto: optocouplers (an LED lighting a photoresistor: quick to brighten, slower to
      darken, a rounded swing); JFET: field-effect attenuators (exact and symmetric).
    - Power amps: two 40 W channels into 4 ohms on unregulated +-32 V rails: under 0.5 % THD at the
      rated 40 W (0 dBu in), rounding off about 3 dB above it, and the rails sag on long loud chords.
    - Speakers: four 10-inch speakers (a parallel pair per side) or two 12-inch ones; the system
      reaches about 30 Hz - 15 kHz. Line Out takes the 0 dBu umbilical signals instead (after the
      vibrato, before the power amps), as when recording the suitcase direct.
    - Levels: 0 dBu = -18 dBFS in and out; Master trims the output.
*/
class SuitcaseStage final
{
public:
    static constexpr double kNominal = 0.1;             ///< 0 dBu in the circuits' units
    static constexpr double kAmpHeadroomDb = 3.0;       ///< over the rated 40 W
    static constexpr double kAmpHardness = 8.0;
    static constexpr double kUnitsToDbfs = 1.2589254117941673; ///< 0 dBu (0.1) to -18 dBFS

    /** The stage's controls (owned by the processor). */
    struct Controls
    {
        juce::AudioParameterFloat* speed = nullptr;     ///< 0 .. 10 (1 .. 10 Hz)
        juce::AudioParameterFloat* intensity = nullptr; ///< 0 .. 10
        juce::AudioParameterChoice* modulator = nullptr; ///< Opto, JFET
        juce::AudioParameterChoice* output = nullptr;   ///< Speakers, Line Out
        juce::AudioParameterChoice* cabinet = nullptr;  ///< 4x10", 2x12"
        juce::AudioParameterFloat* master = nullptr;
    };
    /** Speed as shown and typed: the LFO's rate in Hz. */
    [[nodiscard]] static juce::String speedText(float position);
    [[nodiscard]] static float speedFromText(const juce::String& text);

    [[nodiscard]] static double rateHz(double speed) noexcept { return std::pow(10.0, std::clamp(speed, 0.0, 10.0) / 10.0); }

    void prepare(double sampleRate);
    void reset() noexcept;
    /** Mono (circuit units, 0 dBu = 0.1) in, stereo out (dBFS), `n` samples. */
    void process(const Controls& controls, const double* mono, double* left, double* right, int n) noexcept;

    /** The LFO now (-1 .. 1): the LED's brightness follows it. */
    [[nodiscard]] float getLfo() const noexcept { return lfoOut_.load(std::memory_order_relaxed); }
    /** The power amps' output levels (dB re the rated 40 W) since the last call. */
    [[nodiscard]] float takeAmpPeakDb() noexcept { return ampPeak_.exchange(-100.0f, std::memory_order_acq_rel); }

private:
    double sampleRate_ = 48000.0;
    double phase_ = 0.0;
    std::array<double, 2> light_{0.5, 0.5}, gain_{1.0, 1.0}, sag_{0.0, 0.0};
    std::array<std::array<dsp::Biquad, 4>, 2> speaker_;
    std::array<dsp::BiquadCoefficients, 4> speakerCoefficients_;
    int cabinet_ = -1;
    std::atomic<float> lfoOut_{0.0f};
    std::atomic<float> ampPeak_{-100.0f};
};

/**
    A 1980s Rhodes Suitcase 73 Mark II electric piano, modelled stage by stage.

    - The harp: 73 keys (E1 to E7, MIDI 28 to 100), each a tine and tone bar struck by a neoprene
      hammer: the tine's fundamental (and the tone bar's slow beat against it), and the tine's bell
      overtones, which ring briefly at the attack. Hammer: how hard the tips are (a shorter contact,
      a brighter strike); harder playing also strikes brighter. Tine: how much bell. Decay scales
      how long the tines sing; Release is the dampers' felt on the key's release (the top 19 keys
      have no dampers and die away by themselves); the sustain pedal (CC 64) lifts the dampers.
    - The pickups: each tine swings in front of a variable-reluctance pickup (an Alnico magnet wound
      with copper wire): the voltage is the change of the magnetic flux, and the flux is not linear
      in the tine's position, so playing harder brings in the harmonics of the famous "bark".
      Voicing sets the tines' alignment with their pickups: mellow (off-centre, farther) to barky
      (closer to centre). Age spreads the tuning, voicing and tone-bar beating from key to key, as on
      an instrument that has been played for years. Velocity Curve sets the touch.
    - Harp Load: the Janus preamp's high-impedance input (over 1 MOhm) keeps every harmonic; plugged
      into a guitar amplifier (100 kOhm) or a mixer (10 kOhm) the harp's inductance rolls the top off.
    - The Janus preamp: Volume (an audio-taper slider), Treble (+-12 dB shelf at 10 kHz) and Bass
      (+-15 dB shelf at about 75 Hz) on +-15 V op-amps (0 dBu nominal, about +21 dBu before they
      clip).
    - Accessory Loop: devices put into the Rhodes' own insert loop (in the Devices panel, the loop
      right after it) run between the send (after the EQ, before the vibrato, 0 dBu = -18 dBFS) and
      the return, like pedals plugged into the suitcase's loop jacks; the Accessory Loop switch takes
      them in and out. With nothing in the loop the contacts are normalled: the signal goes straight
      through.
    - The suitcase: see SuitcaseStage (vibrato, power amps, speakers or line out).
*/
class RhodesSuitcase final : public BuiltinProcessor, public InsertLoopHost, private juce::AudioProcessorParameter::Listener
{
public:
    static constexpr const char* kTypeId = "builtin:rhodes73";
    static constexpr int kKeys = 73;
    static constexpr int kLowestNote = 28;      ///< E1
    static constexpr int kUndampedKeys = 19;    ///< the top keys without dampers
    static constexpr int kModes = 4;            ///< fundamental, tone-bar beat, two bell overtones

    RhodesSuitcase();
    ~RhodesSuitcase() override;

    // InsertLoopHost: the accessory loop (send after the EQ, return into the vibrato).
    [[nodiscard]] bool isLoopEngaged() const noexcept override { return loop_->get(); }
    void processSend(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;
    void processReturn(juce::AudioBuffer<double>& buffer, int loopLatency) noexcept override;
    [[nodiscard]] juce::String getLoopName() const override { return "Accessory Loop"; }
    [[nodiscard]] juce::String getReturnDescription() const override { return "vibrato, power amps, speakers"; }

    /** Keys sounding now (for the display and tests). */
    [[nodiscard]] int getSoundingKeys() const noexcept { return sounding_.load(std::memory_order_relaxed); }
    /** Whether key `key` (0 = E1 .. 72 = E7) is sounding. */
    [[nodiscard]] bool isKeySounding(int key) const noexcept
    {
        if (key < 0 || key >= kKeys)
            return false;
        return ((keyMask_[static_cast<std::size_t>(key / 64)].load(std::memory_order_relaxed) >> (key % 64)) & 1u) != 0;
    }
    [[nodiscard]] float getLfo() const noexcept { return stage_.getLfo(); }
    [[nodiscard]] static double frequencyOf(int note, double cents = 0.0) noexcept;

    // The harp
    juce::AudioParameterFloat* voicing_ = nullptr;
    juce::AudioParameterFloat* hammer_ = nullptr;
    juce::AudioParameterFloat* tine_ = nullptr;
    juce::AudioParameterFloat* decay_ = nullptr;
    juce::AudioParameterFloat* release_ = nullptr;
    juce::AudioParameterFloat* velocityCurve_ = nullptr;
    juce::AudioParameterFloat* age_ = nullptr;
    juce::AudioParameterFloat* tuning_ = nullptr;
    juce::AudioParameterChoice* load_ = nullptr; ///< Janus (1 MOhm), Amp (100 kOhm), Mixer (10 kOhm)
    // The Janus preamp
    juce::AudioParameterFloat* volume_ = nullptr;
    juce::AudioParameterFloat* treble_ = nullptr;
    juce::AudioParameterFloat* bass_ = nullptr;
    juce::AudioParameterBool* loop_ = nullptr;
    // The suitcase
    SuitcaseStage::Controls stage{};

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    struct Key
    {
        std::array<std::complex<double>, kModes> z{};         ///< the modes (displacement = imaginary part)
        std::array<std::complex<double>, kModes> heldStep{};  ///< per sample, dampers off
        std::array<std::complex<double>, kModes> dampedStep{}; ///< per sample, damper on the tine
        std::array<double, kModes> ratio{};                   ///< mode frequency / fundamental
        double frequency = 440.0;
        double offset = 1.0;    ///< the tine's offset from the pickup's centre (in gaps)
        double swing = 0.5;     ///< displacement at full velocity (in gaps)
        double normal = 1.0;    ///< output normalisation (the pickup's small-signal slope)
        double previousFlux = 0.0;
        std::array<double, kModes> pulse{}; ///< the hammer's strike per mode (spread over the contact)
        int contactLeft = 0, contactLength = 1;
        bool held = false, active = false;
    };

    void parameterValueChanged(int index, float value) override;
    void parameterGestureChanged(int, bool) override {}
    /** The harp and the Janus preamp: `n` samples of the post-EQ signal (circuit units) into harp_. */
    void renderPreamp(juce::MidiBuffer& midi, int n) noexcept;
    void handleMidi(const std::uint8_t* data, int size) noexcept;
    void strike(int key, double velocity) noexcept;
    void release(int key) noexcept;
    void updateKeys() noexcept;
    void render(int from, int to) noexcept;
    [[nodiscard]] bool damped(int key) const noexcept;
    [[nodiscard]] double flux(const Key& key, double x) const noexcept;

    std::array<Key, kKeys> keys_;
    bool sustainPedal_ = false;
    std::vector<double> harp_; ///< the harp's output, per block
    dsp::Biquad loadFilter_, trebleFilter_, bassFilter_;
    SuitcaseStage stage_;
    std::array<float, 8> signature_{}; ///< (when the key tables were last computed)
    std::atomic<int> sounding_{0};
    std::array<std::atomic<std::uint64_t>, 2> keyMask_{};
};

} // namespace dojo::devices
