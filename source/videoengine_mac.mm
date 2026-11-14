#import <Accelerate/Accelerate.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include "videoengine_backend.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ReasonVideo {

namespace {

constexpr int kMaxRenderWidth  = 1280;
constexpr int kMaxRenderHeight = 720;

void permuteBgraToOrder (const uint8_t* src,
                         size_t srcBytesPerRow,
                         int width,
                         int height,
                         PixelOrder order,
                         std::vector<uint8_t>& outPixels)
{
    const size_t dstBytesPerRow = static_cast<size_t> (width) * 4;
    outPixels.resize (dstBytesPerRow * static_cast<size_t> (height));

    if (order == PixelOrder::BGRA)
    {
        for (int y = 0; y < height; ++y)
            std::memcpy (outPixels.data () + static_cast<size_t> (y) * dstBytesPerRow,
                         src + static_cast<size_t> (y) * srcBytesPerRow,
                         dstBytesPerRow);
        return;
    }

    // Source in memory is [B, G, R, A] (kCVPixelFormatType_32BGRA on little-endian)
    uint8_t map[4] = {0, 1, 2, 3};
    switch (order)
    {
        case PixelOrder::ARGB: map[0] = 3; map[1] = 2; map[2] = 1; map[3] = 0; break;
        case PixelOrder::RGBA: map[0] = 2; map[1] = 1; map[2] = 0; map[3] = 3; break;
        case PixelOrder::ABGR: map[0] = 3; map[1] = 0; map[2] = 1; map[3] = 2; break;
        case PixelOrder::BGRA: break;
    }

    vImage_Buffer srcBuf {};
    srcBuf.data = const_cast<uint8_t*> (src);
    srcBuf.width = static_cast<vImagePixelCount> (width);
    srcBuf.height = static_cast<vImagePixelCount> (height);
    srcBuf.rowBytes = srcBytesPerRow;

    vImage_Buffer dstBuf {};
    dstBuf.data = outPixels.data ();
    dstBuf.width = static_cast<vImagePixelCount> (width);
    dstBuf.height = static_cast<vImagePixelCount> (height);
    dstBuf.rowBytes = dstBytesPerRow;

    vImagePermuteChannels_ARGB8888 (&srcBuf, &dstBuf, map, kvImageNoFlags);
}

AVAssetTrack* loadFirstTrackSync (AVAsset* asset, AVMediaType mediaType)
{
    if (!asset)
        return nil;

    __block AVAssetTrack* resultTrack = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create (0);
    if (@available(macOS 12.0, *))
    {
        [asset loadTracksWithMediaType:mediaType
                     completionHandler:^(NSArray<AVAssetTrack*>* tracks, NSError*) {
                         if (tracks.count > 0)
                             resultTrack = tracks.firstObject;
                         dispatch_semaphore_signal (sem);
                     }];
        dispatch_semaphore_wait (sem, dispatch_time (DISPATCH_TIME_NOW, 10LL * NSEC_PER_SEC));
    }
    else
    {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        NSArray<AVAssetTrack*>* tracks = [asset tracksWithMediaType:mediaType];
        resultTrack = tracks.firstObject;
#pragma clang diagnostic pop
    }
    return resultTrack;
}

//-----------------------------------------------------------------------------
// macOS AVFoundation Video Decoder Session
//-----------------------------------------------------------------------------
class MacVideoDecoderSession final : public IVideoDecoderSession
{
public:
    ~MacVideoDecoderSession () override { close (); }

    bool open (const std::string& path, std::string& outError) override
    {
        @autoreleasepool
        {
            close ();
            if (path.empty ())
                return false;

            NSString* nsPath = [NSString stringWithUTF8String:path.c_str ()];
            if (!nsPath)
            {
                outError = "Invalid UTF-8 file path";
                return false;
            }

            NSURL* url = [NSURL fileURLWithPath:nsPath];
            NSDictionary* opts = @{ AVURLAssetPreferPreciseDurationAndTimingKey: @YES };
            AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:opts];
            if (!asset)
            {
                outError = "Failed to open media URL";
                return false;
            }

            // Load duration asynchronously with bounded timeout
            dispatch_semaphore_t durSem = dispatch_semaphore_create (0);
            [asset loadValuesAsynchronouslyForKeys:@[@"duration", @"readable"]
                                 completionHandler:^{
                                     dispatch_semaphore_signal (durSem);
                                 }];
            dispatch_semaphore_wait (durSem, dispatch_time (DISPATCH_TIME_NOW, 10LL * NSEC_PER_SEC));

            NSError* err = nil;
            if ([asset statusOfValueForKey:@"duration" error:&err] == AVKeyValueStatusFailed)
            {
                outError = err ? std::string (err.localizedDescription.UTF8String) : "Could not read media";
                close ();
                return false;
            }

            AVAssetTrack* vTrack = loadFirstTrackSync (asset, AVMediaTypeVideo);
            if (!vTrack)
            {
                outError = "No video track found (MP4/MOV/M4V supported natively)";
                close ();
                return false;
            }

            const CGSize natSize = vTrack.naturalSize;
            const CGAffineTransform xf = vTrack.preferredTransform;
            const CGSize transformed = CGSizeApplyAffineTransform (natSize, xf);
            const double rawW = std::fabs (transformed.width) > 1.0 ? std::fabs (transformed.width) : natSize.width;
            const double rawH = std::fabs (transformed.height) > 1.0 ? std::fabs (transformed.height) : natSize.height;

            if (rawW <= 0.0 || rawH <= 0.0)
            {
                outError = "Invalid video dimensions";
                close ();
                return false;
            }

            double scale = 1.0;
            if (rawW > kMaxRenderWidth)
                scale = std::min (scale, static_cast<double> (kMaxRenderWidth) / rawW);
            if (rawH > kMaxRenderHeight)
                scale = std::min (scale, static_cast<double> (kMaxRenderHeight) / rawH);

            dstWidth = std::max (2, static_cast<int> (std::round (rawW * scale)) & ~1);
            dstHeight = std::max (2, static_cast<int> (std::round (rawH * scale)) & ~1);

            const float nomRate = vTrack.nominalFrameRate;
            fps = (nomRate > 1.0f && nomRate < 240.0f) ? static_cast<double> (nomRate) : 24.0;

            const CMTime durTime = asset.duration;
            if (CMTIME_IS_VALID (durTime) && durTime.timescale > 0)
                durationSec = std::max (0.0, CMTimeGetSeconds (durTime));
            else
                durationSec = 0.0;

            videoAsset = asset;
            videoTrack = vTrack;
            hasTransform = !CGAffineTransformIsIdentity (xf);

            outError.clear ();
            return seekTo (0.0);
        }
    }

    void close () override
    {
        @autoreleasepool
        {
            if (videoReader)
                [videoReader cancelReading];
            videoOutput = nil;
            videoReader = nil;
            videoTrack = nil;
            videoAsset = nil;
            hasTransform = false;
            dstWidth = 0;
            dstHeight = 0;
            fps = 24.0;
            durationSec = 0.0;
            eofReached = false;
        }
    }

    bool isOpen () const override { return videoAsset != nil && videoTrack != nil; }
    int getWidth () const override { return dstWidth; }
    int getHeight () const override { return dstHeight; }
    double getFps () const override { return fps; }
    double getDurationSeconds () const override { return durationSec; }

    bool seekTo (double targetSec) override
    {
        @autoreleasepool
        {
            if (!videoAsset || !videoTrack)
                return false;

            if (videoReader)
            {
                [videoReader cancelReading];
                videoReader = nil;
                videoOutput = nil;
            }

            NSError* err = nil;
            AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:videoAsset error:&err];
            if (!reader || err)
                return false;

            const double clamped = std::max (0.0, targetSec);
            reader.timeRange = CMTimeRangeMake (CMTimeMakeWithSeconds (clamped, 60000), kCMTimePositiveInfinity);

            NSDictionary* outputSettings = @{
                (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
                (id)kCVPixelBufferWidthKey: @(dstWidth),
                (id)kCVPixelBufferHeightKey: @(dstHeight)
            };

            AVAssetReaderOutput* out = nil;
            if (hasTransform)
            {
                AVMutableVideoComposition* comp =
                    [AVMutableVideoComposition videoCompositionWithPropertiesOfAsset:videoAsset];
                if (comp)
                {
                    comp.renderSize = CGSizeMake (dstWidth, dstHeight);
                    AVAssetReaderVideoCompositionOutput* compOut =
                        [AVAssetReaderVideoCompositionOutput assetReaderVideoCompositionOutputWithVideoTracks:@[videoTrack]
                                                                                              videoSettings:outputSettings];
                    compOut.videoComposition = comp;
                    compOut.alwaysCopiesSampleData = NO;
                    out = compOut;
                }
            }

            if (!out)
            {
                AVAssetReaderTrackOutput* trackOut =
                    [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:videoTrack
                                                               outputSettings:outputSettings];
                trackOut.alwaysCopiesSampleData = NO;
                out = trackOut;
            }

            if (![reader canAddOutput:out])
                return false;

            [reader addOutput:out];
            if (![reader startReading])
                return false;

            videoReader = reader;
            videoOutput = out;
            eofReached = false;
            return true;
        }
    }

    bool decodeNextFrame (PixelOrder order,
                          std::vector<uint8_t>& outPixels,
                          double& outPtsSec,
                          bool& outEof) override
    {
        @autoreleasepool
        {
            outEof = false;
            if (!videoReader || !videoOutput || eofReached)
            {
                outEof = true;
                return false;
            }

            while (videoReader.status == AVAssetReaderStatusReading)
            {
                CMSampleBufferRef sample = [videoOutput copyNextSampleBuffer];
                if (!sample)
                    break;

                CVImageBufferRef imgBuf = CMSampleBufferGetImageBuffer (sample);
                if (!imgBuf)
                {
                    CFRelease (sample);
                    continue;
                }

                const CMTime pts = CMSampleBufferGetPresentationTimeStamp (sample);
                outPtsSec = CMTIME_IS_VALID (pts) ? CMTimeGetSeconds (pts) : 0.0;

                if (CVPixelBufferLockBaseAddress (imgBuf, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess)
                {
                    const size_t bufW = CVPixelBufferGetWidth (imgBuf);
                    const size_t bufH = CVPixelBufferGetHeight (imgBuf);
                    const size_t bpr = CVPixelBufferGetBytesPerRow (imgBuf);
                    const uint8_t* base = static_cast<const uint8_t*> (CVPixelBufferGetBaseAddress (imgBuf));

                    if (base && static_cast<int> (bufW) == dstWidth && static_cast<int> (bufH) == dstHeight)
                        permuteBgraToOrder (base, bpr, dstWidth, dstHeight, order, outPixels);

                    CVPixelBufferUnlockBaseAddress (imgBuf, kCVPixelBufferLock_ReadOnly);
                }

                CFRelease (sample);
                return !outPixels.empty ();
            }

            eofReached = true;
            outEof = true;
            return false;
        }
    }

private:
    AVURLAsset* videoAsset {nil};
    AVAssetTrack* videoTrack {nil};
    AVAssetReader* videoReader {nil};
    AVAssetReaderOutput* videoOutput {nil};
    bool hasTransform {false};
    int dstWidth {0};
    int dstHeight {0};
    double fps {24.0};
    double durationSec {0.0};
    bool eofReached {false};
};

//-----------------------------------------------------------------------------
// macOS AVFoundation Audio Decoder Session (native AudioConverter resampling)
//-----------------------------------------------------------------------------
class MacAudioDecoderSession final : public IAudioDecoderSession
{
public:
    ~MacAudioDecoderSession () override { close (); }

    bool open (const std::string& path) override
    {
        @autoreleasepool
        {
            close ();
            if (path.empty ())
                return false;

            NSString* nsPath = [NSString stringWithUTF8String:path.c_str ()];
            if (!nsPath)
                return false;

            NSURL* url = [NSURL fileURLWithPath:nsPath];
            NSDictionary* opts = @{ AVURLAssetPreferPreciseDurationAndTimingKey: @YES };
            AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:opts];
            if (!asset)
                return false;

            AVAssetTrack* aTrack = loadFirstTrackSync (asset, AVMediaTypeAudio);
            if (!aTrack)
                return false;

            audioAsset = asset;
            audioTrack = aTrack;
            pendingSeekSec = 0.0;
            nextExpectedAudioPtsSec = 0.0;
            needsReaderReset = true;
            activeReaderOutRate = 0;
            eofReached = false;
            return true;
        }
    }

    void close () override
    {
        @autoreleasepool
        {
            if (audioReader)
                [audioReader cancelReading];
            audioOutput = nil;
            audioReader = nil;
            audioTrack = nil;
            audioAsset = nil;
            activeReaderOutRate = 0;
            pendingSeekSec = 0.0;
            nextExpectedAudioPtsSec = 0.0;
            needsReaderReset = true;
            eofReached = false;
        }
    }

    bool isOpen () const override { return audioAsset != nil && audioTrack != nil; }

    bool seekTo (double targetSec) override
    {
        @autoreleasepool
        {
            if (!audioAsset || !audioTrack)
                return false;

            if (audioReader)
            {
                [audioReader cancelReading];
                audioReader = nil;
                audioOutput = nil;
            }

            pendingSeekSec = std::max (0.0, targetSec);
            nextExpectedAudioPtsSec = pendingSeekSec;
            needsReaderReset = true;
            activeReaderOutRate = 0;
            eofReached = false;
            return true;
        }
    }

    bool readNextChunk (int targetOutRate,
                        std::vector<float>& outInterleavedStereo,
                        double& outStartSec,
                        bool& outEof) override
    {
        @autoreleasepool
        {
            outEof = false;
            outInterleavedStereo.clear ();

            if (!audioAsset || !audioTrack)
            {
                outEof = true;
                return false;
            }

            if (needsReaderReset || !audioReader || activeReaderOutRate != targetOutRate)
            {
                const double startSec = needsReaderReset ? pendingSeekSec : nextExpectedAudioPtsSec;
                if (!startReaderAt (startSec, targetOutRate))
                {
                    eofReached = true;
                    outEof = true;
                    return false;
                }
            }

            if (eofReached || !audioReader || !audioOutput)
            {
                outEof = true;
                return false;
            }

            while (audioReader.status == AVAssetReaderStatusReading)
            {
                CMSampleBufferRef sample = [audioOutput copyNextSampleBuffer];
                if (!sample)
                    break;

                AudioBufferList abl {};
                CMBlockBufferRef blockBuf = nullptr;
                const OSStatus st = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer (
                    sample,
                    nullptr,
                    &abl,
                    sizeof (abl),
                    nullptr,
                    nullptr,
                    kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
                    &blockBuf);

                if (st == noErr && abl.mNumberBuffers > 0 && abl.mBuffers[0].mData && abl.mBuffers[0].mDataByteSize > 0)
                {
                    const size_t numFloats = abl.mBuffers[0].mDataByteSize / sizeof (float);
                    const size_t numFrames = numFloats / 2;
                    if (numFrames > 0)
                    {
                        outInterleavedStereo.resize (numFrames * 2);
                        std::memcpy (outInterleavedStereo.data (), abl.mBuffers[0].mData, numFrames * 2 * sizeof (float));

                        const CMTime pts = CMSampleBufferGetPresentationTimeStamp (sample);
                        outStartSec = CMTIME_IS_VALID (pts) ? CMTimeGetSeconds (pts) : nextExpectedAudioPtsSec;
                        nextExpectedAudioPtsSec =
                            outStartSec + static_cast<double> (numFrames) / static_cast<double> (std::max (1, targetOutRate));

                        if (blockBuf)
                            CFRelease (blockBuf);
                        CFRelease (sample);
                        return true;
                    }
                }

                if (blockBuf)
                    CFRelease (blockBuf);
                CFRelease (sample);
            }

            eofReached = true;
            outEof = true;
            return false;
        }
    }

private:
    bool startReaderAt (double targetSec, int outRate)
    {
        if (audioReader)
        {
            [audioReader cancelReading];
            audioReader = nil;
            audioOutput = nil;
        }

        NSError* err = nil;
        AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:audioAsset error:&err];
        if (!reader || err)
            return false;

        // Pre-seek ~150ms so AAC/MP3 decoder priming stabilizes before targetSec
        const double preSeekSec = std::max (0.0, targetSec - 0.15);
        reader.timeRange = CMTimeRangeMake (CMTimeMakeWithSeconds (preSeekSec, 60000), kCMTimePositiveInfinity);

        NSDictionary* audioSettings = @{
            AVFormatIDKey: @(kAudioFormatLinearPCM),
            AVSampleRateKey: @(outRate),
            AVNumberOfChannelsKey: @(2),
            AVLinearPCMBitDepthKey: @(32),
            AVLinearPCMIsFloatKey: @YES,
            AVLinearPCMIsBigEndianKey: @NO,
            AVLinearPCMIsNonInterleaved: @NO
        };

        AVAssetReaderTrackOutput* out =
            [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:audioTrack
                                                       outputSettings:audioSettings];
        if (!out || ![reader canAddOutput:out])
            return false;

        out.alwaysCopiesSampleData = NO;
        [reader addOutput:out];
        if (![reader startReading])
            return false;

        audioReader = reader;
        audioOutput = out;
        activeReaderOutRate = outRate;
        pendingSeekSec = targetSec;
        nextExpectedAudioPtsSec = preSeekSec;
        needsReaderReset = false;
        eofReached = false;
        return true;
    }

    AVURLAsset* audioAsset {nil};
    AVAssetTrack* audioTrack {nil};
    AVAssetReader* audioReader {nil};
    AVAssetReaderTrackOutput* audioOutput {nil};
    int activeReaderOutRate {0};
    double pendingSeekSec {0.0};
    double nextExpectedAudioPtsSec {0.0};
    bool needsReaderReset {true};
    bool eofReached {false};
};

} // namespace

std::unique_ptr<IVideoDecoderSession> createVideoDecoderSession ()
{
    return std::make_unique<MacVideoDecoderSession> ();
}

std::unique_ptr<IAudioDecoderSession> createAudioDecoderSession ()
{
    return std::make_unique<MacAudioDecoderSession> ();
}

} // namespace ReasonVideo
