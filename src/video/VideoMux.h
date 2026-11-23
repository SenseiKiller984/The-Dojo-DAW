#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <functional>

namespace dojo::video
{

/** How the mix is stored in the movie. */
enum class MuxAudioCodec
{
    aac, ///< AAC (lossy, plays everywhere)
    pcm, ///< PCM 24-bit (lossless)
};

/** A movie with a new sound: see muxAudioIntoVideo. */
struct MuxSettings
{
    juce::File video;        ///< the movie: its picture is copied as it is (not re-encoded)
    double from = 0.0;       ///< seconds into it where the result starts (a key frame: keyFrameAtOrBefore)
    double to = 0.0;         ///< seconds into it where the result ends (0: its end)
    juce::File audio;        ///< the mix (a WAV file)
    double audioOffset = 0.0; ///< seconds of silence before the mix's first sample (from `from`)
    juce::File destination;  ///< the result (muxSupport says which extension)
    MuxAudioCodec codec = MuxAudioCodec::aac;
    int aacBitrate = 320000; ///< bits a second, all channels (Media Foundation: up to 192 kbps)
    /** The mix's speakers, channel by channel (empty: unnamed, in the usual order for its count).
        The movie gets the channels in its container's order (the WAVE speaker order: L R C LFE,
        backs, sides, tops, then wides and top sides) with their speakers named; leftSurround is
        5.1's surround (a back), leftSurroundSide and leftSurroundRear 7.1's sides and backs.
        (A list, not a juce::AudioChannelSet: that keeps its own order, and Dolby Digital's
        L C R Ls Rs LFE is not it.) */
    juce::Array<juce::AudioChannelSet::ChannelType> speakers;
};

/** The channel-mask bit (FFmpeg's AV_CH_*, the same as WAVE's SPEAKER_* below bit 18) that names a
    speaker in a movie's sound, or -1 for one it has no name for. */
[[nodiscard]] int speakerBit(juce::AudioChannelSet::ChannelType type) noexcept;

/** What can put a mix into a movie, on this system. */
struct MuxSupport
{
    juce::String backend;   ///< "FFmpeg 6.1", "Media Foundation", "built-in (AVI)"; empty: none
    juce::String extension; ///< of the result (".mp4", ".mov", ".mkv", ".avi"...)
    /** Of the result with PCM sound: an MP4's goes into a MOV (QuickTime's 'in24' is what editors
        read; MP4's own PCM, ISO/IEC 23003-5 'ipcm', is new: even FFmpeg 6.1 misreads its 24-bit). */
    juce::String pcmExtension;
    bool pcm = false;       ///< lossless PCM sound
    int aacChannels = 0;    ///< the most channels its AAC holds (0: no AAC)
    bool aacSurround = false; ///< (Media Foundation: only 1, 2 or 6 channels)
    int aacMaxBitrate = 0;    ///< the most AAC bits a second it writes (0: no limit of its own; Media Foundation: 192 kbps)
    [[nodiscard]] bool canMux() const noexcept { return backend.isNotEmpty(); }
    /** Whether AAC holds a mix of `channels`. */
    [[nodiscard]] bool aacHolds(int channels) const noexcept;
};
[[nodiscard]] MuxSupport muxSupport(const juce::File& video);

/** The time (seconds) of the last key frame at or before `seconds`: where a copy of the movie's
    picture can start (any frame of a Motion-JPEG AVI). */
[[nodiscard]] double keyFrameAtOrBefore(const juce::File& video, double seconds);

/**
    Writes `settings.destination`: the movie's picture from `from` to `to` copied packet by packet
    (no re-encoding, so no loss and no time), with the mix as its only sound (AAC or PCM), its first
    sample at `from` + `audioOffset`. The picture's timing, size and codec stay as they were. Every
    picture shown before `to` is there, and those it is predicted from: with B-frames that can be a
    frame shown at or just after `to` (the sound ends at `to`).
      - FFmpeg (Linux): MP4, MOV, MKV, WebM, AVI... in the movie's own container; AAC up to 7.1, PCM
        (24-bit; 32-bit in an MP4, which is better given a .mov: see MuxSupport::pcmExtension).
      - Media Foundation (Windows): an MP4 file; AAC (mono, stereo or 5.1, up to 192 kbps).
      - Built in (every platform): a Motion-JPEG AVI into an AVI with PCM sound.
    `progress` (0..1) is called on this thread; return false from it to cancel. On failure (or a
    cancel) nothing is left at the destination.
*/
[[nodiscard]] juce::Result muxAudioIntoVideo(const MuxSettings& settings, const std::function<bool(double)>& progress = {});

/** The sound of a media file, decoded (FFmpeg, or the built-in AVI reader): its channels and rate,
    from the file's time zero (where its picture starts): an encoder's priming that the file places
    before zero is left out, a late start is preceded by silence. False with `error` set if there is
    none or nothing here can decode it. */
[[nodiscard]] bool decodeAudio(const juce::File& file, juce::AudioBuffer<float>& audio, double& sampleRate, juce::String& error);

} // namespace dojo::video
