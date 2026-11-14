#ifdef _WIN32

#include "videoengine_backend.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace ReasonVideo {

namespace {

constexpr int kMaxRenderWidth  = 1280;
constexpr int kMaxRenderHeight = 720;

struct ScopedMfInit
{
    bool coOk {false};
    bool mfOk {false};

    ScopedMfInit ()
    {
        HRESULT hrCo = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
        coOk = SUCCEEDED (hrCo);
        if (SUCCEEDED (MFStartup (MF_VERSION, MFSTARTUP_NOSOCKET)))
            mfOk = true;
    }

    ~ScopedMfInit ()
    {
        if (mfOk)
            MFShutdown ();
        if (coOk)
            CoUninitialize ();
    }
};

std::wstring utf8ToWide (const std::string& utf8)
{
    if (utf8.empty ())
        return {};
    const int needed = MultiByteToWideChar (CP_UTF8, 0, utf8.c_str (), -1, nullptr, 0);
    if (needed <= 1)
        return {};
    std::wstring out (static_cast<size_t> (needed - 1), L'\0');
    MultiByteToWideChar (CP_UTF8, 0, utf8.c_str (), -1, &out[0], needed);
    return out;
}

void scaleBgraBilinearToOrder (const uint8_t* src,
                                int srcW,
                                int srcH,
                                LONG srcStride,
                                int dstW,
                                int dstH,
                                PixelOrder order,
                                std::vector<uint8_t>& outPixels)
{
    outPixels.resize (static_cast<size_t> (dstW) * static_cast<size_t> (dstH) * 4);
    const bool flipY = (srcStride < 0);
    const size_t absStride = static_cast<size_t> (srcStride < 0 ? -srcStride : srcStride);

    for (int y = 0; y < dstH; ++y)
    {
        const int sy = std::clamp ((y * srcH) / dstH, 0, srcH - 1);
        const int srcRowIdx = flipY ? (srcH - 1 - sy) : sy;
        const uint8_t* row = src + static_cast<size_t> (srcRowIdx) * absStride;
        uint8_t* dstRow = outPixels.data () + static_cast<size_t> (y) * static_cast<size_t> (dstW) * 4;

        for (int x = 0; x < dstW; ++x)
        {
            const int sx = std::clamp ((x * srcW) / dstW, 0, srcW - 1);
            const uint8_t b = row[sx * 4 + 0];
            const uint8_t g = row[sx * 4 + 1];
            const uint8_t r = row[sx * 4 + 2];
            const uint8_t a = 255;

            uint8_t* d = dstRow + static_cast<size_t> (x) * 4;
            switch (order)
            {
                case PixelOrder::BGRA: d[0] = b; d[1] = g; d[2] = r; d[3] = a; break;
                case PixelOrder::ARGB: d[0] = a; d[1] = r; d[2] = g; d[3] = b; break;
                case PixelOrder::RGBA: d[0] = r; d[1] = g; d[2] = b; d[3] = a; break;
                case PixelOrder::ABGR: d[0] = a; d[1] = b; d[2] = g; d[3] = r; break;
            }
        }
    }
}

//-----------------------------------------------------------------------------
// Windows Media Foundation Video Decoder Session
//-----------------------------------------------------------------------------
class WinVideoDecoderSession final : public IVideoDecoderSession
{
public:
    WinVideoDecoderSession () = default;
    ~WinVideoDecoderSession () override { close (); }

    bool open (const std::string& path, std::string& outError) override
    {
        close ();
        if (path.empty () || !mfInit.mfOk)
            return false;

        const std::wstring wpath = utf8ToWide (path);
        if (wpath.empty ())
        {
            outError = "Invalid UTF-8 file path";
            return false;
        }

        ComPtr<IMFAttributes> attrs;
        MFCreateAttributes (&attrs, 1);
        if (attrs)
            attrs->SetUINT32 (MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

        if (FAILED (MFCreateSourceReaderFromURL (wpath.c_str (), attrs.Get (), &reader)))
        {
            outError = "Media Foundation could not open file";
            close ();
            return false;
        }

        reader->SetStreamSelection (MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader->SetStreamSelection (MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

        ComPtr<IMFMediaType> outType;
        MFCreateMediaType (&outType);
        outType->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID (MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (FAILED (reader->SetCurrentMediaType (MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, outType.Get ())))
        {
            outError = "No supported video stream found";
            close ();
            return false;
        }

        ComPtr<IMFMediaType> curType;
        if (FAILED (reader->GetCurrentMediaType (MF_SOURCE_READER_FIRST_VIDEO_STREAM, &curType)))
        {
            outError = "Failed to query video format";
            close ();
            return false;
        }

        UINT32 w = 0, h = 0;
        MFGetAttributeSize (curType.Get (), MF_MT_FRAME_SIZE, &w, &h);
        if (w == 0 || h == 0)
        {
            outError = "Invalid video dimensions";
            close ();
            return false;
        }

        srcWidth = static_cast<int> (w);
        srcHeight = static_cast<int> (h);
        LONG strideVal = static_cast<LONG> (w * 4);
        UINT32 defStride = 0;
        if (SUCCEEDED (curType->GetUINT32 (MF_MT_DEFAULT_STRIDE, &defStride)))
            strideVal = static_cast<LONG> (static_cast<INT32> (defStride));
        srcStride = strideVal;

        double scale = 1.0;
        if (srcWidth > kMaxRenderWidth)
            scale = std::min (scale, static_cast<double> (kMaxRenderWidth) / srcWidth);
        if (srcHeight > kMaxRenderHeight)
            scale = std::min (scale, static_cast<double> (kMaxRenderHeight) / srcHeight);
        dstWidth = std::max (2, static_cast<int> (std::round (srcWidth * scale)) & ~1);
        dstHeight = std::max (2, static_cast<int> (std::round (srcHeight * scale)) & ~1);

        UINT32 num = 24, den = 1;
        if (SUCCEEDED (MFGetAttributeRatio (curType.Get (), MF_MT_FRAME_RATE, &num, &den)) && num > 0 && den > 0)
            fps = static_cast<double> (num) / static_cast<double> (den);

        PROPVARIANT var;
        PropVariantInit (&var);
        if (SUCCEEDED (reader->GetPresentationAttribute (MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var)))
        {
            ULONGLONG dur100ns = 0;
            if (SUCCEEDED (PropVariantToUInt64 (var, &dur100ns)))
                durationSec = static_cast<double> (dur100ns) / 1e7;
        }
        PropVariantClear (&var);

        eofReached = false;
        outError.clear ();
        return true;
    }

    void close () override
    {
        reader.Reset ();
        srcWidth = srcHeight = dstWidth = dstHeight = 0;
        srcStride = 0;
        fps = 24.0;
        durationSec = 0.0;
        eofReached = false;
    }

    bool isOpen () const override { return reader != nullptr; }
    int getWidth () const override { return dstWidth; }
    int getHeight () const override { return dstHeight; }
    double getFps () const override { return fps; }
    double getDurationSeconds () const override { return durationSec; }

    bool seekTo (double targetSec) override
    {
        if (!reader)
            return false;

        PROPVARIANT var;
        InitPropVariantFromInt64 (static_cast<LONGLONG> (std::max (0.0, targetSec) * 1e7), &var);
        const HRESULT hr = reader->SetCurrentPosition (GUID_NULL, var);
        PropVariantClear (&var);
        eofReached = false;
        return SUCCEEDED (hr);
    }

    bool decodeNextFrame (PixelOrder order,
                          std::vector<uint8_t>& outPixels,
                          double& outPtsSec,
                          bool& outEof) override
    {
        outEof = false;
        if (!reader || eofReached)
        {
            outEof = true;
            return false;
        }

        while (true)
        {
            DWORD streamIdx = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;

            const HRESULT hr = reader->ReadSample (
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIdx, &flags, &timestamp, &sample);
            if (FAILED (hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM))
            {
                eofReached = true;
                outEof = true;
                return false;
            }

            if (!sample)
                continue;

            ComPtr<IMFMediaBuffer> buf;
            if (FAILED (sample->ConvertToContiguousBuffer (&buf)) || !buf)
                continue;

            BYTE* data = nullptr;
            DWORD curLen = 0;
            if (SUCCEEDED (buf->Lock (&data, nullptr, &curLen)) && data && curLen > 0)
            {
                scaleBgraBilinearToOrder (data, srcWidth, srcHeight, srcStride, dstWidth, dstHeight, order, outPixels);
                buf->Unlock ();
                outPtsSec = static_cast<double> (timestamp) / 1e7;
                return true;
            }
        }
    }

private:
    ScopedMfInit mfInit;
    ComPtr<IMFSourceReader> reader;
    int srcWidth {0};
    int srcHeight {0};
    LONG srcStride {0};
    int dstWidth {0};
    int dstHeight {0};
    double fps {24.0};
    double durationSec {0.0};
    bool eofReached {false};
};

//-----------------------------------------------------------------------------
// Windows Media Foundation Audio Decoder Session
//-----------------------------------------------------------------------------
class WinAudioDecoderSession final : public IAudioDecoderSession
{
public:
    WinAudioDecoderSession () = default;
    ~WinAudioDecoderSession () override { close (); }

    bool open (const std::string& path) override
    {
        close ();
        if (path.empty () || !mfInit.mfOk)
            return false;

        const std::wstring wpath = utf8ToWide (path);
        if (wpath.empty ())
            return false;

        if (FAILED (MFCreateSourceReaderFromURL (wpath.c_str (), nullptr, &reader)))
        {
            close ();
            return false;
        }

        reader->SetStreamSelection (MF_SOURCE_READER_ALL_STREAMS, FALSE);
        if (FAILED (reader->SetStreamSelection (MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE)))
        {
            close ();
            return false;
        }

        activeOutRate = 0;
        nextExpectedPtsSec = 0.0;
        eofReached = false;
        return true;
    }

    void close () override
    {
        reader.Reset ();
        activeOutRate = 0;
        nextExpectedPtsSec = 0.0;
        eofReached = false;
    }

    bool isOpen () const override { return reader != nullptr; }

    bool seekTo (double targetSec) override
    {
        if (!reader)
            return false;

        const double preSeekSec = std::max (0.0, targetSec - 0.15);
        PROPVARIANT var;
        InitPropVariantFromInt64 (static_cast<LONGLONG> (preSeekSec * 1e7), &var);
        const HRESULT hr = reader->SetCurrentPosition (GUID_NULL, var);
        PropVariantClear (&var);
        nextExpectedPtsSec = preSeekSec;
        eofReached = false;
        return SUCCEEDED (hr);
    }

    bool readNextChunk (int targetOutRate,
                        std::vector<float>& outInterleavedStereo,
                        double& outStartSec,
                        bool& outEof) override
    {
        outEof = false;
        outInterleavedStereo.clear ();

        if (!reader || eofReached)
        {
            outEof = true;
            return false;
        }

        if (activeOutRate != targetOutRate)
        {
            ComPtr<IMFMediaType> outType;
            MFCreateMediaType (&outType);
            outType->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            outType->SetGUID (MF_MT_SUBTYPE, MFAudioFormat_Float);
            outType->SetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, 2);
            outType->SetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32> (targetOutRate));
            outType->SetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
            outType->SetUINT32 (MF_MT_AUDIO_BLOCK_ALIGNMENT, 8);
            outType->SetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32> (targetOutRate * 8));
            if (FAILED (reader->SetCurrentMediaType (MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, outType.Get ())))
            {
                eofReached = true;
                outEof = true;
                return false;
            }
            activeOutRate = targetOutRate;
        }

        while (true)
        {
            DWORD streamIdx = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;

            const HRESULT hr = reader->ReadSample (
                MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &streamIdx, &flags, &timestamp, &sample);
            if (FAILED (hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM))
            {
                eofReached = true;
                outEof = true;
                return false;
            }

            if (!sample)
                continue;

            ComPtr<IMFMediaBuffer> buf;
            if (FAILED (sample->ConvertToContiguousBuffer (&buf)) || !buf)
                continue;

            BYTE* data = nullptr;
            DWORD curLen = 0;
            if (SUCCEEDED (buf->Lock (&data, nullptr, &curLen)) && data && curLen >= sizeof (float) * 2)
            {
                const size_t numFrames = curLen / (sizeof (float) * 2);
                outInterleavedStereo.resize (numFrames * 2);
                std::memcpy (outInterleavedStereo.data (), data, numFrames * 2 * sizeof (float));
                buf->Unlock ();

                outStartSec = static_cast<double> (timestamp) / 1e7;
                nextExpectedPtsSec =
                    outStartSec + static_cast<double> (numFrames) / static_cast<double> (std::max (1, targetOutRate));
                return true;
            }
        }
    }

private:
    ScopedMfInit mfInit;
    ComPtr<IMFSourceReader> reader;
    int activeOutRate {0};
    double nextExpectedPtsSec {0.0};
    bool eofReached {false};
};

} // namespace

std::unique_ptr<IVideoDecoderSession> createVideoDecoderSession ()
{
    return std::make_unique<WinVideoDecoderSession> ();
}

std::unique_ptr<IAudioDecoderSession> createAudioDecoderSession ()
{
    return std::make_unique<WinAudioDecoderSession> ();
}

} // namespace ReasonVideo

#endif // _WIN32
