#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ReasonVideo {

enum class PixelOrder
{
    ARGB = 0, // macOS CoreGraphics little-endian (bytes: A R G B)
    RGBA = 1,
    ABGR = 2,
    BGRA = 3  // Windows Direct2D (bytes: B G R A)
};

struct TransportSnapshot
{
    bool isPlaying {false};
    double dawTimeSec {0.0};
    double sampleRate {44100.0};
    double tempo {120.0};
    double projectTimeMusic {0.0};
    bool hasMusicTime {false};
};

struct VideoFrameSnapshot
{
    uint64_t frameSequence {0};
    int width {0};
    int height {0};
    int strideBytes {0};
    PixelOrder pixelOrder {PixelOrder::ARGB};
    std::vector<uint8_t> pixels;

    double videoPtsSec {0.0};
    double durationSec {0.0};
    double fps {24.0};
    bool hasVideo {false};
    bool outOfRange {false};
    std::string filePath;
    std::string errorMessage;
};

class VideoEngine
{
public:
    VideoEngine ();
    ~VideoEngine ();

    VideoEngine (const VideoEngine&) = delete;
    VideoEngine& operator= (const VideoEngine&) = delete;

    // Called from UI / State restore (thread-safe)
    void openFile (const std::string& path);
    void clearFile ();
    std::string getFilePath () const;

    void setOffsetSeconds (double offsetSec);
    double getOffsetSeconds () const;

    void setPixelOrder (PixelOrder order);

    // Called from real-time audio thread (lock-free)
    void updateTransport (bool isPlaying,
                          double dawTimeSec,
                          double sampleRate,
                          double tempo,
                          double projectTimeMusic,
                          bool hasMusicTime);

    // Called from real-time audio thread (lock-free read from SPSC ring buffer)
    // Returns true if audio samples were written to outLeft/outRight.
    bool readAudioSamples (float* outLeft,
                           float* outRight,
                           int32_t numSamples,
                           double dawTimeSec,
                           double sampleRate,
                           float volume,
                           bool muted);

    // Called from UI thread
    TransportSnapshot getTransportSnapshot () const;
    bool copyLatestFrameIfChanged (uint64_t& lastSeenSequence, VideoFrameSnapshot& outFrame);
    void getStatus (bool& outHasVideo,
                    double& outDurationSec,
                    double& outFps,
                    double& outVideoPtsSec,
                    bool& outOfRange,
                    std::string& outFilePath,
                    std::string& outError) const;

private:
    void videoWorkerLoop ();
    void audioWorkerLoop ();

    // Atomic transport state written by audio thread, read by workers/UI
    std::atomic<bool> transportPlaying {false};
    std::atomic<double> transportDawTimeSec {0.0};
    std::atomic<double> transportSampleRate {44100.0};
    std::atomic<double> transportTempo {120.0};
    std::atomic<double> transportMusicPos {0.0};
    std::atomic<bool> transportHasMusic {false};
    std::atomic<uint64_t> transportGeneration {0};

    std::atomic<double> offsetSeconds {0.0};
    std::atomic<int> requestedPixelOrder {
#if defined(_WIN32)
        static_cast<int> (PixelOrder::BGRA)
#else
        static_cast<int> (PixelOrder::ARGB)
#endif
    };

    // File command state shared cleanly between UI and both video & audio workers
    mutable std::mutex commandMutex;
    std::string desiredFilePath;
    std::atomic<uint64_t> fileCommandSeq {0};

    // Shared published frame buffer (video worker -> UI)
    mutable std::mutex frameMutex;
    VideoFrameSnapshot publishedFrame;

    // Wait-free ping-pong SPSC audio ring buffers (audio worker -> DAW audio thread)
    static constexpr size_t kAudioRingFrames = 96000 * 4; // 4 sec @ 96kHz (8 sec @ 48kHz)
    struct AudioRing
    {
        std::vector<float> pcm; // interleaved stereo L, R
        std::atomic<uint64_t> readFrame {0};
        std::atomic<uint64_t> writeFrame {0};
        double startVideoSec {0.0};
        double sampleRate {48000.0};
        std::atomic<bool> valid {false};
    };

    AudioRing audioRings[2];
    std::atomic<int> activeAudioRing {-1};
    std::atomic<int> readerInAudioRing {-1};
    std::atomic<bool> audioSeekRequested {true};
    std::atomic<double> audioSeekTargetSec {0.0};
    std::atomic<uint64_t> audioSeekEpoch {0};
    std::atomic<bool> hasAudioStream {false};

    // Audio-thread-only smoothing state
    bool audioWasOutputting {false};
    int lastReaderRingIdx {-1};
    int fadeInSamplesRemaining {0};

    // Worker thread management
    std::atomic<bool> stopRequested {false};
    std::condition_variable videoCv;
    std::mutex videoCvMutex;
    std::condition_variable audioCv;
    std::mutex audioCvMutex;
    std::thread videoThread;
    std::thread audioThread;
};

} // namespace ReasonVideo
