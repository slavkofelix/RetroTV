#include "videoengine.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace ReasonVideo;

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf (stderr, "Usage: %s <test_video.mp4>\n", argv[0]);
        return 2;
    }

    const std::string videoPath = argv[1];
    VideoEngine engine;
    engine.setPixelOrder (PixelOrder::ARGB);
    engine.openFile (videoPath);

    // 1. Verify stopped initial frame at t = 0.0
    engine.updateTransport (false, 0.0, 48000.0, 120.0, 0.0, true);

    uint64_t seq = 0;
    VideoFrameSnapshot snap;
    bool gotInitial = false;
    for (int i = 0; i < 60; ++i)
    {
        if (engine.copyLatestFrameIfChanged (seq, snap) && snap.hasVideo && !snap.outOfRange)
        {
            gotInitial = true;
            break;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (25));
    }
    assert (gotInitial && "Failed to decode initial frame");
    assert (snap.width > 0 && snap.height > 0);
    assert (snap.pixels.size () == static_cast<size_t> (snap.width) * snap.height * 4);
    assert (snap.durationSec > 1.5);
    std::printf ("Initial frame decoded: %dx%d @ %.2f fps, pts=%.3fs, duration=%.3fs\n",
                 snap.width, snap.height, snap.fps, snap.videoPtsSec, snap.durationSec);

    // 2. Verify scrubbing forward to 1.25s
    engine.updateTransport (false, 1.25, 48000.0, 120.0, 2.5, true);
    bool gotScrubFwd = false;
    for (int i = 0; i < 60; ++i)
    {
        if (engine.copyLatestFrameIfChanged (seq, snap) && !snap.outOfRange && snap.videoPtsSec >= 1.1)
        {
            gotScrubFwd = true;
            break;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (25));
    }
    assert (gotScrubFwd && "Failed to scrub forward to 1.25s");
    std::printf ("Forward scrub frame pts=%.3fs\n", snap.videoPtsSec);

    // 3. Verify scrubbing backward to 0.30s
    engine.updateTransport (false, 0.30, 48000.0, 120.0, 0.6, true);
    bool gotScrubBack = false;
    for (int i = 0; i < 60; ++i)
    {
        if (engine.copyLatestFrameIfChanged (seq, snap) && !snap.outOfRange && snap.videoPtsSec <= 0.45)
        {
            gotScrubBack = true;
            break;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (25));
    }
    assert (gotScrubBack && "Failed to scrub backward to 0.30s");
    std::printf ("Backward scrub frame pts=%.3fs\n", snap.videoPtsSec);

    // 4. Verify pre-roll outOfRange when offset = -5.0s and dawTime = 1.0s
    engine.setOffsetSeconds (-5.0);
    engine.updateTransport (false, 1.0, 48000.0, 120.0, 2.0, true);
    bool gotPreRoll = false;
    for (int i = 0; i < 40; ++i)
    {
        if (engine.copyLatestFrameIfChanged (seq, snap) && snap.outOfRange)
        {
            gotPreRoll = true;
            break;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (25));
    }
    assert (gotPreRoll && "Failed to detect pre-roll outOfRange");
    std::printf ("Pre-roll outOfRange verified (pts=%.3fs)\n", snap.videoPtsSec);

    // 5. Verify continuous glitch-free playback & audio extraction at 48kHz (offset = 0.0)
    engine.setOffsetSeconds (0.0);
    constexpr int kBlockSize = 512;
    constexpr double kHostRate = 48000.0;
    std::vector<float> left (kBlockSize, 0.0f), right (kBlockSize, 0.0f);
    bool audioStarted = false;
    int continuousBlocks = 0;
    float prevSample = 0.0f;
    double maxStepAfterFade = 0.0;
    double simTime = 0.0;

    // Simulate ~1.4s of real-time 512-sample callbacks (~10.67ms per block)
    for (int step = 0; step < 130; ++step)
    {
        engine.updateTransport (true, simTime, kHostRate, 120.0, simTime * 2.0, true);
        const bool ok = engine.readAudioSamples (left.data (), right.data (), kBlockSize, simTime, kHostRate, 1.0f, false);

        if (!audioStarted)
        {
            if (ok)
            {
                float peak = 0.0f;
                for (float s : left)
                    peak = std::max (peak, std::fabs (s));
                if (peak > 0.01f)
                {
                    audioStarted = true;
                    continuousBlocks = 1;
                    prevSample = left.back ();
                }
            }
        }
        else
        {
            assert (ok && "Audio dropped out mid-playback!");
            // After initial fade-in block, verify smooth sample-to-sample waveform continuity
            if (continuousBlocks >= 2)
            {
                for (int i = 0; i < kBlockSize; ++i)
                {
                    const double diff = std::fabs (static_cast<double> (left[i]) - static_cast<double> (prevSample));
                    maxStepAfterFade = std::max (maxStepAfterFade, diff);
                    prevSample = left[i];
                }
            }
            else
            {
                prevSample = left.back ();
            }
            continuousBlocks++;
        }

        simTime += static_cast<double> (kBlockSize) / kHostRate;
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    assert (audioStarted && "Failed to start audio playback");
    assert (continuousBlocks >= 100 && "Audio did not stream continuously");
    // 440Hz sine at 48kHz has max step ~0.058; AAC compression adds slight quantization, well under 0.15
    assert (maxStepAfterFade < 0.15 && "Detected sample discontinuity / click in audio stream");
    std::printf ("Continuous 48kHz playback verified: %d blocks, max sample step=%.4f\n",
                 continuousBlocks, maxStepAfterFade);

    // 6. Verify loop jump back to 0.10s and sample-rate conversion to 44.1kHz
    double loopTime = 0.10;
    bool loopAudioStarted = false;
    int loopBlocks = 0;
    for (int step = 0; step < 60; ++step)
    {
        engine.updateTransport (true, loopTime, 44100.0, 120.0, loopTime * 2.0, true);
        if (engine.readAudioSamples (left.data (), right.data (), kBlockSize, loopTime, 44100.0, 1.0f, false))
        {
            loopAudioStarted = true;
            loopBlocks++;
        }
        else if (loopAudioStarted)
        {
            assert (false && "Audio dropped out after loop seek / 44.1kHz switch");
        }
        loopTime += static_cast<double> (kBlockSize) / 44100.0;
        std::this_thread::sleep_for (std::chrono::milliseconds (11));
    }
    assert (loopAudioStarted && loopBlocks >= 25 && "Failed to recover after loop seek + 44.1kHz switch");
    std::printf ("Loop seek + 44.1kHz resample verified (%d continuous blocks).\n", loopBlocks);

    return 0;
}
