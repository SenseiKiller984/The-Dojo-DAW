#pragma once

#include "core/HardwareIo.h"
#include "devices/BuiltinProcessor.h"

#include <atomic>
#include <optional>
#include <vector>

namespace dojo::devices
{

/**
    Measuring a hardware loop's round trip: a sweep goes out, what comes back is recorded from the
    moment it left, and the round trip is where the two correlate best (an FFT cross-correlation).
    A sweep correlates with itself in a single sharp peak, so the delay is found to the sample,
    through EQ, transformers and noise; the peak's sign gives the loop's polarity and its height
    the loop's gain.
*/
namespace loopback
{

/** The ping: a logarithmic sine sweep from 40 Hz to 16 kHz (or 0.4 of the rate) of 0.15 s, with
    short fades, peaking at `level` (linear). */
[[nodiscard]] std::vector<float> makePing(double sampleRate, float level = 1.0f);

struct Measurement
{
    bool found = false;     ///< a clear copy of the ping came back
    int latency = 0;        ///< samples from sending it to its return
    float gainDb = -100.0f; ///< the return's level against what was sent
    bool inverted = false;  ///< it came back upside down
    float clarityDb = 0.0f; ///< the correlation peak over the rest of the correlation (RMS)
    /** The correlation's magnitude (0 .. 1, the peak at 1) from no delay to `correlationLags`, at most
        512 points, for a display. */
    std::vector<float> correlation;
    int correlationLags = 0; ///< the round trips `correlation` covers: four times the one found, or more
};

inline constexpr float kMinClarityDb = 15.0f; ///< below this the peak may be noise
inline constexpr float kMinGainDb = -60.0f;   ///< below this nothing really came back

/** Finds `ping` in `recorded` (which starts as the ping was sent), at round trips that leave the
    whole ping inside the recording. The correlation for display covers at least `minimumView` round
    trips (and at least 512). */
[[nodiscard]] Measurement measure(const std::vector<float>& ping, const float* recorded, int recordedLength, int minimumView = 0);

} // namespace loopback

/**
    HW Insert: outboard gear patched into a track like a plug-in. The track's signal leaves the
    computer through a pair (or one) of the sound card's outputs, goes through the hardware and
    comes back through a pair (or one) of its inputs, in place of the signal (or blended with it).

    - Send To / Return From: the sound card's channels (stereo pairs or single channels), and Send
      / Return levels (+-24 dB); Phase flips the return; Dry/Wet blends in the signal as it was,
      delayed to meet the return.
    - The round trip (the sound card's buffers both ways and the converters) is reported as the
      device's latency, so the session's delay compensation lines everything up with the return.
    - PING measures it: a short sweep goes out (the device is silent meanwhile), what comes back is
      recorded for half a second and the round trip found to the sample (loopback::measure). It
      also sets Phase when the hardware inverts. Auto Ping measures by itself when the device first
      runs and whenever the sample rate, the buffer or the channels change: only while the
      transport is stopped, and once per setup if nothing comes back. Offset nudges the delay
      compensation. Until measured, the driver's reported latency stands in.
    - A return channel the sound card lacks passes the signal through (dry), and offline renders
      (no sound card) do too, delayed by the latency so everything stays in time.

    The engine talks to it as a HardwareIoDevice: the sound card's inputs before each sub-block,
    the send added to the outputs after the graph ran. Measurements are analysed in service() on
    the message thread.
*/
class HardwareInsert final : public BuiltinProcessor, public HardwareIoDevice
{
public:
    static constexpr const char* kTypeId = "builtin:hwinsert";
    /** The longest round trip it compensates (the engine's delay compensation reaches 8191). */
    static constexpr int kMaxLatency = 8191;
    static constexpr double kListenSeconds = 0.5; ///< how long a ping listens for its return

    HardwareInsert();

    /** A choice of sound card channels: `count` (0, 1 or 2) channels from `first` (0-based). */
    struct Channels
    {
        int first = -1;
        int count = 0;
        bool operator==(const Channels&) const = default;
    };
    /** "None", the stereo pairs "1/2" .. "63/64", then the single channels "1" .. "64". */
    [[nodiscard]] static const juce::StringArray& channelChoices();
    [[nodiscard]] static Channels channelsOf(int choice) noexcept;
    [[nodiscard]] static int choiceOf(Channels channels) noexcept;
    [[nodiscard]] Channels getSendChannels() const noexcept;
    [[nodiscard]] Channels getReturnChannels() const noexcept;

    // --- HardwareIoDevice ---------------------------------------------------------------------------
    void setHardwarePorts(const HardwarePorts& ports) noexcept override;
    void addSends(float* const* outputs, int numOutputs, int offset, int numSamples) noexcept override;
    void service() override;

    // --- Latency measurement (message thread) -------------------------------------------------------
    enum class PingState
    {
        idle,
        waiting,     ///< asked for: starts with the next sub-block that has the sound card
        running,     ///< sending the sweep and listening
        analysing,   ///< recorded: service() finds the round trip
        unavailable, ///< the channels are not on the sound card (service() reports it)
    };
    /** Starts a measurement (or cancels one still waiting for the sound card). */
    void ping();
    [[nodiscard]] PingState getPingState() const noexcept { return static_cast<PingState>(pingState_.load(std::memory_order_acquire)); }

    /** What a measurement found, and the setup it was made with. */
    struct Calibration
    {
        bool valid = false;
        int latency = 0;
        float gainDb = 0.0f;
        bool inverted = false;
        float clarityDb = 0.0f;
        double sampleRate = 0.0;
        int bufferSize = 0;
        int send = 0, ret = 0; ///< the channel choices
    };
    [[nodiscard]] const Calibration& getCalibration() const noexcept { return calibration_; }
    /** Whether the calibration was made with the current sample rate, buffer and channels. */
    [[nodiscard]] bool isCalibrationCurrent() const;
    /** The last measurement's correlation, for the display (0 .. 1 from no delay to
        getCorrelationLags()). */
    [[nodiscard]] const std::vector<float>& getCorrelation() const noexcept { return correlation_; }
    [[nodiscard]] int getCorrelationLags() const noexcept { return correlationLags_; }
    /** How long a ping listens for its return (samples). */
    [[nodiscard]] int getListenSamples() const noexcept { return listenSamples_; }
    /** What happened last, in words ("Measured: 256 samples (5.33 ms)", "Nothing came back..."). */
    [[nodiscard]] const juce::String& getMessage() const noexcept { return message_; }

    /** Sets Return so that the loop comes back at the level it was sent (after a measurement). */
    void matchLevels();

    /** The sound card as the device last saw it (any thread). */
    struct Hardware
    {
        bool live = false; ///< processed with a sound card recently
        int numInputs = 0, numOutputs = 0, bufferSize = 0, reportedLatency = 0;
    };
    [[nodiscard]] Hardware getHardware() const noexcept;
    /** The send's and the return's peaks since the last call (linear; message thread). */
    [[nodiscard]] std::pair<float, float> takePeaks() noexcept;

    juce::AudioParameterChoice* send_ = nullptr;
    juce::AudioParameterChoice* return_ = nullptr;
    juce::AudioParameterFloat* sendGain_ = nullptr;
    juce::AudioParameterFloat* returnGain_ = nullptr;
    juce::AudioParameterFloat* mix_ = nullptr;
    juce::AudioParameterBool* phase_ = nullptr;
    juce::AudioParameterFloat* offset_ = nullptr;
    juce::AudioParameterBool* autoPing_ = nullptr;
    juce::AudioParameterChoice* pingLevel_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;
    void writeExtraState(juce::XmlElement& xml) const override;
    void readExtraState(const juce::XmlElement& xml) override;

private:
    struct Setup
    {
        int sampleRateHz = 0; ///< (whole hertz: compared exactly)
        int bufferSize = 0;
        int send = 0, ret = 0;
        bool operator==(const Setup&) const = default;
    };
    [[nodiscard]] Setup currentSetup() const;
    [[nodiscard]] float pingLevelGain() const;
    void analyse();
    void updateLatency();

    // Audio thread.
    HardwarePorts ports_;
    juce::AudioBuffer<double> sendBuffer_;
    int sendSamples_ = 0;
    Channels sentTo_;
    juce::AudioBuffer<double> dryLine_;
    int dryWrite_ = 0;
    std::vector<float> ping_; ///< the sweep at full level
    std::vector<float> capture_;
    int pingPosition_ = 0;
    int capturePosition_ = 0;
    float pingGain_ = 0.0f;
    double sendGainNow_ = 1.0, returnGainNow_ = 1.0, mixNow_ = 1.0, muteNow_ = 1.0;
    double heldLeft_ = 0.0, heldRight_ = 0.0; ///< the output as a ping starts (it fades out from there)
    double smoothing_ = 0.002, muteStep_ = 0.004;

    // Shared.
    std::atomic<int> pingState_{0};
    std::atomic<int> appliedLatency_{0};
    std::atomic<float> pingLevelAsked_{0.125f};
    std::atomic<float> sendPeak_{0.0f}, returnPeak_{0.0f};
    std::atomic<bool> seenLive_{false}, playing_{false};
    std::atomic<int> seenInputs_{0}, seenOutputs_{0}, seenBuffer_{0}, seenReported_{0};
    std::atomic<std::uint32_t> processed_{0};

    // Message thread.
    Calibration calibration_;
    std::optional<Setup> autoTried_;
    std::uint32_t lastProcessed_ = 0;
    int idleServices_ = 0;
    juce::String message_;
    std::vector<float> correlation_;
    int correlationLags_ = 0;
    int listenSamples_ = 0;
};

} // namespace dojo::devices
