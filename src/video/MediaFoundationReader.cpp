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

#include "video/MediaFoundationReader.h"

#include <algorithm>
#include <cstring>

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

/** Media Foundation, started once for the process (and left running until it exits). */
bool startMediaFoundation()
{
    static const bool started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    return started;
}

/** COM on this thread: already initialised in any apartment is fine. */
void ensureCom()
{
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)hr; // S_OK, S_FALSE (already) or RPC_E_CHANGED_MODE (an STA thread): COM is usable
}
} // namespace

struct Reader::Impl
{
    IMFSourceReader* reader = nullptr;
    int width = 0;
    int height = 0;
    LONG stride = 0;
    double frameRate = 25.0;
    double duration = 0.0;
    GUID subtype{};

    ~Impl() { release(reader); }

    bool readFormat()
    {
        IMFMediaType* type = nullptr;
        if (FAILED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type)))
            return false;

        UINT32 w = 0;
        UINT32 h = 0;
        const bool sized = SUCCEEDED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h));
        UINT32 numerator = 0;
        UINT32 denominator = 0;
        if (SUCCEEDED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &numerator, &denominator)) && numerator > 0 && denominator > 0)
            frameRate = static_cast<double>(numerator) / static_cast<double>(denominator);
        UINT32 defaultStride = 0;
        stride = SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &defaultStride)) ? static_cast<LONG>(static_cast<INT32>(defaultStride))
                                                                                  : static_cast<LONG>(w * 4);
        release(type);
        width = static_cast<int>(w);
        height = static_cast<int>(h);
        return sized && width > 0 && height > 0;
    }
};

Reader::Reader()
    : impl_(std::make_unique<Impl>())
{
}

Reader::~Reader() = default;

bool Reader::open(const std::wstring& path, std::string& error)
{
    ensureCom();
    if (!startMediaFoundation())
    {
        error = "Media Foundation is not available on this system.";
        return false;
    }

    IMFAttributes* attributes = nullptr;
    if (FAILED(MFCreateAttributes(&attributes, 1)))
        return false;
    attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE); // YUV -> RGB32 for us
    const HRESULT created = MFCreateSourceReaderFromURL(path.c_str(), attributes, &impl_->reader);
    release(attributes);
    if (FAILED(created) || impl_->reader == nullptr)
    {
        error = "Media Foundation cannot open this file.";
        return false;
    }

    auto* reader = impl_->reader;
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (FAILED(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE)))
    {
        error = "The file has no video.";
        return false;
    }

    // The codec (from the native type), then ask for 32-bit RGB.
    IMFMediaType* native = nullptr;
    if (SUCCEEDED(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &native)))
    {
        native->GetGUID(MF_MT_SUBTYPE, &impl_->subtype);
        release(native);
    }

    IMFMediaType* rgb = nullptr;
    if (FAILED(MFCreateMediaType(&rgb)))
        return false;
    rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    const HRESULT set = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, rgb);
    release(rgb);
    if (FAILED(set) || !impl_->readFormat())
    {
        error = "Media Foundation has no decoder for this video.";
        return false;
    }

    PROPVARIANT duration;
    PropVariantInit(&duration);
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration))
        && duration.vt == VT_UI8)
        impl_->duration = static_cast<double>(duration.uhVal.QuadPart) / 1.0e7;
    PropVariantClear(&duration);
    return true;
}

int Reader::getWidth() const noexcept { return impl_->width; }
int Reader::getHeight() const noexcept { return impl_->height; }
double Reader::getFrameRate() const noexcept { return impl_->frameRate; }
double Reader::getDuration() const noexcept { return impl_->duration; }

std::string Reader::getCodec() const
{
    const auto& s = impl_->subtype;
    if (IsEqualGUID(s, MFVideoFormat_H264))
        return "h264";
    if (IsEqualGUID(s, MFVideoFormat_HEVC))
        return "hevc";
    if (IsEqualGUID(s, MFVideoFormat_MP4V))
        return "mpeg4";
    if (IsEqualGUID(s, MFVideoFormat_MJPG))
        return "mjpeg";
    if (IsEqualGUID(s, MFVideoFormat_WMV3))
        return "wmv3";
    if (IsEqualGUID(s, MFVideoFormat_MPEG2))
        return "mpeg2video";
    return "video";
}

void Reader::seek(double seconds)
{
    if (impl_->reader == nullptr)
        return;
    ensureCom();
    PROPVARIANT position;
    PropVariantInit(&position);
    position.vt = VT_I8;
    position.hVal.QuadPart = static_cast<LONGLONG>(std::max(0.0, seconds) * 1.0e7);
    const GUID timeFormat{}; // GUID_NULL: 100-nanosecond units
    impl_->reader->SetCurrentPosition(timeFormat, position);
    PropVariantClear(&position);
}

bool Reader::readNext(std::vector<std::uint8_t>& bgra, int& width, int& height, double& time)
{
    if (impl_->reader == nullptr)
        return false;
    ensureCom();

    for (int attempt = 0; attempt < 64; ++attempt)
    {
        DWORD stream = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        IMFSample* sample = nullptr;
        if (FAILED(impl_->reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream, &flags, &timestamp, &sample)))
            return false;
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
        {
            release(sample);
            return false;
        }
        if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0)
            impl_->readFormat();
        if (sample == nullptr)
            continue; // a gap in the stream

        IMFMediaBuffer* buffer = nullptr;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
        {
            release(sample);
            return false;
        }

        BYTE* data = nullptr;
        DWORD length = 0;
        bool ok = false;
        if (SUCCEEDED(buffer->Lock(&data, nullptr, &length)))
        {
            width = impl_->width;
            height = impl_->height;
            const LONG stride = impl_->stride != 0 ? impl_->stride : static_cast<LONG>(width * 4);
            const auto rowBytes = static_cast<std::size_t>(width) * 4;
            const auto absStride = static_cast<std::size_t>(stride < 0 ? -stride : stride);
            if (static_cast<std::size_t>(length) >= absStride * static_cast<std::size_t>(height - 1) + rowBytes)
            {
                bgra.resize(rowBytes * static_cast<std::size_t>(height));
                for (int y = 0; y < height; ++y)
                {
                    // A negative stride: bottom-up rows.
                    const BYTE* row = stride >= 0 ? data + static_cast<std::size_t>(y) * absStride
                                                  : data + static_cast<std::size_t>(height - 1 - y) * absStride;
                    auto* out = bgra.data() + static_cast<std::size_t>(y) * rowBytes;
                    std::memcpy(out, row, rowBytes);
                    for (std::size_t x = 3; x < rowBytes; x += 4)
                        out[x] = 0xff; // RGB32 has no alpha
                }
                time = static_cast<double>(timestamp) / 1.0e7;
                ok = true;
            }
            buffer->Unlock();
        }
        release(buffer);
        release(sample);
        return ok;
    }
    return false;
}

} // namespace dojo::video::mf

#endif
