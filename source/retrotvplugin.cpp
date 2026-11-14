#include "retrotvplugin.h"
#include "retrotveditor.h"

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "public.sdk/source/vst/vstaudioprocessoralgo.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#if defined(__APPLE__)
#include <dlfcn.h>
#endif

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace ReasonVideo {

namespace {
constexpr uint32 kStateMagic = 0x52545631; // 'RTV1'
constexpr int32 kStateVersion = 1;

std::string resolveBundledEasterEggPath ()
{
#if defined(__APPLE__)
    Dl_info info {};
    if (dladdr (reinterpret_cast<const void*> (&resolveBundledEasterEggPath), &info) != 0 && info.dli_fname)
    {
        // Executable binary sits at .../RetroTVVideo.vst3/Contents/MacOS/RetroTVVideo
        std::string binPath (info.dli_fname);
        const size_t slash1 = binPath.find_last_of ('/');
        if (slash1 != std::string::npos)
        {
            const std::string macOsDir = binPath.substr (0, slash1);
            const size_t slash2 = macOsDir.find_last_of ('/');
            if (slash2 != std::string::npos)
            {
                const std::string contentsDir = macOsDir.substr (0, slash2);
                const std::string resPath = contentsDir + "/Resources/practice-crazy.mp4";
                std::ifstream testFile (resPath);
                if (testFile.good ())
                    return resPath;
            }
        }
    }
#endif
    return {};
}
} // namespace

//-----------------------------------------------------------------------------
RetroTVPlugin::RetroTVPlugin ()
: videoEngine (std::make_unique<VideoEngine> ())
, easterEngine (std::make_unique<VideoEngine> ())
{
}

//-----------------------------------------------------------------------------
double RetroTVPlugin::normalizedToOffsetSeconds (ParamValue norm)
{
    const double clamped = std::clamp (norm, 0.0, 1.0);
    return kMinOffsetSeconds + clamped * (kMaxOffsetSeconds - kMinOffsetSeconds);
}

//-----------------------------------------------------------------------------
ParamValue RetroTVPlugin::offsetSecondsToNormalized (double sec)
{
    const double clamped = std::clamp (sec, kMinOffsetSeconds, kMaxOffsetSeconds);
    return (clamped - kMinOffsetSeconds) / (kMaxOffsetSeconds - kMinOffsetSeconds);
}

//-----------------------------------------------------------------------------
tresult PLUGIN_API RetroTVPlugin::initialize (FUnknown* context)
{
    tresult result = SingleComponentEffect::initialize (context);
    if (result != kResultOk)
        return result;

    // Instrument buses: stereo audio output + event input
    addAudioOutput (STR16 ("Video Audio"), SpeakerArr::kStereo);
    addEventInput (STR16 ("Event In"), 1);

    processContextRequirements.needTransportState ()
        .needProjectTimeMusic ()
        .needTempo ()
        .needTimeSignature ()
        .needSystemTime ();

    // Parameters
    auto* offsetParam = new RangeParameter (
        STR16 ("Video Offset"),
        kParamOffsetSeconds,
        STR16 ("s"),
        kMinOffsetSeconds,
        kMaxOffsetSeconds,
        0.0,
        0,
        ParameterInfo::kCanAutomate);
    offsetParam->setPrecision (2);
    parameters.addParameter (offsetParam);

    auto* volParam = new RangeParameter (
        STR16 ("Audio Gain"),
        kParamVolume,
        STR16 ("%"),
        0.0,
        100.0,
        80.0,
        0,
        ParameterInfo::kCanAutomate);
    volParam->setPrecision (0);
    parameters.addParameter (volParam);

    auto* muteParam = new StringListParameter (STR16 ("Audio Mute"), kParamMute, nullptr, ParameterInfo::kCanAutomate);
    muteParam->appendString (STR16 ("Off"));
    muteParam->appendString (STR16 ("On"));
    muteParam->setNormalized (0.0);
    parameters.addParameter (muteParam);

    auto* crtParam = new StringListParameter (STR16 ("CRT FX"), kParamCrtFx, nullptr, ParameterInfo::kCanAutomate);
    crtParam->appendString (STR16 ("Off"));
    crtParam->appendString (STR16 ("On"));
    crtParam->setNormalized (1.0);
    parameters.addParameter (crtParam);

    return kResultOk;
}

//-----------------------------------------------------------------------------
tresult PLUGIN_API RetroTVPlugin::terminate ()
{
    easterEngine.reset ();
    videoEngine.reset ();
    return SingleComponentEffect::terminate ();
}

//-----------------------------------------------------------------------------
tresult PLUGIN_API RetroTVPlugin::setActive (TBool state)
{
    return SingleComponentEffect::setActive (state);
}

//-----------------------------------------------------------------------------
tresult PLUGIN_API RetroTVPlugin::setupProcessing (ProcessSetup& newSetup)
{
    const size_t maxFrames = static_cast<size_t> (std::max<int32> (1024, newSetup.maxSamplesPerBlock));
    tempLeft.assign (maxFrames, 0.0f);
    tempRight.assign (maxFrames, 0.0f);
    return SingleComponentEffect::setupProcessing (newSetup);
}

//-----------------------------------------------------------------------------
tresult PLUGIN_API RetroTVPlugin::canProcessSampleSize (int32 symbolicSampleSize)
{
    if (symbolicSampleSize == kSample32 || symbolicSampleSize == kSample64)
        return kResultTrue;
    return kResultFalse;
}

//-----------------------------------------------------------------------------
tresult PLUGIN_API RetroTVPlugin::setBusArrangements (SpeakerArrangement* inputs,
                                                      int32 numIns,
                                                      SpeakerArrangement* outputs,
                                                      int32 numOuts)
{
    if (numIns == 0 && numOuts == 1 && outputs && outputs[0] == SpeakerArr::kStereo)
        return SingleComponentEffect::setBusArrangements (inputs, numIns, outputs, numOuts);
    return kResultFalse;
}

//-----------------------------------------------------------------------------
void RetroTVPlugin::applyParameterValue (ParamID tag, ParamValue normValue)
{
    switch (tag)
    {
        case kParamOffsetSeconds:
        {
            const double sec = normalizedToOffsetSeconds (normValue);
            offsetSeconds.store (sec, std::memory_order_relaxed);
            if (videoEngine)
                videoEngine->setOffsetSeconds (sec);
            break;
        }
        case kParamVolume:
            volume.store (static_cast<float> (std::clamp (normValue, 0.0, 1.0)), std::memory_order_relaxed);
            break;
        case kParamMute:
            muted.store (normValue >= 0.5, std::memory_order_relaxed);
            break;
        case kParamCrtFx:
            crtFxEnabled.store (normValue >= 0.5, std::memory_order_relaxed);
            break;
        default:
            break;
    }
}

//-----------------------------------------------------------------------------
tresult PLUGIN_API RetroTVPlugin::setParamNormalized (ParamID tag, ParamValue value)
{
    applyParameterValue (tag, value);
    return SingleComponentEffect::setParamNormalized (tag, value);
}

//-----------------------------------------------------------------------------
void RetroTVPlugin::setVideoFileFromUI (const std::string& path)
{
    if (videoEngine)
        videoEngine->openFile (path);
    setDirty (true);
}

//-----------------------------------------------------------------------------
void RetroTVPlugin::setPreferredPixelOrder (PixelOrder order)
{
    preferredPixelOrder.store (static_cast<int> (order), std::memory_order_relaxed);
    if (videoEngine)
        videoEngine->setPixelOrder (order);
    if (easterEngine)
        easterEngine->setPixelOrder (order);
}

//-----------------------------------------------------------------------------
void RetroTVPlugin::setEasterDurationSeconds (double dur)
{
    if (dur > 0.25)
        easterDurationSec.store (dur, std::memory_order_relaxed);
}

//-----------------------------------------------------------------------------
void RetroTVPlugin::setActiveChannel (int channel)
{
    const int nextCh = (channel == 1) ? 1 : 2;
    if (nextCh == 1)
    {
        easterTimeSec.store (0.0, std::memory_order_relaxed);
        if (easterEngine)
        {
            easterEngine->setPixelOrder (static_cast<PixelOrder> (preferredPixelOrder.load (std::memory_order_relaxed)));
            const std::string eggPath = resolveBundledEasterEggPath ();
            if (!eggPath.empty ())
                easterEngine->openFile (eggPath);
        }
        activeChannel.store (1, std::memory_order_release);
    }
    else
    {
        activeChannel.store (2, std::memory_order_release);
        if (easterEngine)
            easterEngine->clearFile ();
    }
}

//-----------------------------------------------------------------------------
void RetroTVPlugin::advanceEasterClockFromUI (double dtSec)
{
    if (activeChannel.load (std::memory_order_acquire) != 1 || !easterEngine)
        return;

    const uint64_t curCalls = processCallCount.load (std::memory_order_relaxed);
    if (curCalls != lastObservedProcessCalls)
    {
        lastObservedProcessCalls = curCalls;
        return; // Real-time audio thread is driving easterTimeSec
    }

    const double dur = std::max (1.0, easterDurationSec.load (std::memory_order_relaxed));
    double t = easterTimeSec.load (std::memory_order_relaxed) + std::max (0.0, dtSec);
    if (t >= dur)
        t = std::fmod (t, dur);
    easterTimeSec.store (t, std::memory_order_relaxed);
    easterEngine->updateTransport (true, t, 48000.0, 120.0, 0.0, false);
}

//-----------------------------------------------------------------------------
void RetroTVPlugin::setParameterFromUI (ParamID tag, ParamValue normValue)
{
    beginEdit (tag);
    performEdit (tag, normValue);
    endEdit (tag);
    setParamNormalized (tag, normValue);
}

//-----------------------------------------------------------------------------
tresult PLUGIN_API RetroTVPlugin::process (ProcessData& data)
{
    // 1. Apply incoming parameter changes
    if (IParameterChanges* paramChanges = data.inputParameterChanges)
    {
        const int32 count = paramChanges->getParameterCount ();
        for (int32 i = 0; i < count; ++i)
        {
            if (IParamValueQueue* queue = paramChanges->getParameterData (i))
            {
                const int32 points = queue->getPointCount ();
                if (points > 0)
                {
                    ParamValue val = 0.0;
                    int32 sampleOffset = 0;
                    if (queue->getPoint (points - 1, sampleOffset, val) == kResultTrue)
                        applyParameterValue (queue->getParameterId (), val);
                }
            }
        }
    }

    // 2. Forward host ProcessContext to VideoEngine (lock-free)
    processCallCount.fetch_add (1, std::memory_order_relaxed);
    double dawTimeSec = 0.0;
    double sampleRate = processSetup.sampleRate > 0.0 ? processSetup.sampleRate : 44100.0;

    if (const ProcessContext* ctx = data.processContext)
    {
        if (ctx->sampleRate > 0.0)
            sampleRate = ctx->sampleRate;

        const bool isPlaying = (ctx->state & ProcessContext::kPlaying) != 0;
        dawTimeSec = std::max (0.0, static_cast<double> (ctx->projectTimeSamples) / sampleRate);

        const double tempo = (ctx->state & ProcessContext::kTempoValid) ? ctx->tempo : 120.0;
        const bool hasMusic = (ctx->state & ProcessContext::kProjectTimeMusicValid) != 0;
        const double musicPos = hasMusic ? ctx->projectTimeMusic : 0.0;

        if (videoEngine)
            videoEngine->updateTransport (isPlaying, dawTimeSec, sampleRate, tempo, musicPos, hasMusic);
    }

    const int curChannel = activeChannel.load (std::memory_order_acquire);
    double audioClockSec = dawTimeSec;
    VideoEngine* activeAudioEngine = videoEngine.get ();

    if (curChannel == 1 && easterEngine)
    {
        const double dur = std::max (1.0, easterDurationSec.load (std::memory_order_relaxed));
        const double prevEggSec = easterTimeSec.load (std::memory_order_relaxed);
        const double nextEggSec = std::fmod (prevEggSec + static_cast<double> (std::max<int32> (0, data.numSamples)) / sampleRate, dur);
        easterTimeSec.store (nextEggSec, std::memory_order_relaxed);

        easterEngine->updateTransport (true, prevEggSec, sampleRate, 120.0, 0.0, false);
        audioClockSec = prevEggSec;
        activeAudioEngine = easterEngine.get ();
    }

    // 3. Output decoded video guide audio (or silence)
    if (data.numOutputs > 0 && data.outputs && data.numSamples > 0)
    {
        AudioBusBuffers& outBus = data.outputs[0];
        const int32 numCh = outBus.numChannels;
        const int32 n = data.numSamples;

        bool wroteAudio = false;
        if (numCh >= 2 && activeAudioEngine && n <= static_cast<int32> (tempLeft.size ()))
        {
            wroteAudio = activeAudioEngine->readAudioSamples (
                tempLeft.data (),
                tempRight.data (),
                n,
                audioClockSec,
                sampleRate,
                volume.load (std::memory_order_relaxed),
                muted.load (std::memory_order_relaxed));
        }

        if (wroteAudio)
        {
            outBus.silenceFlags = 0;
            if (data.symbolicSampleSize == kSample32 && outBus.channelBuffers32)
            {
                std::memcpy (outBus.channelBuffers32[0], tempLeft.data (), static_cast<size_t> (n) * sizeof (float));
                std::memcpy (outBus.channelBuffers32[1], tempRight.data (), static_cast<size_t> (n) * sizeof (float));
                for (int32 ch = 2; ch < numCh; ++ch)
                    std::memset (outBus.channelBuffers32[ch], 0, static_cast<size_t> (n) * sizeof (float));
            }
            else if (data.symbolicSampleSize == kSample64 && outBus.channelBuffers64)
            {
                for (int32 i = 0; i < n; ++i)
                {
                    outBus.channelBuffers64[0][i] = static_cast<double> (tempLeft[i]);
                    outBus.channelBuffers64[1][i] = static_cast<double> (tempRight[i]);
                }
                for (int32 ch = 2; ch < numCh; ++ch)
                    std::memset (outBus.channelBuffers64[ch], 0, static_cast<size_t> (n) * sizeof (double));
            }
        }
        else
        {
            outBus.silenceFlags = getChannelMask (numCh);
            const uint32 bytesPerCh = getSampleFramesSizeInBytes (processSetup, n);
            void** out = getChannelBuffersPointer (processSetup, outBus);
            for (int32 ch = 0; ch < numCh; ++ch)
            {
                if (out && out[ch])
                    std::memset (out[ch], 0, bytesPerCh);
            }
        }
    }

    return kResultOk;
}

//-----------------------------------------------------------------------------
tresult PLUGIN_API RetroTVPlugin::getState (IBStream* state)
{
    if (!state)
        return kInvalidArgument;

    IBStreamer streamer (state, kLittleEndian);
    if (!streamer.writeInt32u (kStateMagic))
        return kResultFalse;
    if (!streamer.writeInt32 (kStateVersion))
        return kResultFalse;

    const std::string path = videoEngine ? videoEngine->getFilePath () : std::string ();
    const uint32 pathLen = static_cast<uint32> (path.size ());
    if (!streamer.writeInt32u (pathLen))
        return kResultFalse;
    if (pathLen > 0 && !streamer.writeRaw (path.data (), pathLen))
        return kResultFalse;

    if (!streamer.writeDouble (offsetSeconds.load (std::memory_order_relaxed)))
        return kResultFalse;
    if (!streamer.writeFloat (volume.load (std::memory_order_relaxed)))
        return kResultFalse;
    if (!streamer.writeInt8 (muted.load (std::memory_order_relaxed) ? 1 : 0))
        return kResultFalse;
    if (!streamer.writeInt8 (crtFxEnabled.load (std::memory_order_relaxed) ? 1 : 0))
        return kResultFalse;

    return kResultOk;
}

//-----------------------------------------------------------------------------
tresult PLUGIN_API RetroTVPlugin::setState (IBStream* state)
{
    if (!state)
        return kInvalidArgument;

    IBStreamer streamer (state, kLittleEndian);

    uint32 magic = 0;
    if (!streamer.readInt32u (magic) || magic != kStateMagic)
        return kResultFalse;

    int32 version = 0;
    if (!streamer.readInt32 (version) || version != kStateVersion)
        return kResultFalse;

    uint32 pathLen = 0;
    if (!streamer.readInt32u (pathLen) || pathLen > 65536)
        return kResultFalse;

    std::string loadedPath;
    if (pathLen > 0)
    {
        loadedPath.resize (pathLen);
        if (!streamer.readRaw (&loadedPath[0], pathLen))
            return kResultFalse;
    }

    double savedOffset = 0.0;
    float savedVol = 0.8f;
    int8 savedMute = 0;
    int8 savedCrt = 1;

    if (!streamer.readDouble (savedOffset))
        return kResultFalse;
    if (!streamer.readFloat (savedVol))
        return kResultFalse;
    if (!streamer.readInt8 (savedMute))
        return kResultFalse;
    if (!streamer.readInt8 (savedCrt))
        return kResultFalse;

    setParamNormalized (kParamOffsetSeconds, offsetSecondsToNormalized (savedOffset));
    setParamNormalized (kParamVolume, std::clamp (static_cast<double> (savedVol), 0.0, 1.0));
    setParamNormalized (kParamMute, savedMute ? 1.0 : 0.0);
    setParamNormalized (kParamCrtFx, savedCrt ? 1.0 : 0.0);

    if (videoEngine)
        videoEngine->openFile (loadedPath);

    return kResultOk;
}

//-----------------------------------------------------------------------------
IPlugView* PLUGIN_API RetroTVPlugin::createView (FIDString name)
{
    if (name && std::strcmp (name, ViewType::kEditor) == 0)
        return new RetroTVEditor (this);
    return nullptr;
}

} // namespace ReasonVideo
