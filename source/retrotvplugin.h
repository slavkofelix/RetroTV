#pragma once

// Must come first before vsteditcontroller includes
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include "pluginids.h"
#include "videoengine.h"

#include <atomic>
#include <memory>
#include <string>

namespace ReasonVideo {

class RetroTVPlugin : public Steinberg::Vst::SingleComponentEffect
{
public:
    RetroTVPlugin ();
    ~RetroTVPlugin () override = default;

    static Steinberg::FUnknown* createInstance (void* /*context*/)
    {
        return static_cast<Steinberg::Vst::IAudioProcessor*> (new RetroTVPlugin ());
    }

    //--- IPluginBase ---
    Steinberg::tresult PLUGIN_API initialize (Steinberg::FUnknown* context) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API terminate () SMTG_OVERRIDE;

    //--- IComponent ---
    Steinberg::tresult PLUGIN_API setActive (Steinberg::TBool state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setState (Steinberg::IBStream* state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getState (Steinberg::IBStream* state) SMTG_OVERRIDE;

    //--- IAudioProcessor ---
    Steinberg::tresult PLUGIN_API setupProcessing (Steinberg::Vst::ProcessSetup& newSetup) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API canProcessSampleSize (Steinberg::int32 symbolicSampleSize) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setBusArrangements (Steinberg::Vst::SpeakerArrangement* inputs,
                                                      Steinberg::int32 numIns,
                                                      Steinberg::Vst::SpeakerArrangement* outputs,
                                                      Steinberg::int32 numOuts) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API process (Steinberg::Vst::ProcessData& data) SMTG_OVERRIDE;

    //--- IEditController ---
    Steinberg::IPlugView* PLUGIN_API createView (Steinberg::FIDString name) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setParamNormalized (Steinberg::Vst::ParamID tag,
                                                      Steinberg::Vst::ParamValue value) SMTG_OVERRIDE;

    //--- Helpers for Editor ---
    VideoEngine& getVideoEngine () { return *videoEngine; }
    VideoEngine* getEasterEngine () { return easterEngine.get (); }
    void setVideoFileFromUI (const std::string& path);
    void setParameterFromUI (Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue normValue);

    void setActiveChannel (int channel);
    int getActiveChannel () const { return activeChannel.load (std::memory_order_acquire); }
    void setPreferredPixelOrder (PixelOrder order);
    double getEasterTimeSeconds () const { return easterTimeSec.load (std::memory_order_relaxed); }
    void setEasterDurationSeconds (double dur);
    void advanceEasterClockFromUI (double dtSec);

    bool isCrtFxEnabled () const { return crtFxEnabled.load (std::memory_order_relaxed); }
    bool isMuted () const { return muted.load (std::memory_order_relaxed); }
    float getVolume () const { return volume.load (std::memory_order_relaxed); }
    double getOffsetSeconds () const { return offsetSeconds.load (std::memory_order_relaxed); }

    static double normalizedToOffsetSeconds (Steinberg::Vst::ParamValue norm);
    static Steinberg::Vst::ParamValue offsetSecondsToNormalized (double sec);

    OBJ_METHODS (RetroTVPlugin, SingleComponentEffect)
    REFCOUNT_METHODS (SingleComponentEffect)

private:
    void applyParameterValue (Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue normValue);

    std::unique_ptr<VideoEngine> videoEngine;
    std::unique_ptr<VideoEngine> easterEngine;

    std::atomic<int> activeChannel {2}; // 2 = User DAW video (Default), 1 = Bundled Easter Egg
    std::atomic<int> preferredPixelOrder {static_cast<int> (PixelOrder::ARGB)};
    std::atomic<double> easterTimeSec {0.0};
    std::atomic<double> easterDurationSec {26.922667};
    std::atomic<uint64_t> processCallCount {0};
    uint64_t lastObservedProcessCalls {0};

    std::atomic<double> offsetSeconds {0.0};
    std::atomic<float> volume {0.8f};
    std::atomic<bool> muted {false};
    std::atomic<bool> crtFxEnabled {true};

    std::vector<float> tempLeft;
    std::vector<float> tempRight;
};

} // namespace ReasonVideo
