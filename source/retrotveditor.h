#pragma once

#include "public.sdk/source/vst/vstguieditor.h"
#include "vstgui/lib/controls/icontrollistener.h"
#include "vstgui/lib/cvstguitimer.h"

#include "videoengine.h"

#include <cstdint>
#include <string>

namespace ReasonVideo {

class RetroTVPlugin;
class RetroTVChassisView;
class RetroCRTView;
class RetroVhfDial;
class RetroRoundButton;
class RetroLoadButton;

class RetroTVEditor : public Steinberg::Vst::VSTGUIEditor,
                      public VSTGUI::IControlListener
{
public:
    explicit RetroTVEditor (RetroTVPlugin* plugin);
    ~RetroTVEditor () override;

    //--- VSTGUIEditor ---
    bool PLUGIN_API open (void* parent, const VSTGUI::PlatformType& platformType) SMTG_OVERRIDE;
    void PLUGIN_API close () SMTG_OVERRIDE;
    VSTGUI::CMessageResult notify (VSTGUI::CBaseObject* sender, const char* message) SMTG_OVERRIDE;

    //--- IPlugView ---
    Steinberg::tresult PLUGIN_API canResize () SMTG_OVERRIDE { return Steinberg::kResultTrue; }
    Steinberg::tresult PLUGIN_API checkSizeConstraint (Steinberg::ViewRect* rect) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API onSize (Steinberg::ViewRect* newSize) SMTG_OVERRIDE;

    //--- IControlListener ---
    void valueChanged (VSTGUI::CControl* pControl) SMTG_OVERRIDE;
    void controlBeginEdit (VSTGUI::CControl* pControl) SMTG_OVERRIDE;
    void controlEndEdit (VSTGUI::CControl* pControl) SMTG_OVERRIDE;

    void requestOpenVideoDialog ();
    void handleDroppedFile (const std::string& path);
    void toggleCrtFx ();
    void nudgeOffset (int frames);
    void resetOffset ();
    void setTeletextPage (int page);
    int getTeletextPage () const { return teletextPage; }
    void setActiveChannel (int ch);
    int getActiveChannel () const;
    bool isCrtFxEnabled () const;

private:
    enum UIControlTags
    {
        kTagLoadButton = 1000,
        kTagInfoButton = 1001
    };

    void layoutViews (VSTGUI::CCoord width, VSTGUI::CCoord height);
    void pollEngineAndRefresh ();
    void syncControlsFromPlugin ();

    RetroTVPlugin* plugin {nullptr};

    RetroTVChassisView* chassisView {nullptr};
    RetroCRTView* crtView {nullptr};
    RetroVhfDial* offsetDial {nullptr};
    RetroVhfDial* volumeDial {nullptr};
    RetroLoadButton* loadButton {nullptr};
    RetroRoundButton* muteButton {nullptr};
    RetroRoundButton* infoButton {nullptr};

    VSTGUI::SharedPointer<VSTGUI::CVSTGUITimer> refreshTimer;
    uint64_t lastFrameSeq {0};
    VideoFrameSnapshot cachedFrame;
    bool osdEnabled {true};
    int teletextPage {-1};
};

} // namespace ReasonVideo
