#include "videoengine.h"
#include "videoengine_backend.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace ReasonVideo {

namespace {

constexpr int kFadeInSamples = 128; // ~2.7ms @ 48kHz to prevent start/seek clicks

//-----------------------------------------------------------------------------
// Platform-independent wrapper around native video decoder backend
//-----------------------------------------------------------------------------
struct VideoDecodeSession
{
    std::unique_ptr<IVideoDecoderSession> backend {createVideoDecoderSession ()};
    int dstWidth {0};
    int dstHeight {0};
    double fps {24.0};
    double durationSec {0.0};
    double lastDecodedVideoPts {-1000.0};
    PixelOrder lastOrder {PixelOrder::ARGB};
    bool hasPublishedOrder {false};

    std::vector<uint8_t> stagingBuffer;
    std::vector<uint8_t> tempBuffer;

    bool isOpen () const { return backend && backend->isOpen (); }

    void close ()
    {
        if (backend)
            backend->close ();
        dstWidth = dstHeight = 0;
        fps = 24.0;
        durationSec = 0.0;
        lastDecodedVideoPts = -1000.0;
        hasPublishedOrder = false;
        stagingBuffer.clear ();
        tempBuffer.clear ();
    }

    bool open (const std::string& path, std::string& outError)
    {
        close ();
        if (!backend || path.empty ())
            return false;

        if (!backend->open (path, outError))
        {
            close ();
            return false;
        }

        dstWidth = backend->getWidth ();
        dstHeight = backend->getHeight ();
        fps = backend->getFps ();
        durationSec = backend->getDurationSeconds ();
        lastDecodedVideoPts = -1000.0;
        hasPublishedOrder = false;
        return dstWidth > 0 && dstHeight > 0;
    }

    bool seekTo (double targetSec)
    {
        if (!isOpen ())
            return false;
        lastDecodedVideoPts = -1000.0;
        return backend->seekTo (targetSec);
    }

    bool decodeNextFrame (PixelOrder order, bool& outEof)
    {
        outEof = false;
        if (!isOpen ())
        {
            outEof = true;
            return false;
        }

        double ptsSec = 0.0;
        if (!backend->decodeNextFrame (order, tempBuffer, ptsSec, outEof))
            return false;

        lastDecodedVideoPts = ptsSec;
        lastOrder = order;
        hasPublishedOrder = true;
        stagingBuffer.swap (tempBuffer);
        return true;
    }
};

//-----------------------------------------------------------------------------
// Platform-independent wrapper around native audio decoder backend
//-----------------------------------------------------------------------------
struct AudioDecodeSession
{
    std::unique_ptr<IAudioDecoderSession> backend {createAudioDecoderSession ()};
    bool openFlag {false};
    bool eofReached {false};

    std::vector<float> pendingPcm;
    size_t pendingFrameOffset {0};
    double pendingStartSec {0.0};

    bool isOpen () const { return openFlag && backend && backend->isOpen (); }

    void close ()
    {
        if (backend)
            backend->close ();
        openFlag = false;
        eofReached = false;
        pendingPcm.clear ();
        pendingFrameOffset = 0;
        pendingStartSec = 0.0;
    }

    bool open (const std::string& path)
    {
        close ();
        if (!backend || path.empty ())
            return false;

        if (!backend->open (path))
        {
            close ();
            return false;
        }

        openFlag = true;
        eofReached = false;
        return true;
    }

    bool seekTo (double targetSec)
    {
        if (!isOpen ())
            return false;

        eofReached = false;
        pendingPcm.clear ();
        pendingFrameOffset = 0;
        pendingStartSec = 0.0;
        return backend->seekTo (targetSec);
    }

    bool decodeNextChunk (int targetOutRate)
    {
        if (!isOpen () || eofReached)
            return false;

        pendingPcm.clear ();
        pendingFrameOffset = 0;

        bool outEof = false;
        double startSec = 0.0;
        if (!backend->readNextChunk (targetOutRate, pendingPcm, startSec, outEof))
        {
            eofReached = true;
            pendingPcm.clear ();
            return false;
        }

        pendingStartSec = startSec;
        pendingFrameOffset = 0;
        return !pendingPcm.empty ();
    }
};

} // namespace

//-----------------------------------------------------------------------------
VideoEngine::VideoEngine ()
{
    audioRings[0].pcm.assign (kAudioRingFrames * 2, 0.0f);
    audioRings[1].pcm.assign (kAudioRingFrames * 2, 0.0f);

    videoThread = std::thread (&VideoEngine::videoWorkerLoop, this);
    audioThread = std::thread (&VideoEngine::audioWorkerLoop, this);
}

//-----------------------------------------------------------------------------
VideoEngine::~VideoEngine ()
{
    stopRequested.store (true, std::memory_order_release);
    videoCv.notify_all ();
    audioCv.notify_all ();
    if (videoThread.joinable ())
        videoThread.join ();
    if (audioThread.joinable ())
        audioThread.join ();
}

//-----------------------------------------------------------------------------
void VideoEngine::openFile (const std::string& path)
{
    {
        std::lock_guard<std::mutex> lock (commandMutex);
        desiredFilePath = path;
        fileCommandSeq.fetch_add (1, std::memory_order_release);
    }
    activeAudioRing.store (-1, std::memory_order_release);
    audioSeekRequested.store (true, std::memory_order_release);
    videoCv.notify_all ();
    audioCv.notify_all ();
}

//-----------------------------------------------------------------------------
void VideoEngine::clearFile ()
{
    openFile ("");
}

//-----------------------------------------------------------------------------
std::string VideoEngine::getFilePath () const
{
    std::lock_guard<std::mutex> lock (commandMutex);
    return desiredFilePath;
}

//-----------------------------------------------------------------------------
void VideoEngine::setOffsetSeconds (double offsetSec)
{
    offsetSeconds.store (offsetSec, std::memory_order_relaxed);
    transportGeneration.fetch_add (1, std::memory_order_release);
    videoCv.notify_all ();
    audioCv.notify_all ();
}

//-----------------------------------------------------------------------------
double VideoEngine::getOffsetSeconds () const
{
    return offsetSeconds.load (std::memory_order_relaxed);
}

//-----------------------------------------------------------------------------
void VideoEngine::setPixelOrder (PixelOrder order)
{
    int prev = requestedPixelOrder.exchange (static_cast<int> (order), std::memory_order_relaxed);
    if (prev != static_cast<int> (order))
        videoCv.notify_all ();
}

//-----------------------------------------------------------------------------
void VideoEngine::updateTransport (bool isPlaying,
                                   double dawTimeSec,
                                   double sampleRate,
                                   double tempo,
                                   double projectTimeMusic,
                                   bool hasMusicTime)
{
    const bool prevPlaying = transportPlaying.load (std::memory_order_relaxed);
    const double prevTime = transportDawTimeSec.load (std::memory_order_relaxed);

    transportPlaying.store (isPlaying, std::memory_order_relaxed);
    transportDawTimeSec.store (dawTimeSec, std::memory_order_relaxed);
    if (sampleRate > 0.0)
        transportSampleRate.store (sampleRate, std::memory_order_relaxed);
    if (tempo > 0.0)
        transportTempo.store (tempo, std::memory_order_relaxed);
    transportMusicPos.store (projectTimeMusic, std::memory_order_relaxed);
    transportHasMusic.store (hasMusicTime, std::memory_order_relaxed);

    if (prevPlaying != isPlaying || std::fabs (dawTimeSec - prevTime) > 0.002)
    {
        transportGeneration.fetch_add (1, std::memory_order_release);
        videoCv.notify_all ();
        audioCv.notify_all ();
    }
}

//-----------------------------------------------------------------------------
bool VideoEngine::readAudioSamples (float* outLeft,
                                    float* outRight,
                                    int32_t numSamples,
                                    double dawTimeSec,
                                    double sampleRate,
                                    float volume,
                                    bool muted)
{
    if (!hasAudioStream.load (std::memory_order_acquire) || muted || volume <= 0.0001f ||
        !transportPlaying.load (std::memory_order_relaxed) || sampleRate <= 0.0 || numSamples <= 0)
    {
        audioWasOutputting = false;
        return false;
    }

    const double targetVideoSec = dawTimeSec + offsetSeconds.load (std::memory_order_relaxed);
    if (targetVideoSec < 0.0)
    {
        audioWasOutputting = false;
        return false;
    }

    // Acquire active ping-pong ring wait-free
    int idx = activeAudioRing.load (std::memory_order_acquire);
    bool acquired = false;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        if (idx < 0)
            break;

        readerInAudioRing.store (idx, std::memory_order_seq_cst);
        const int confirmIdx = activeAudioRing.load (std::memory_order_seq_cst);
        if (confirmIdx == idx)
        {
            acquired = true;
            break;
        }
        idx = confirmIdx;
    }

    if (!acquired || idx < 0)
    {
        readerInAudioRing.store (-1, std::memory_order_release);
        audioWasOutputting = false;
        return false;
    }

    AudioRing& ring = audioRings[idx];
    if (!ring.valid.load (std::memory_order_acquire) || std::fabs (ring.sampleRate - sampleRate) > 1.0)
    {
        readerInAudioRing.store (-1, std::memory_order_release);
        audioSeekTargetSec.store (targetVideoSec, std::memory_order_relaxed);
        audioSeekRequested.store (true, std::memory_order_release);
        audioCv.notify_one ();
        audioWasOutputting = false;
        return false;
    }

    uint64_t r = ring.readFrame.load (std::memory_order_acquire);
    const uint64_t w = ring.writeFrame.load (std::memory_order_acquire);

    const double ringHeadVideoSec = ring.startVideoSec + static_cast<double> (r) / sampleRate;
    const double ringTailVideoSec = ring.startVideoSec + static_cast<double> (w) / sampleRate;
    const double driftSec = targetVideoSec - ringHeadVideoSec;

    bool skippedForward = false;

    // Tolerance of 20ms ignores minor DAW clock jitter; larger positive jumps fast-forward inside ring
    if (driftSec > 0.020)
    {
        const double neededEndSec = targetVideoSec + static_cast<double> (numSamples) / sampleRate;
        if (neededEndSec <= ringTailVideoSec)
        {
            const uint64_t skipFrames = static_cast<uint64_t> (std::llround (driftSec * sampleRate));
            if (r + skipFrames + static_cast<uint64_t> (numSamples) <= w)
            {
                r += skipFrames;
                skippedForward = true;
            }
        }
        else
        {
            // Jumped past buffered lookahead: request audio worker seek
            readerInAudioRing.store (-1, std::memory_order_release);
            audioSeekTargetSec.store (targetVideoSec, std::memory_order_relaxed);
            audioSeekRequested.store (true, std::memory_order_release);
            audioCv.notify_one ();
            audioWasOutputting = false;
            return false;
        }
    }
    else if (driftSec < -0.040)
    {
        // Loop jump or rewind behind ring head: request audio worker seek
        readerInAudioRing.store (-1, std::memory_order_release);
        audioSeekTargetSec.store (targetVideoSec, std::memory_order_relaxed);
        audioSeekRequested.store (true, std::memory_order_release);
        audioCv.notify_one ();
        audioWasOutputting = false;
        return false;
    }

    const uint64_t avail = (w >= r) ? (w - r) : 0;
    if (avail < static_cast<uint64_t> (numSamples))
    {
        readerInAudioRing.store (-1, std::memory_order_release);
        audioCv.notify_one ();
        audioWasOutputting = false;
        return false;
    }

    if (!audioWasOutputting || skippedForward || lastReaderRingIdx != idx)
        fadeInSamplesRemaining = kFadeInSamples;

    for (int32_t i = 0; i < numSamples; ++i)
    {
        float ramp = 1.0f;
        if (fadeInSamplesRemaining > 0)
        {
            ramp = 1.0f - static_cast<float> (fadeInSamplesRemaining) / static_cast<float> (kFadeInSamples);
            --fadeInSamplesRemaining;
        }
        const size_t pcmIdx = static_cast<size_t> ((r + static_cast<uint64_t> (i)) % kAudioRingFrames) * 2;
        outLeft[i] = ring.pcm[pcmIdx] * volume * ramp;
        outRight[i] = ring.pcm[pcmIdx + 1] * volume * ramp;
    }

    ring.readFrame.store (r + static_cast<uint64_t> (numSamples), std::memory_order_release);
    lastReaderRingIdx = idx;
    audioWasOutputting = true;

    readerInAudioRing.store (-1, std::memory_order_release);
    return true;
}

//-----------------------------------------------------------------------------
TransportSnapshot VideoEngine::getTransportSnapshot () const
{
    TransportSnapshot snap;
    snap.isPlaying = transportPlaying.load (std::memory_order_relaxed);
    snap.dawTimeSec = transportDawTimeSec.load (std::memory_order_relaxed);
    snap.sampleRate = transportSampleRate.load (std::memory_order_relaxed);
    snap.tempo = transportTempo.load (std::memory_order_relaxed);
    snap.projectTimeMusic = transportMusicPos.load (std::memory_order_relaxed);
    snap.hasMusicTime = transportHasMusic.load (std::memory_order_relaxed);
    return snap;
}

//-----------------------------------------------------------------------------
bool VideoEngine::copyLatestFrameIfChanged (uint64_t& lastSeenSequence, VideoFrameSnapshot& outFrame)
{
    std::lock_guard<std::mutex> lock (frameMutex);
    if (publishedFrame.frameSequence == lastSeenSequence)
        return false;

    lastSeenSequence = publishedFrame.frameSequence;
    outFrame = publishedFrame;
    return true;
}

//-----------------------------------------------------------------------------
void VideoEngine::getStatus (bool& outHasVideo,
                             double& outDurationSec,
                             double& outFps,
                             double& outVideoPtsSec,
                             bool& outOfRange,
                             std::string& outFilePath,
                             std::string& outError) const
{
    std::lock_guard<std::mutex> lock (frameMutex);
    outHasVideo = publishedFrame.hasVideo;
    outDurationSec = publishedFrame.durationSec;
    outFps = publishedFrame.fps;
    outVideoPtsSec = publishedFrame.videoPtsSec;
    outOfRange = publishedFrame.outOfRange;
    outFilePath = publishedFrame.filePath;
    outError = publishedFrame.errorMessage;
}

//-----------------------------------------------------------------------------
void VideoEngine::videoWorkerLoop ()
{
    VideoDecodeSession session;
    std::string currentFilePath;
    uint64_t lastSeenFileSeq = 0;
    double lastPublishedTargetSec = -9999.0;

    auto publishErrorOrEmpty = [&] (const std::string& path, const std::string& err) {
        std::lock_guard<std::mutex> lock (frameMutex);
        publishedFrame.frameSequence++;
        publishedFrame.hasVideo = false;
        publishedFrame.outOfRange = false;
        publishedFrame.width = 0;
        publishedFrame.height = 0;
        publishedFrame.strideBytes = 0;
        publishedFrame.pixels.clear ();
        publishedFrame.durationSec = 0.0;
        publishedFrame.fps = 24.0;
        publishedFrame.videoPtsSec = 0.0;
        publishedFrame.filePath = path;
        publishedFrame.errorMessage = err;
    };

    auto publishCurrentFrame = [&] (double ptsSec, bool outOfRange, const std::string& path, PixelOrder order) {
        std::lock_guard<std::mutex> lock (frameMutex);
        publishedFrame.frameSequence++;
        publishedFrame.hasVideo = true;
        publishedFrame.outOfRange = outOfRange;
        publishedFrame.width = session.dstWidth;
        publishedFrame.height = session.dstHeight;
        publishedFrame.strideBytes = session.dstWidth * 4;
        publishedFrame.pixelOrder = order;
        if (!outOfRange && !session.stagingBuffer.empty ())
            publishedFrame.pixels = session.stagingBuffer;
        publishedFrame.durationSec = session.durationSec;
        publishedFrame.fps = session.fps;
        publishedFrame.videoPtsSec = ptsSec;
        publishedFrame.filePath = path;
        publishedFrame.errorMessage.clear ();
    };

    while (!stopRequested.load (std::memory_order_acquire))
    {
        const uint64_t seq = fileCommandSeq.load (std::memory_order_acquire);
        if (seq != lastSeenFileSeq)
        {
            {
                std::lock_guard<std::mutex> lock (commandMutex);
                currentFilePath = desiredFilePath;
                lastSeenFileSeq = fileCommandSeq.load (std::memory_order_acquire);
            }
            lastPublishedTargetSec = -9999.0;

            if (currentFilePath.empty ())
            {
                session.close ();
                publishErrorOrEmpty ("", "");
            }
            else
            {
                std::string err;
                if (!session.open (currentFilePath, err))
                    publishErrorOrEmpty (currentFilePath, err);
            }
        }

        if (!session.isOpen ())
        {
            std::unique_lock<std::mutex> lk (videoCvMutex);
            videoCv.wait_for (lk, std::chrono::milliseconds (50), [&] {
                return stopRequested.load (std::memory_order_acquire) ||
                       fileCommandSeq.load (std::memory_order_acquire) != lastSeenFileSeq;
            });
            continue;
        }

        const bool isPlaying = transportPlaying.load (std::memory_order_relaxed);
        const double dawTimeSec = transportDawTimeSec.load (std::memory_order_relaxed);
        const double targetSec = dawTimeSec + offsetSeconds.load (std::memory_order_relaxed);
        const PixelOrder order = static_cast<PixelOrder> (requestedPixelOrder.load (std::memory_order_relaxed));

        const double frameDuration = (session.fps > 1.0) ? (1.0 / session.fps) : (1.0 / 24.0);
        const bool orderChanged = (!session.hasPublishedOrder || order != session.lastOrder);
        const bool needDecode =
            (lastPublishedTargetSec < -1000.0) ||
            (std::fabs (targetSec - lastPublishedTargetSec) >= frameDuration * 0.45) ||
            orderChanged;

        if (needDecode)
        {
            if (targetSec < 0.0 || (session.durationSec > 0.0 && targetSec > session.durationSec + 0.05))
            {
                lastPublishedTargetSec = targetSec;
                publishCurrentFrame (targetSec, true, currentFilePath, order);
            }
            else
            {
                const double clampedTarget = std::max (0.0, targetSec);
                const bool needSeek =
                    orderChanged ||
                    (session.lastDecodedVideoPts < -500.0) ||
                    (clampedTarget < session.lastDecodedVideoPts - frameDuration * 0.5) ||
                    (clampedTarget - session.lastDecodedVideoPts > 0.5);

                if (needSeek)
                    session.seekTo (clampedTarget);

                bool gotFrame = false;
                bool decodedTarget = false;
                const uint64_t decodeStartGen = transportGeneration.load (std::memory_order_acquire);

                while (!stopRequested.load (std::memory_order_acquire) && !decodedTarget)
                {
                    if (fileCommandSeq.load (std::memory_order_acquire) != lastSeenFileSeq)
                        break;

                    if (!isPlaying && transportGeneration.load (std::memory_order_acquire) != decodeStartGen)
                    {
                        const double newestTarget =
                            transportDawTimeSec.load (std::memory_order_relaxed) +
                            offsetSeconds.load (std::memory_order_relaxed);
                        if (std::fabs (newestTarget - clampedTarget) > 0.15)
                            break;
                    }

                    bool eof = false;
                    if (session.decodeNextFrame (order, eof))
                    {
                        gotFrame = true;
                        if (session.lastDecodedVideoPts + frameDuration * 0.5 >= clampedTarget)
                        {
                            publishCurrentFrame (session.lastDecodedVideoPts, false, currentFilePath, order);
                            lastPublishedTargetSec = session.lastDecodedVideoPts;
                            decodedTarget = true;
                        }
                        continue;
                    }

                    if (eof)
                    {
                        if (gotFrame)
                        {
                            publishCurrentFrame (session.lastDecodedVideoPts, false, currentFilePath, order);
                            lastPublishedTargetSec = clampedTarget;
                        }
                        break;
                    }
                }

                if (gotFrame && !decodedTarget)
                {
                    publishCurrentFrame (session.lastDecodedVideoPts, false, currentFilePath, order);
                    lastPublishedTargetSec = clampedTarget;
                }
            }
        }

        std::unique_lock<std::mutex> lk (videoCvMutex);
        const auto waitMs = isPlaying ? std::chrono::milliseconds (6) : std::chrono::milliseconds (30);
        const uint64_t currentGen = transportGeneration.load (std::memory_order_acquire);
        videoCv.wait_for (lk, waitMs, [&] {
            return stopRequested.load (std::memory_order_acquire) ||
                   fileCommandSeq.load (std::memory_order_acquire) != lastSeenFileSeq ||
                   transportGeneration.load (std::memory_order_acquire) != currentGen;
        });
    }
}

//-----------------------------------------------------------------------------
void VideoEngine::audioWorkerLoop ()
{
    AudioDecodeSession session;
    std::string currentFilePath;
    uint64_t lastSeenFileSeq = 0;
    double lastStoppedPrebufferTarget = -9999.0;

    auto writeFramesToRing = [] (AudioRing& ring, const float* interleaved, size_t numFrames) {
        if (numFrames == 0)
            return;
        const uint64_t w = ring.writeFrame.load (std::memory_order_relaxed);
        for (size_t i = 0; i < numFrames; ++i)
        {
            const size_t idx = static_cast<size_t> ((w + i) % kAudioRingFrames) * 2;
            float l = interleaved[i * 2];
            float r = interleaved[i * 2 + 1];
            if (!std::isfinite (l)) l = 0.0f;
            if (!std::isfinite (r)) r = 0.0f;
            ring.pcm[idx] = l;
            ring.pcm[idx + 1] = r;
        }
        ring.writeFrame.store (w + numFrames, std::memory_order_release);
    };

    auto writeSilenceToRing = [] (AudioRing& ring, size_t numFrames) {
        if (numFrames == 0)
            return;
        const uint64_t w = ring.writeFrame.load (std::memory_order_relaxed);
        for (size_t i = 0; i < numFrames; ++i)
        {
            const size_t idx = static_cast<size_t> ((w + i) % kAudioRingFrames) * 2;
            ring.pcm[idx] = 0.0f;
            ring.pcm[idx + 1] = 0.0f;
        }
        ring.writeFrame.store (w + numFrames, std::memory_order_release);
    };

    auto pushPendingAligned = [&] (AudioRing& ring,
                                   double outRate,
                                   double initialTargetSec,
                                   bool& ringInitialized,
                                   bool& hitLargeGap) -> bool {
        const size_t totalPendingFrames = session.pendingPcm.size () / 2;
        if (session.pendingFrameOffset >= totalPendingFrames)
            return true;

        const size_t remFrames = totalPendingFrames - session.pendingFrameOffset;
        const double chunkStartSec =
            session.pendingStartSec + static_cast<double> (session.pendingFrameOffset) / outRate;
        const double chunkEndSec = chunkStartSec + static_cast<double> (remFrames) / outRate;

        const uint64_t r = ring.readFrame.load (std::memory_order_acquire);
        const uint64_t w = ring.writeFrame.load (std::memory_order_relaxed);
        const uint64_t freeFrames = (kAudioRingFrames > (w - r)) ? (kAudioRingFrames - (w - r)) : 0;

        if (!ringInitialized)
        {
            if (chunkEndSec <= initialTargetSec)
            {
                // Chunk entirely before targetSec (warmup frame): discard cleanly
                session.pendingFrameOffset = totalPendingFrames;
                return true;
            }

            size_t startOffset = 0;
            if (chunkStartSec < initialTargetSec)
            {
                const int64_t skip = static_cast<int64_t> (std::llround ((initialTargetSec - chunkStartSec) * outRate));
                startOffset = static_cast<size_t> (std::clamp<int64_t> (skip, 0, static_cast<int64_t> (remFrames)));
                ring.startVideoSec = chunkStartSec + static_cast<double> (startOffset) / outRate;
            }
            else
            {
                const double gap = chunkStartSec - initialTargetSec;
                if (gap > 0.005 && gap < 2.0)
                {
                    const size_t sil = std::min<size_t> (
                        static_cast<size_t> (std::llround (gap * outRate)),
                        static_cast<size_t> (freeFrames));
                    ring.startVideoSec = initialTargetSec;
                    writeSilenceToRing (ring, sil);
                    ringInitialized = true;
                    // Recompute free frames after silence
                }
                else
                {
                    ring.startVideoSec = chunkStartSec;
                }
            }

            ringInitialized = true;
            const uint64_t rNow = ring.readFrame.load (std::memory_order_acquire);
            const uint64_t wNow = ring.writeFrame.load (std::memory_order_relaxed);
            const uint64_t availFree = (kAudioRingFrames > (wNow - rNow)) ? (kAudioRingFrames - (wNow - rNow)) : 0;
            const size_t writable = std::min<size_t> (remFrames - startOffset, static_cast<size_t> (availFree));
            const float* srcPtr = session.pendingPcm.data () + (session.pendingFrameOffset + startOffset) * 2;
            writeFramesToRing (ring, srcPtr, writable);
            session.pendingFrameOffset += startOffset + writable;
            return session.pendingFrameOffset >= totalPendingFrames;
        }

        // Subsequent sequential chunks into an already-started ring
        const double expectedNextSec = ring.startVideoSec + static_cast<double> (w) / outRate;
        const double gap = chunkStartSec - expectedNextSec;

        if (gap <= -0.025)
        {
            const int64_t overlap = static_cast<int64_t> (std::llround (-gap * outRate));
            if (overlap >= static_cast<int64_t> (remFrames))
            {
                session.pendingFrameOffset = totalPendingFrames;
                return true;
            }
            session.pendingFrameOffset += static_cast<size_t> (overlap);
        }
        else if (gap >= 2.0)
        {
            hitLargeGap = true;
            return false;
        }
        else if (gap >= 0.025)
        {
            const size_t silNeeded = static_cast<size_t> (std::llround (gap * outRate));
            const size_t silToWrite = std::min<size_t> (silNeeded, static_cast<size_t> (freeFrames));
            writeSilenceToRing (ring, silToWrite);
            if (silToWrite < silNeeded)
                return false; // Ring full; finish gap on next pass
        }

        const uint64_t rNow = ring.readFrame.load (std::memory_order_acquire);
        const uint64_t wNow = ring.writeFrame.load (std::memory_order_relaxed);
        const uint64_t availFree = (kAudioRingFrames > (wNow - rNow)) ? (kAudioRingFrames - (wNow - rNow)) : 0;
        const size_t leftFrames = totalPendingFrames - session.pendingFrameOffset;
        const size_t toWrite = std::min<size_t> (leftFrames, static_cast<size_t> (availFree));
        if (toWrite == 0)
            return false;

        const float* srcPtr = session.pendingPcm.data () + session.pendingFrameOffset * 2;
        writeFramesToRing (ring, srcPtr, toWrite);
        session.pendingFrameOffset += toWrite;
        return session.pendingFrameOffset >= totalPendingFrames;
    };

    bool activeRingInitialized = false;
    double activeInitialTarget = 0.0;

    while (!stopRequested.load (std::memory_order_acquire))
    {
        const uint64_t seq = fileCommandSeq.load (std::memory_order_acquire);
        if (seq != lastSeenFileSeq)
        {
            {
                std::lock_guard<std::mutex> lock (commandMutex);
                currentFilePath = desiredFilePath;
                lastSeenFileSeq = fileCommandSeq.load (std::memory_order_acquire);
            }

            activeAudioRing.store (-1, std::memory_order_release);
            hasAudioStream.store (false, std::memory_order_release);
            activeRingInitialized = false;
            lastStoppedPrebufferTarget = -9999.0;

            if (currentFilePath.empty () || !session.open (currentFilePath))
            {
                session.close ();
            }
            else
            {
                hasAudioStream.store (true, std::memory_order_release);
                audioSeekRequested.store (true, std::memory_order_release);
            }
        }

        if (!session.isOpen ())
        {
            std::unique_lock<std::mutex> lk (audioCvMutex);
            audioCv.wait_for (lk, std::chrono::milliseconds (50), [&] {
                return stopRequested.load (std::memory_order_acquire) ||
                       fileCommandSeq.load (std::memory_order_acquire) != lastSeenFileSeq;
            });
            continue;
        }

        const bool isPlaying = transportPlaying.load (std::memory_order_relaxed);
        const double dawTimeSec = transportDawTimeSec.load (std::memory_order_relaxed);
        const double offsetSec = offsetSeconds.load (std::memory_order_relaxed);
        const double outRate = std::max (8000.0, transportSampleRate.load (std::memory_order_relaxed));
        const int outRateInt = static_cast<int> (std::llround (outRate));
        const double currentTargetSec = std::max (0.0, dawTimeSec + offsetSec);

        int curIdx = activeAudioRing.load (std::memory_order_acquire);

        // If stopped and timeline moved/rewound away from active ring head, prebuffer new position
        if (!isPlaying)
        {
            bool needStoppedPrebuffer = (curIdx < 0);
            if (!needStoppedPrebuffer && std::fabs (currentTargetSec - lastStoppedPrebufferTarget) > 0.05)
                needStoppedPrebuffer = true;
            if (!needStoppedPrebuffer && curIdx >= 0)
            {
                const uint64_t r = audioRings[curIdx].readFrame.load (std::memory_order_acquire);
                const double headSec =
                    audioRings[curIdx].startVideoSec + static_cast<double> (r) / audioRings[curIdx].sampleRate;
                if (std::fabs (headSec - currentTargetSec) > 0.05)
                    needStoppedPrebuffer = true;
            }
            if (needStoppedPrebuffer)
            {
                audioSeekTargetSec.store (currentTargetSec, std::memory_order_relaxed);
                audioSeekRequested.store (true, std::memory_order_release);
                lastStoppedPrebufferTarget = currentTargetSec;
            }
        }

        const bool sampleRateChanged =
            (curIdx >= 0) && (std::fabs (audioRings[curIdx].sampleRate - outRate) > 1.0);

        if (audioSeekRequested.load (std::memory_order_acquire) || curIdx < 0 || sampleRateChanged)
        {
            const bool explicitSeek = audioSeekRequested.exchange (false, std::memory_order_acq_rel);
            double targetSec = explicitSeek ? audioSeekTargetSec.load (std::memory_order_relaxed) : currentTargetSec;
            if (!isPlaying)
                targetSec = currentTargetSec;
            targetSec = std::max (0.0, targetSec);

            const int nextIdx = (curIdx == 0) ? 1 : 0;

            // If reader was reading nextIdx before a prior swap, wait briefly until it releases nextIdx
            while (readerInAudioRing.load (std::memory_order_seq_cst) == nextIdx &&
                   !stopRequested.load (std::memory_order_acquire))
            {
                std::this_thread::yield ();
            }

            AudioRing& nextRing = audioRings[nextIdx];
            nextRing.valid.store (false, std::memory_order_relaxed);
            nextRing.readFrame.store (0, std::memory_order_relaxed);
            nextRing.writeFrame.store (0, std::memory_order_relaxed);
            nextRing.startVideoSec = targetSec;
            nextRing.sampleRate = outRate;

            bool seekOk = session.seekTo (targetSec);
            // If index overshot targetSec on quirky containers, retry further back
            if (seekOk)
            {
                if (session.decodeNextChunk (outRateInt))
                {
                    if (session.pendingStartSec > targetSec + 0.08 && targetSec > 0.25)
                    {
                        const double fallback = std::max (0.0, targetSec - 2.0);
                        session.seekTo (fallback);
                        session.decodeNextChunk (outRateInt);
                    }
                }
            }

            activeRingInitialized = false;
            activeInitialTarget = targetSec;
            bool hitLargeGap = false;
            const uint64_t prefillTarget = static_cast<uint64_t> (outRate * 0.35);

            // Push initial decoded chunk + decode up to 350ms prefill before publishing
            while (!stopRequested.load (std::memory_order_acquire) &&
                   fileCommandSeq.load (std::memory_order_acquire) == lastSeenFileSeq)
            {
                if (!pushPendingAligned (nextRing, outRate, activeInitialTarget, activeRingInitialized, hitLargeGap))
                    break;
                if (activeRingInitialized && nextRing.writeFrame.load (std::memory_order_relaxed) >= prefillTarget)
                    break;
                if (!session.decodeNextChunk (outRateInt))
                    break;
            }

            if (activeRingInitialized && nextRing.writeFrame.load (std::memory_order_relaxed) > 0)
            {
                nextRing.valid.store (true, std::memory_order_release);
                activeAudioRing.store (nextIdx, std::memory_order_release);
                curIdx = nextIdx;
            }
            else
            {
                activeAudioRing.store (-1, std::memory_order_release);
                curIdx = -1;
            }
            if (!isPlaying)
                lastStoppedPrebufferTarget = currentTargetSec;
        }

        // Top up active ring buffer up to 1.5s lookahead during playback (or 0.5s while stopped)
        if (curIdx >= 0 && !session.eofReached && !audioSeekRequested.load (std::memory_order_acquire))
        {
            AudioRing& ring = audioRings[curIdx];
            const uint64_t targetAheadFrames = static_cast<uint64_t> (
                std::min (static_cast<double> (kAudioRingFrames - 16384),
                          outRate * (isPlaying ? 1.5 : 0.5)));

            bool hitLargeGap = false;
            while (!stopRequested.load (std::memory_order_acquire) &&
                   fileCommandSeq.load (std::memory_order_acquire) == lastSeenFileSeq &&
                   !audioSeekRequested.load (std::memory_order_acquire) &&
                   !session.eofReached &&
                   !hitLargeGap)
            {
                const uint64_t r = ring.readFrame.load (std::memory_order_acquire);
                const uint64_t w = ring.writeFrame.load (std::memory_order_relaxed);
                const uint64_t buffered = (w >= r) ? (w - r) : 0;
                if (buffered >= targetAheadFrames)
                    break;

                if (session.pendingFrameOffset >= session.pendingPcm.size () / 2)
                {
                    if (!session.decodeNextChunk (outRateInt))
                        break;
                }

                if (!pushPendingAligned (ring, outRate, activeInitialTarget, activeRingInitialized, hitLargeGap))
                    break;
            }
        }

        std::unique_lock<std::mutex> lk (audioCvMutex);
        const auto waitMs = isPlaying ? std::chrono::milliseconds (8) : std::chrono::milliseconds (35);
        audioCv.wait_for (lk, waitMs, [&] {
            return stopRequested.load (std::memory_order_acquire) ||
                   fileCommandSeq.load (std::memory_order_acquire) != lastSeenFileSeq ||
                   audioSeekRequested.load (std::memory_order_acquire);
        });
    }
}

} // namespace ReasonVideo
