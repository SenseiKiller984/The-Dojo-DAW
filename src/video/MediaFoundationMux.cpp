#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include "video/MediaFoundationMux.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace dojo::video::mf
{
namespace
{
template <typename T>
void release(T*& p)
{
    if (p != nullptr)
    {
        p->Release();
        p = nullptr;
    }
}

bool startMediaFoundation()
{
    static const bool started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    return started;
}

void ensureCom()
{
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)hr; // S_OK, S_FALSE (already) or RPC_E_CHANGED_MODE (an STA thread): COM is usable
}

constexpr DWORD kVideo = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

/** A reader handing out the movie's picture as it is stored (no decoder), and its media type. */
IMFSourceReader* openCompressed(const std::wstring& path, IMFMediaType** type)
{
    IMFSourceReader* reader = nullptr;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader)) || reader == nullptr)
        return nullptr;
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (FAILED(reader->SetStreamSelection(kVideo, TRUE)) || FAILED(reader->GetNativeMediaType(kVideo, 0, type))
        || FAILED(reader->SetCurrentMediaType(kVideo, nullptr, *type)))
    {
        release(*type);
        release(reader);
        return nullptr;
    }
    return reader;
}

bool seek(IMFSourceReader* reader, double seconds)
{
    PROPVARIANT position;
    PropVariantInit(&position);
    position.vt = VT_I8;
    position.hVal.QuadPart = static_cast<LONGLONG>(std::llround(seconds * 1.0e7));
    const HRESULT hr = reader->SetCurrentPosition(GUID_NULL, position);
    PropVariantClear(&position);
    return SUCCEEDED(hr);
}

IMFMediaType* audioType(const GUID& subtype, int sampleRate, int channels, std::uint32_t bytesPerSecond)
{
    IMFMediaType* type = nullptr;
    if (FAILED(MFCreateMediaType(&type)))
        return nullptr;
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    type->SetGUID(MF_MT_SUBTYPE, subtype);
    type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(sampleRate));
    type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, static_cast<UINT32>(channels));
    type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32>(bytesPerSecond));
    if (subtype == MFAudioFormat_PCM)
    {
        type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, static_cast<UINT32>(channels * 2));
        type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    }
    return type;
}
} // namespace

double keyFrameAtOrBefore(const std::wstring& video, double seconds)
{
    ensureCom();
    if (seconds <= 0.0 || !startMediaFoundation())
        return 0.0;
    IMFMediaType* type = nullptr;
    IMFSourceReader* reader = openCompressed(video, &type);
    if (reader == nullptr)
        return 0.0;
    double found = -1.0;
    if (seek(reader, seconds))
    {
        DWORD flags = 0;
        LONGLONG time = 0;
        IMFSample* sample = nullptr;
        if (SUCCEEDED(reader->ReadSample(kVideo, 0, nullptr, &flags, &time, &sample)) && sample != nullptr)
            found = static_cast<double>(time) / 1.0e7;
        release(sample);
    }
    release(type);
    release(reader);
    return found >= 0.0 && found <= seconds + 1.0e-6 ? found : 0.0;
}

bool mux(const MuxRequest& request, const PullAudio& pull, const std::function<bool(double)>& progress, std::string& error)
{
    ensureCom();
    if (!startMediaFoundation())
    {
        error = "Media Foundation is not available on this system.";
        return false;
    }
    if (!(request.to > request.from) || request.channels <= 0 || request.sampleRate <= 0)
    {
        error = "Nothing to write.";
        return false;
    }

    IMFMediaType* pictureType = nullptr;
    IMFSourceReader* reader = openCompressed(request.video, &pictureType);
    if (reader == nullptr)
    {
        error = "Media Foundation cannot read the movie's picture.";
        return false;
    }

    IMFAttributes* attributes = nullptr;
    IMFSinkWriter* writer = nullptr;
    IMFMediaType* aac = nullptr;
    IMFMediaType* pcm = nullptr;
    DWORD pictureStream = 0;
    DWORD soundStream = 0;
    bool ok = SUCCEEDED(MFCreateAttributes(&attributes, 1)) && SUCCEEDED(attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4))
              && SUCCEEDED(MFCreateSinkWriterFromURL(request.destination.c_str(), nullptr, attributes, &writer)) && writer != nullptr;
    // The picture: its own type in and out, so the writer passes the samples through.
    ok = ok && SUCCEEDED(writer->AddStream(pictureType, &pictureStream)) && SUCCEEDED(writer->SetInputMediaType(pictureStream, pictureType, nullptr));
    if (!ok)
        error = "Media Foundation cannot write this movie's picture into an MP4 file.";
    // The sound: 16-bit PCM in, AAC out.
    if (ok)
    {
        aac = audioType(MFAudioFormat_AAC, request.sampleRate, request.channels, request.aacBytesPerSecond);
        pcm = audioType(MFAudioFormat_PCM, request.sampleRate, request.channels,
                        static_cast<std::uint32_t>(request.sampleRate * request.channels * 2));
        ok = aac != nullptr && pcm != nullptr && SUCCEEDED(writer->AddStream(aac, &soundStream))
             && SUCCEEDED(writer->SetInputMediaType(soundStream, pcm, nullptr));
        if (!ok)
            error = "Media Foundation's AAC encoder does not take this mix (it takes mono, stereo or 5.1 at 44.1 or 48 kHz).";
    }
    ok = ok && SUCCEEDED(writer->BeginWriting());

    const auto duration = static_cast<LONGLONG>(std::llround((request.to - request.from) * 1.0e7));
    LONGLONG soundTime = 0;
    bool soundEnded = false;
    constexpr int kFrames = 4096;
    std::vector<std::int16_t> block(static_cast<std::size_t>(kFrames * request.channels));
    const auto soundUntil = [&](LONGLONG until) -> bool
    {
        while (!soundEnded && soundTime < until && soundTime < duration)
        {
            const int got = pull(block.data(), kFrames);
            if (got <= 0)
            {
                soundEnded = true;
                break;
            }
            const auto bytes = static_cast<DWORD>(got * request.channels * 2);
            IMFMediaBuffer* buffer = nullptr;
            IMFSample* sample = nullptr;
            BYTE* data = nullptr;
            bool written = SUCCEEDED(MFCreateMemoryBuffer(bytes, &buffer)) && SUCCEEDED(buffer->Lock(&data, nullptr, nullptr));
            if (written)
            {
                std::memcpy(data, block.data(), bytes);
                buffer->Unlock();
                buffer->SetCurrentLength(bytes);
                const auto length = static_cast<LONGLONG>(std::llround(got * 1.0e7 / request.sampleRate));
                written = SUCCEEDED(MFCreateSample(&sample)) && SUCCEEDED(sample->AddBuffer(buffer)) && SUCCEEDED(sample->SetSampleTime(soundTime))
                          && SUCCEEDED(sample->SetSampleDuration(length)) && SUCCEEDED(writer->WriteSample(soundStream, sample));
                soundTime += length;
            }
            release(sample);
            release(buffer);
            if (!written)
                return false;
        }
        return true;
    };

    // From the key frame at `from`: every picture shown before `to` and the ones it is predicted
    // from, the sound kept a little ahead. With B-frames a picture shown after `to` can come first in
    // decoding order: it is held back, and written only if one shown before `to` follows it (H.264
    // reorders at most 16 pictures, so after 17 held ones nothing shown before `to` can follow).
    if (ok && request.from > 0.0)
        seek(reader, request.from);
    LONGLONG base = -1;
    bool cancelled = false;
    std::vector<IMFSample*> held;
    const auto write = [&](IMFSample* sample, LONGLONG at) -> bool
    {
        return soundUntil(at + 2500000) && SUCCEEDED(sample->SetSampleTime(at)) && SUCCEEDED(writer->WriteSample(pictureStream, sample));
    };
    while (ok)
    {
        DWORD flags = 0;
        LONGLONG time = 0;
        IMFSample* sample = nullptr;
        if (FAILED(reader->ReadSample(kVideo, 0, nullptr, &flags, &time, &sample)))
        {
            ok = false;
            error = "Reading the movie failed.";
            break;
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0 || sample == nullptr)
        {
            release(sample);
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
                break;
            continue;
        }
        if (base < 0)
            base = time;
        const LONGLONG at = time - base;
        if (at >= duration)
        {
            held.push_back(sample); // (its reference kept until written or dropped)
            if (held.size() > 16)
                break;
            continue;
        }
        for (auto*& h : held)
        {
            LONGLONG shown = 0;
            ok = ok && SUCCEEDED(h->GetSampleTime(&shown)) && write(h, shown - base);
            release(h);
        }
        held.clear();
        ok = ok && write(sample, at);
        release(sample);
        if (!ok)
            error = "Writing the MP4 failed.";
        if (ok && progress && !progress(std::clamp(static_cast<double>(at) / static_cast<double>(duration), 0.0, 1.0)))
        {
            cancelled = true;
            break;
        }
    }
    for (auto*& h : held)
        release(h);
    if (ok && !cancelled)
        ok = soundUntil(duration);
    if (writer != nullptr && SUCCEEDED(writer->Finalize()) == false && ok && !cancelled)
    {
        ok = false;
        error = "Finishing the MP4 failed.";
    }
    if (cancelled)
    {
        ok = false;
        error = "Export cancelled.";
    }

    release(pcm);
    release(aac);
    release(writer);
    release(attributes);
    release(pictureType);
    release(reader);
    return ok;
}

} // namespace dojo::video::mf

#endif // _WIN32
