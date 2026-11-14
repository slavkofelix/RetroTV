#include "public.sdk/source/main/pluginfactory.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"

#include "pluginids.h"
#include "retrotvplugin.h"
#include "version.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

BEGIN_FACTORY_DEF (stringCompanyName, stringCompanyWeb, stringCompanyEmail)

    DEF_CLASS2 (INLINE_UID_FROM_FUID (ReasonVideo::RetroTVProcessorUID),
                PClassInfo::kManyInstances,
                kVstAudioEffectClass,
                stringPluginName,
                0,
                Vst::PlugType::kInstrument,
                FULL_VERSION_STR,
                kVstVersionString,
                ReasonVideo::RetroTVPlugin::createInstance)

END_FACTORY
