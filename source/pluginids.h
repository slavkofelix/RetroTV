#pragma once

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/vsttypes.h"

namespace ReasonVideo {

// Unique 128-bit VST3 Processor/SingleComponent UID
static const Steinberg::FUID RetroTVProcessorUID (0x7A4E2B19, 0x93C14D88, 0xA6520F1C, 0x4E881101);

enum ParamIDs : Steinberg::Vst::ParamID
{
    kParamOffsetSeconds = 100, // Normalized 0..1 mapped to -300.0 .. +300.0 seconds
    kParamVolume        = 101, // Normalized 0..1 linear/tapered gain
    kParamMute          = 102, // 0 = Unmuted, 1 = Muted
    kParamCrtFx         = 103  // 0 = Clean, 1 = Retro CRT scanlines + vignette
};

constexpr double kMinOffsetSeconds = -300.0;
constexpr double kMaxOffsetSeconds =  300.0;

} // namespace ReasonVideo
