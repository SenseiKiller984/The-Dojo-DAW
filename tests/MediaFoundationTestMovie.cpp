// Test movies for the Media Foundation muxer: kept free of JUCE, the Windows headers first.
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

#include "MediaFoundationTestMovie.h"

#include <cstdio>
#include <cstring>

namespace dojo::test::mf
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

bool start(std::string& error)
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)com; // (S_OK, S_FALSE or RPC_E_CHANGED_MODE: usable)
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
    {
        error = "MFStartup failed";
        return false;
    }
    return true;
}

std::string describe(const char* step, HRESULT hr)
{
    char text[96] = {};
    std::snprintf(text, sizeof(text), "%s failed (0x%08lx)", step, static_cast<unsigned long>(hr));
    return text;
}
} // namespace

bool writeH264Movie(const std::wstring& path, int frames, int fps, int keyFrameInterval, int width, int height,
                    const std::function<void(int frame, std::uint8_t* bgra)>& paint, std::string& error)
{
    if (!start(error))
        return false;
    IMFSinkWriter* writer = nullptr;
    IMFMediaType* out = nullptr;
    IMFMediaType* in = nullptr;
    DWORD stream = 0;
    const char* step = "MFCreateSinkWriterFromURL";
    HRESULT hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, nullptr, &writer);
    if (SUCCEEDED(hr))
        hr = MFCreateMediaType(&out);
    if (SUCCEEDED(hr))
    {
        step = "the H.264 output type";
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        out->SetUINT32(MF_MT_AVG_BITRATE, 1'000'000);
        out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        out->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING, static_cast<UINT32>(keyFrameInterval));
        MFSetAttributeSize(out, MF_MT_FRAME_SIZE, static_cast<UINT32>(width), static_cast<UINT32>(height));
        MFSetAttributeRatio(out, MF_MT_FRAME_RATE, static_cast<UINT32>(fps), 1);
        MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = writer->AddStream(out, &stream);
    }
    if (SUCCEEDED(hr))
        hr = MFCreateMediaType(&in);
    if (SUCCEEDED(hr))
    {
        step = "the RGB32 input type";
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        in->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(width * 4));
        MFSetAttributeSize(in, MF_MT_FRAME_SIZE, static_cast<UINT32>(width), static_cast<UINT32>(height));
        MFSetAttributeRatio(in, MF_MT_FRAME_RATE, static_cast<UINT32>(fps), 1);
        MFSetAttributeRatio(in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = writer->SetInputMediaType(stream, in, nullptr);
    }
    if (SUCCEEDED(hr))
    {
        step = "BeginWriting";
        hr = writer->BeginWriting();
    }

    const auto bytes = static_cast<DWORD>(width * height * 4);
    const LONGLONG duration = 10'000'000LL / fps;
    std::vector<std::uint8_t> pixels(bytes);
    for (int k = 0; SUCCEEDED(hr) && k < frames; ++k)
    {
        step = "writing a frame";
        paint(k, pixels.data());
        IMFMediaBuffer* buffer = nullptr;
        IMFSample* sample = nullptr;
        BYTE* data = nullptr;
        hr = MFCreateMemoryBuffer(bytes, &buffer);
        if (SUCCEEDED(hr))
            hr = buffer->Lock(&data, nullptr, nullptr);
        if (SUCCEEDED(hr))
        {
            std::memcpy(data, pixels.data(), bytes);
            buffer->Unlock();
            hr = buffer->SetCurrentLength(bytes);
        }
        if (SUCCEEDED(hr))
            hr = MFCreateSample(&sample);
        if (SUCCEEDED(hr))
            hr = sample->AddBuffer(buffer);
        if (SUCCEEDED(hr))
            hr = sample->SetSampleTime(k * duration);
        if (SUCCEEDED(hr))
            hr = sample->SetSampleDuration(duration);
        if (SUCCEEDED(hr))
            hr = writer->WriteSample(stream, sample);
        release(sample);
        release(buffer);
    }
    if (SUCCEEDED(hr))
    {
        step = "Finalize";
        hr = writer->Finalize();
    }
    release(in);
    release(out);
    release(writer);
    if (FAILED(hr))
    {
        error = describe(step, hr);
        return false;
    }
    return true;
}

bool readSound(const std::wstring& path, std::vector<float>& interleaved, int& channels, int& sampleRate, std::string& error)
{
    interleaved.clear();
    if (!start(error))
        return false;
    constexpr auto kAudio = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    IMFSourceReader* reader = nullptr;
    IMFMediaType* pcm = nullptr;
    IMFMediaType* actual = nullptr;
    const char* step = "MFCreateSourceReaderFromURL";
    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader);
    if (SUCCEEDED(hr))
    {
        step = "selecting the sound";
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        hr = reader->SetStreamSelection(kAudio, TRUE);
    }
    if (SUCCEEDED(hr))
        hr = MFCreateMediaType(&pcm);
    if (SUCCEEDED(hr))
    {
        step = "decoding to float";
        pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        hr = reader->SetCurrentMediaType(kAudio, nullptr, pcm);
    }
    if (SUCCEEDED(hr))
        hr = reader->GetCurrentMediaType(kAudio, &actual);
    UINT32 count = 0;
    UINT32 rate = 0;
    if (SUCCEEDED(hr))
    {
        actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &count);
        actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    }
    while (SUCCEEDED(hr))
    {
        step = "reading";
        DWORD flags = 0;
        IMFSample* sample = nullptr;
        hr = reader->ReadSample(kAudio, 0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
        {
            release(sample);
            break;
        }
        if (sample == nullptr)
            continue;
        IMFMediaBuffer* buffer = nullptr;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)))
        {
            BYTE* data = nullptr;
            DWORD length = 0;
            if (SUCCEEDED(buffer->Lock(&data, nullptr, &length)))
            {
                const auto* first = reinterpret_cast<const float*>(data);
                interleaved.insert(interleaved.end(), first, first + length / sizeof(float));
                buffer->Unlock();
            }
        }
        release(buffer);
        release(sample);
    }
    release(actual);
    release(pcm);
    release(reader);
    if (FAILED(hr))
    {
        error = describe(step, hr);
        return false;
    }
    channels = static_cast<int>(count);
    sampleRate = static_cast<int>(rate);
    return true;
}

} // namespace dojo::test::mf

#endif // _WIN32
