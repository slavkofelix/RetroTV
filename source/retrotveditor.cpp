#include "retrotvplugin.h"
#include "retrotveditor.h"
#include "pluginids.h"

#include "vstgui/lib/cbitmap.h"
#include "vstgui/lib/ccolor.h"
#include "vstgui/lib/cdrawcontext.h"
#include "vstgui/lib/cfileselector.h"
#include "vstgui/lib/cfont.h"
#include "vstgui/lib/cframe.h"
#include "vstgui/lib/cgraphicspath.h"
#include "vstgui/lib/cgraphicstransform.h"
#include "vstgui/lib/controls/ccontrol.h"
#include "vstgui/lib/controls/cknob.h"
#include "vstgui/lib/dragging.h"
#include "vstgui/lib/idatapackage.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

using namespace VSTGUI;

namespace ReasonVideo {

namespace {

constexpr CCoord kDefaultWidth  = 760.0;
constexpr CCoord kDefaultHeight = 440.0;
constexpr CCoord kMinWidth      = 520.0;
constexpr CCoord kMinHeight     = 350.0;
constexpr CCoord kMaxWidth      = 1920.0;
constexpr CCoord kMaxHeight     = 1200.0;

PixelOrder fromVstGuiPixelFormat (IPlatformBitmapPixelAccess::PixelFormat fmt)
{
    switch (fmt)
    {
        case IPlatformBitmapPixelAccess::kARGB: return PixelOrder::ARGB;
        case IPlatformBitmapPixelAccess::kRGBA: return PixelOrder::RGBA;
        case IPlatformBitmapPixelAccess::kABGR: return PixelOrder::ABGR;
        case IPlatformBitmapPixelAccess::kBGRA: return PixelOrder::BGRA;
    }
    return PixelOrder::ARGB;
}

std::string formatSmpte (double seconds, double fps)
{
    const bool negative = seconds < 0.0;
    const double absSec = std::fabs (seconds);
    const double safeFps = (fps > 1.0 && fps < 240.0) ? fps : 24.0;
    const int64_t totalFrames = static_cast<int64_t> (std::floor (absSec * safeFps + 1e-6));
    const int fpsInt = std::max (1, static_cast<int> (std::llround (safeFps)));

    const int frames = static_cast<int> (totalFrames % fpsInt);
    const int64_t totalSecs = totalFrames / fpsInt;
    const int secs = static_cast<int> (totalSecs % 60);
    const int mins = static_cast<int> ((totalSecs / 60) % 60);
    const int hrs = static_cast<int> (totalSecs / 3600);

    char buf[32];
    std::snprintf (buf, sizeof (buf), "%s%02d:%02d:%02d:%02d",
                   negative ? "-" : "", hrs, mins, secs, frames);
    return buf;
}

void drawRoundedRect (CDrawContext* ctx, const CRect& r, CCoord radius, const CColor& fill, const CColor& stroke, CCoord lineWidth = 1.0)
{
    if (auto path = owned (ctx->createGraphicsPath ()))
    {
        path->addRoundRect (r, radius);
        if (fill.alpha > 0)
        {
            ctx->setFillColor (fill);
            ctx->drawGraphicsPath (path, CDrawContext::kPathFilled);
        }
        if (stroke.alpha > 0 && lineWidth > 0.0)
        {
            ctx->setFrameColor (stroke);
            ctx->setLineWidth (lineWidth);
            ctx->drawGraphicsPath (path, CDrawContext::kPathStroked);
        }
    }
}

//-----------------------------------------------------------------------------
void addQuadBezier (CGraphicsPath* path, const CPoint& p0, const CPoint& ctrl, const CPoint& p1)
{
    const CPoint cp1 (p0.x + (2.0 / 3.0) * (ctrl.x - p0.x),
                      p0.y + (2.0 / 3.0) * (ctrl.y - p0.y));
    const CPoint cp2 (p1.x + (2.0 / 3.0) * (ctrl.x - p1.x),
                      p1.y + (2.0 / 3.0) * (ctrl.y - p1.y));
    path->addBezierCurve (cp1, cp2, p1);
}

//-----------------------------------------------------------------------------
// Curved CRT Tube Path Subpath
// 8-segment closed path with bowed edges and rounded corners
//-----------------------------------------------------------------------------
void addCrtCutoutSubpath (CGraphicsPath* path, CCoord x, CCoord y, CCoord w, CCoord h, CCoord r, CCoord bow)
{
    path->beginSubpath (x + r, y);
    addQuadBezier (path, CPoint (x + r, y), CPoint (x + w * 0.5, y - bow), CPoint (x + w - r, y));
    addQuadBezier (path, CPoint (x + w - r, y), CPoint (x + w, y), CPoint (x + w, y + r));
    addQuadBezier (path, CPoint (x + w, y + r), CPoint (x + w + bow, y + h * 0.5), CPoint (x + w, y + h - r));
    addQuadBezier (path, CPoint (x + w, y + h - r), CPoint (x + w, y + h), CPoint (x + w - r, y + h));
    addQuadBezier (path, CPoint (x + w - r, y + h), CPoint (x + w * 0.5, y + h + bow), CPoint (x + r, y + h));
    addQuadBezier (path, CPoint (x + r, y + h), CPoint (x, y + h), CPoint (x, y + h - r));
    addQuadBezier (path, CPoint (x, y + h - r), CPoint (x - bow, y + h * 0.5), CPoint (x, y + r));
    addQuadBezier (path, CPoint (x, y + r), CPoint (x, y), CPoint (x + r, y));
    path->closeSubpath ();
}

CCoord computeLowerModuleY (const CRect& bayRect)
{
    // Keep lower control + speaker/service module compact so stacked dials above stay ~1.5x larger
    const CCoord lowerH = std::clamp (std::round (bayRect.getHeight () * 0.44), 128.0, 165.0);
    return bayRect.bottom - lowerH;
}

} // namespace

//-----------------------------------------------------------------------------
// RetroVhfDial: VHF/UHF Rotary Knob with fluted bar, pip dot, tick marks & label
//-----------------------------------------------------------------------------
class RetroVhfDial : public CKnobBase
{
public:
    RetroVhfDial (const CRect& size, IControlListener* listener, int32_t tag, std::string label, float defaultVal)
    : CKnobBase (size, listener, tag, nullptr)
    , labelText (std::move (label))
    , defaultValue (defaultVal)
    {
        setStartAngle (static_cast<float> (3.0 * M_PI / 4.0));
        setRangeAngle (static_cast<float> (3.0 * M_PI / 2.0));
    }

    CMouseEventResult onMouseDown (CPoint& where, const CButtonState& buttons) override
    {
        if (!buttons.isLeftButton ())
            return kMouseEventNotHandled;

        if (buttons.isDoubleClick ())
        {
            beginEdit ();
            setValueNormalized (defaultValue);
            valueChanged ();
            endEdit ();
            invalid ();
            return kMouseEventHandled;
        }

        beginEdit ();
        isDragging = true;
        dragStartPoint = where;
        dragStartVal = getValueNormalized ();
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseMoved (CPoint& where, const CButtonState& buttons) override
    {
        if (isDragging)
        {
            const double dy = dragStartPoint.y - where.y;
            const double speed = (buttons & kShift) ? 0.001 : 0.005;
            const float nextVal = std::clamp (static_cast<float> (dragStartVal + dy * speed), 0.0f, 1.0f);
            if (std::fabs (nextVal - getValueNormalized ()) > 1e-4f)
            {
                setValueNormalized (nextVal);
                valueChanged ();
                invalid ();
            }
            return kMouseEventHandled;
        }
        return kMouseEventNotHandled;
    }

    CMouseEventResult onMouseUp (CPoint&, const CButtonState&) override
    {
        if (isDragging)
        {
            isDragging = false;
            endEdit ();
            return kMouseEventHandled;
        }
        return kMouseEventNotHandled;
    }

    CMouseEventResult onMouseCancel () override
    {
        if (isDragging)
        {
            isDragging = false;
            endEdit ();
            return kMouseEventHandled;
        }
        return kMouseEventNotHandled;
    }

    void onMouseWheelEvent (MouseWheelEvent& event) override
    {
        beginEdit ();
        const float step = (event.modifiers.has (ModifierKey::Shift)) ? 0.01f : 0.04f;
        const float nextVal = std::clamp (getValueNormalized () + static_cast<float> (event.deltaY * step), 0.0f, 1.0f);
        setValueNormalized (nextVal);
        valueChanged ();
        endEdit ();
        invalid ();
        event.consumed = true;
    }

    void draw (CDrawContext* ctx) override
    {
        const CRect bounds = getViewSize ();
        const CCoord r = std::max (14.0, std::min (std::floor ((bounds.getHeight () - 24.0) * 0.5),
                                                   std::floor ((bounds.getWidth () - 16.0) * 0.44)));
        const CCoord cx = bounds.getCenter ().x;
        const CCoord cy = bounds.top + r + 5.0;
        const float normVal = getValueNormalized ();

        // 1. Grounding drop shadow on Studio Silver
        if (auto shPath = owned (ctx->createGraphicsPath ()))
        {
            shPath->addEllipse (CRect (cx + 2.0 - (r + 5.0), cy + 3.0 - (r + 5.0),
                                       cx + 2.0 + (r + 5.0), cy + 3.0 + (r + 5.0)));
            auto shGrad = owned (CGradient::create (0.0, 1.0, CColor (0, 0, 0, 184), CColor (0, 0, 0, 0)));
            shGrad->addColorStop (0.55, CColor (0, 0, 0, 100));
            ctx->fillRadialGradient (shPath, *shGrad, CPoint (cx + 2.0, cy + 3.0), r + 5.0);
        }

        // 2. Outer dark gunmetal collar #141619 + #9aa0a8 hairline rim
        CRect outer (cx - r, cy - r, cx + r, cy + r);
        ctx->setFillColor (CColor (20, 22, 25, 255));
        ctx->drawEllipse (outer, kDrawFilled);
        ctx->setFrameColor (CColor (154, 160, 168, 255));
        ctx->setLineWidth (0.9);
        ctx->drawEllipse (outer, kDrawStroked);

        // 3. 11 radial chrome channel detent ticks (center tick amber #f0a540)
        const double startA = (135.0 * M_PI) / 180.0;
        const double rangeA = (270.0 * M_PI) / 180.0;
        for (int i = 0; i <= 10; ++i)
        {
            const double a = startA + (static_cast<double> (i) / 10.0) * rangeA;
            const CCoord rIn = r * 0.73;
            const CCoord rOut = (i == 5) ? r * 0.94 : r * 0.89;
            ctx->setFrameColor (i == 5 ? CColor (240, 165, 64, 255) : CColor (207, 213, 222, 255));
            ctx->setLineWidth ((i == 5 || i == 0 || i == 10) ? 1.5 : 1.0);
            ctx->drawLine (CPoint (cx + std::cos (a) * rIn, cy + std::sin (a) * rIn),
                           CPoint (cx + std::cos (a) * rOut, cy + std::sin (a) * rOut));
        }

        // 4. Knurled dark inner collar ring #1f2227 + #474c54
        const CCoord rCollar = r * 0.69;
        CRect collarRect (cx - rCollar, cy - rCollar, cx + rCollar, cy + rCollar);
        ctx->setFillColor (CColor (31, 34, 39, 255));
        ctx->drawEllipse (collarRect, kDrawFilled);
        ctx->setFrameColor (CColor (71, 76, 84, 255));
        ctx->setLineWidth (1.1);
        ctx->drawEllipse (collarRect, kDrawStroked);

        // 5. Dark concave inner bowl #363a42 -> #202328 -> #121417
        const CCoord rBowl = r * 0.62;
        CRect bowlRect (cx - rBowl, cy - rBowl, cx + rBowl, cy + rBowl);
        if (auto bowlPath = owned (ctx->createGraphicsPath ()))
        {
            bowlPath->addEllipse (bowlRect);
            auto bowlG = owned (CGradient::create (0.0, 1.0, CColor (54, 58, 66, 255), CColor (18, 20, 23, 255)));
            bowlG->addColorStop (0.65, CColor (32, 35, 40, 255));
            ctx->fillRadialGradient (bowlPath, *bowlG, CPoint (cx - r * 0.14, cy - r * 0.14), rBowl);
            ctx->setFrameColor (CColor (12, 13, 15, 255));
            ctx->setLineWidth (1.0);
            ctx->drawGraphicsPath (bowlPath, CDrawContext::kPathStroked);
        }

        // 6. Subtle stepped center hub #5d636d -> #26292e
        const CCoord rHub = r * 0.34;
        CRect hubRect (cx - rHub, cy - rHub, cx + rHub, cy + rHub);
        if (auto hubPath = owned (ctx->createGraphicsPath ()))
        {
            hubPath->addEllipse (hubRect);
            auto hubG = owned (CGradient::create (0.0, 1.0, CColor (93, 99, 109, 255), CColor (38, 41, 46, 255)));
            ctx->fillRadialGradient (hubPath, *hubG, CPoint (cx - 1.5, cy - 1.5), rHub);
            ctx->setFrameColor (CColor (20, 22, 25, 255));
            ctx->setLineWidth (0.9);
            ctx->drawGraphicsPath (hubPath, CDrawContext::kPathStroked);
        }

        // 7. Rotating RED DOT indicator (no handle bar)
        const double ang = startA + static_cast<double> (normVal) * rangeA;
        const CCoord dotDist = r * 0.48;
        const CCoord dx = cx + std::cos (ang) * dotDist;
        const CCoord dy = cy + std::sin (ang) * dotDist;
        const CCoord dotR = std::max (2.6, r * 0.105);

        ctx->setFillColor (CColor (11, 12, 14, 255));
        ctx->drawEllipse (CRect (dx - (dotR + 1.1), dy - (dotR + 1.1), dx + (dotR + 1.1), dy + (dotR + 1.1)), kDrawFilled);

        if (auto dotPath = owned (ctx->createGraphicsPath ()))
        {
            dotPath->addEllipse (CRect (dx - dotR, dy - dotR, dx + dotR, dy + dotR));
            auto dotG = owned (CGradient::create (0.0, 1.0, CColor (255, 142, 128, 255), CColor (181, 29, 20, 255)));
            dotG->addColorStop (0.45, CColor (255, 61, 46, 255));
            ctx->fillRadialGradient (dotPath, *dotG, CPoint (dx - dotR * 0.3, dy - dotR * 0.3), dotR);
        }

        // 8. Centered crisp light label directly beneath dial on Studio Silver
        auto f = owned (new CFontDesc ("Menlo", (r >= 28.0) ? 10.5 : 9.5, kBoldFace));
        ctx->setFont (f);
        ctx->setFontColor (CColor (0, 0, 0, 209));
        CRect textSh (bounds.left + 1.0, cy + r + 5.0, bounds.right + 1.0, bounds.bottom);
        ctx->drawString (labelText.c_str (), textSh, kCenterText);
        ctx->setFontColor (CColor (245, 247, 250, 255)); // #f5f7fa
        CRect textR (bounds.left, cy + r + 4.0, bounds.right, bounds.bottom - 1.0);
        ctx->drawString (labelText.c_str (), textR, kCenterText);
    }

    CLASS_METHODS_NOCOPY (RetroVhfDial, CKnobBase)

private:
    std::string labelText;
    float defaultValue {0.5f};
    CPoint dragStartPoint;
    float dragStartVal {0.0f};
    bool isDragging {false};
};

//-----------------------------------------------------------------------------
// RetroRoundButton: Round pushbutton with glowing backlit cap & label
//-----------------------------------------------------------------------------
class RetroRoundButton : public CControl
{
public:
    RetroRoundButton (const CRect& size, IControlListener* listener, int32_t tag, std::string label, const CColor& ledColor)
    : CControl (size, listener, tag)
    , labelText (std::move (label))
    , activeColor (ledColor)
    {
    }

    CMouseEventResult onMouseDown (CPoint&, const CButtonState& buttons) override
    {
        if (!buttons.isLeftButton ())
            return kMouseEventNotHandled;

        beginEdit ();
        const float nextVal = (getValueNormalized () > 0.5f) ? 0.0f : 1.0f;
        setValueNormalized (nextVal);
        valueChanged ();
        endEdit ();
        invalid ();
        return kMouseEventHandled;
    }

    void draw (CDrawContext* ctx) override
    {
        const CRect r = getViewSize ();
        const CCoord cx = r.getCenter ().x;
        const CCoord cy = r.top + 10.0;
        const CCoord radius = 7.5;
        const bool active = (getValueNormalized () > 0.5f);

        // Dark recess outer ring: radius + 2.8
        CRect recess (cx - radius - 2.8, cy - radius - 2.8, cx + radius + 2.8, cy + radius + 2.8);
        ctx->setFillColor (CColor (18, 19, 22, 255)); // #121316
        ctx->drawEllipse (recess, kDrawFilled);

        // Cap radial gradient
        CRect capRect (cx - radius, cy - radius, cx + radius, cy + radius);
        if (auto path = owned (ctx->createGraphicsPath ()))
        {
            path->addEllipse (capRect);
            if (active)
            {
                auto grad = owned (CGradient::create (0.0, 1.0, CColor (255, 255, 255, 255), CColor (97, 102, 109, 255)));
                grad->addColorStop (0.55, activeColor);
                ctx->fillRadialGradient (path, *grad, CPoint (cx - 2.0, cy - 2.0), radius);
            }
            else
            {
                auto grad = owned (CGradient::create (0.0, 1.0, CColor (220, 224, 230, 255), CColor (74, 78, 84, 255)));
                grad->addColorStop (0.6, CColor (144, 150, 158, 255));
                ctx->fillRadialGradient (path, *grad, CPoint (cx - 2.0, cy - 2.0), radius);
            }
        }

        // Cap stroke
        ctx->setFrameColor (active ? activeColor : CColor (37, 40, 44, 255));
        ctx->setLineWidth (1.2);
        ctx->drawEllipse (capRect, kDrawStroked);

        // Label underneath: bold 9px Menlo
        auto f = owned (new CFontDesc ("Menlo", 9, kBoldFace));
        ctx->setFont (f);
        ctx->setFontColor (active ? CColor (238, 242, 246, 255) : CColor (174, 181, 190, 255));
        CRect textRect (r.left - 6.0, cy + radius + 3.0, r.right + 6.0, r.bottom);
        ctx->drawString (labelText.c_str (), textRect, kCenterText);
    }

    CLASS_METHODS_NOCOPY (RetroRoundButton, CControl)

private:
    std::string labelText;
    CColor activeColor;
};

//-----------------------------------------------------------------------------
// RetroLoadButton: Small rectangular brushed metallic LOAD pushbutton
//-----------------------------------------------------------------------------
class RetroLoadButton : public CControl
{
public:
    RetroLoadButton (const CRect& size, IControlListener* listener, int32_t tag)
    : CControl (size, listener, tag)
    {
    }

    CMouseEventResult onMouseDown (CPoint&, const CButtonState& buttons) override
    {
        if (!buttons.isLeftButton ())
            return kMouseEventNotHandled;

        beginEdit ();
        isPressed = true;
        invalid ();
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseUp (CPoint&, const CButtonState&) override
    {
        if (isPressed)
        {
            isPressed = false;
            setValueNormalized (1.0f);
            valueChanged ();
            setValueNormalized (0.0f);
            endEdit ();
            invalid ();
            return kMouseEventHandled;
        }
        return kMouseEventNotHandled;
    }

    CMouseEventResult onMouseCancel () override
    {
        if (isPressed)
        {
            isPressed = false;
            endEdit ();
            invalid ();
            return kMouseEventHandled;
        }
        return kMouseEventNotHandled;
    }

    void draw (CDrawContext* ctx) override
    {
        const CRect r = getViewSize ();
        if (auto path = owned (ctx->createGraphicsPath ()))
        {
            path->addRoundRect (r, 2.5);
            const CColor c1 = isPressed ? CColor (58, 61, 66, 255) : CColor (90, 95, 102, 255); // #5a5f66
            const CColor c2 = isPressed ? CColor (31, 33, 37, 255) : CColor (47, 50, 55, 255);  // #2f3237
            auto grad = owned (CGradient::create (0.0, 1.0, c1, c2));
            ctx->fillLinearGradient (path, *grad, r.getTopLeft (), r.getBottomLeft ());

            ctx->setFrameColor (CColor (21, 23, 26, 255)); // #15171a
            ctx->setLineWidth (1.0);
            ctx->drawGraphicsPath (path, CDrawContext::kPathStroked);
        }

        auto f = owned (new CFontDesc ("Menlo", 8.5, kBoldFace));
        ctx->setFont (f);
        ctx->setFontColor (CColor (236, 239, 243, 255)); // #eceff3
        CRect tr = r;
        tr.top += 0.5;
        ctx->drawString ("LOAD", tr, kCenterText);
    }

    CLASS_METHODS_NOCOPY (RetroLoadButton, CControl)

private:
    bool isPressed {false};
};

//-----------------------------------------------------------------------------
// RetroTVChassisView: Vented plinth, double silver-black-silver rim,
// Multi-finish wood faceplate, lower module & Speaker Grille / Service Hatch
//-----------------------------------------------------------------------------
class RetroTVChassisView : public CView
{
public:
    explicit RetroTVChassisView (const CRect& size, RetroTVEditor* owner)
    : CView (size)
    , editor (owner)
    {
    }

    void setMuted (bool m)
    {
        if (muted != m)
        {
            muted = m;
            invalid ();
        }
    }

    void setTeletextPage (int p)
    {
        if (teletextPage != p)
        {
            teletextPage = p;
            invalid ();
        }
    }

    void setGeometries (const CRect& cab, const CRect& grv, const CRect& silv, const CRect& wood, const CRect& by)
    {
        cabinetRect = cab;
        grooveRect = grv;
        silverInnerRect = silv;
        woodFaceRect = wood;
        bayRect = by;
        invalid ();
    }

    CMouseEventResult onMouseDown (CPoint& where, const CButtonState& buttons) override
    {
        if (!buttons.isLeftButton () || bayRect.isEmpty ())
            return kMouseEventNotHandled;

        const CCoord lowerY = computeLowerModuleY (bayRect);
        const CCoord grY = lowerY + 52.0 + 4.0;
        const CCoord grH = bayRect.bottom - grY;
        if (grH <= 12.0)
            return kMouseEventNotHandled;

        const CRect grRect (bayRect.left, grY, bayRect.right, bayRect.bottom);
        if (!grRect.pointInside (where))
            return kMouseEventNotHandled;

        if (!serviceOpen)
        {
            // Clicking closed speaker grille pops off the cover to reveal the Service Hatch!
            serviceOpen = true;
            invalid ();
            return kMouseEventHandled;
        }

        // Inside open Service Hatch:
        // 1. Close-lid button [×] at top-right (14x11px)
        const CRect closeBtn (bayRect.right - 17.0, grY + 3.0, bayRect.right - 3.0, grY + 14.0);
        if (closeBtn.pointInside (where))
        {
            serviceOpen = false;
            invalid ();
            return kMouseEventHandled;
        }

        const CCoord padIn = 5.0;
        const CCoord innerW = bayRect.getWidth () - padIn * 2.0;

        // 2. Row 1: Trimmers [-1F][0s][+1F][CRT] at r1Y = grY + 21
        const CCoord r1Y = grY + 21.0;
        const CCoord trimW = std::floor ((innerW - 14.0) / 4.0);
        for (int i = 0; i < 4; ++i)
        {
            const CCoord bx = bayRect.left + padIn + i * (trimW + 4.0);
            const CRect trimHit (bx - 1.0, r1Y - 1.0, bx + trimW + 1.0, r1Y + 19.0);
            if (trimHit.pointInside (where))
            {
                if (editor)
                {
                    if (i == 0) editor->nudgeOffset (-1);
                    else if (i == 1) editor->resetOffset ();
                    else if (i == 2) editor->nudgeOffset (+1);
                    else if (i == 3) editor->toggleCrtFx ();
                }
                invalid ();
                return kMouseEventHandled;
            }
        }

        // 3. Row 2: Channel switcher [CHANNEL 1][CHANNEL 2] at r2Y = grY + 43
        const CCoord r2Y = grY + 43.0;
        const CCoord chGap = 4.0;
        const CCoord chW = (innerW - chGap) * 0.5;
        const CCoord chH = 12.0;
        for (int i = 0; i < 2; ++i)
        {
            const CCoord cxB = bayRect.left + padIn + i * (chW + chGap);
            const CRect chHit (cxB, r2Y, cxB + chW, r2Y + chH);
            if (chHit.pointInside (where))
            {
                if (editor)
                    editor->setActiveChannel (i == 0 ? 1 : 2);
                invalid ();
                return kMouseEventHandled;
            }
        }

        // 4. Row 3: Teletext Fastext [Red][Green][Yellow][Blue] at r3Y = grY + grH - 13
        const CCoord r3Y = grY + grH - 13.0;
        const CCoord ttGap = 4.0;
        const CCoord ttW = (innerW - ttGap * 3.0) / 4.0;
        const CCoord ttH = 9.0;
        for (int i = 0; i < 4; ++i)
        {
            const CCoord tx = bayRect.left + padIn + i * (ttW + ttGap);
            const CRect ttHit (tx, r3Y - 1.0, tx + ttW, r3Y + ttH + 2.0);
            if (ttHit.pointInside (where))
            {
                if (editor)
                    editor->setTeletextPage (i);
                invalid ();
                return kMouseEventHandled;
            }
        }

        return kMouseEventHandled;
    }

    void draw (CDrawContext* ctx) override
    {
        const CRect bounds = getViewSize ();
        const CCoord W = bounds.getWidth ();
        const CCoord H = bounds.getHeight ();
        const CCoord bodyH = H - 16.0;

        // 0. Base dark backdrop #111214
        ctx->setFillColor (CColor (17, 18, 20, 255));
        ctx->drawRect (bounds, kDrawFilled);

        // 1. Bottom tapered vented plinth foot
        if (auto plinthPath = owned (ctx->createGraphicsPath ()))
        {
            plinthPath->beginSubpath (28.0, bodyH - 2.0);
            plinthPath->addLine (W - 28.0, bodyH - 2.0);
            plinthPath->addLine (W - 40.0, H - 3.0);
            plinthPath->addLine (40.0, H - 3.0);
            plinthPath->closeSubpath ();

            auto footG = owned (CGradient::create (0.0, 1.0, CColor (20, 21, 23, 255), CColor (38, 40, 43, 255)));
            ctx->fillLinearGradient (plinthPath, *footG, CPoint (0, bodyH), CPoint (0, H));

            ctx->setFillColor (CColor (9, 10, 11, 255)); // #090a0b
            for (CCoord vx = 48.0; vx < W - 48.0; vx += 6.0)
            {
                ctx->drawRect (CRect (vx, bodyH + 3.0, vx + 2.5, bodyH + 9.0), kDrawFilled);
            }
        }

        // 2. Outer silver frame #dfe4eb -> #aeb5be -> #858c96 -> #666d75 -> #bcc2ca
        if (auto cabPath = owned (ctx->createGraphicsPath ()))
        {
            cabPath->addRoundRect (cabinetRect, 9.0);
            auto silverG = owned (CGradient::create (0.0, 1.0, CColor (223, 228, 235, 255), CColor (188, 194, 202, 255)));
            silverG->addColorStop (0.10, CColor (174, 181, 190, 255));
            silverG->addColorStop (0.50, CColor (133, 140, 150, 255));
            silverG->addColorStop (0.90, CColor (102, 109, 117, 255));
            ctx->fillLinearGradient (cabPath, *silverG, cabinetRect.getTopLeft (), cabinetRect.getBottomLeft ());

            ctx->setFrameColor (CColor (54, 58, 64, 255)); // #363a40
            ctx->setLineWidth (1.0);
            ctx->drawGraphicsPath (cabPath, CDrawContext::kPathStroked);
        }

        // 3. Black groove #0d0e10
        drawRoundedRect (ctx, grooveRect, 7.0, CColor (13, 14, 16, 255), CColor (0, 0, 0, 0));

        // 4. Scalable Studio Silver brushed vector gradient faceplate (seamlessly scales to any size)
        if (auto facePath = owned (ctx->createGraphicsPath ()))
        {
            facePath->addRoundRect (woodFaceRect, 5.0);
            auto faceG = owned (CGradient::create (0.0, 1.0, CColor (125, 131, 141, 255), CColor (68, 73, 80, 255))); // #7d838d -> #444950
            faceG->addColorStop (0.5, CColor (95, 101, 110, 255)); // #5f656e
            ctx->fillLinearGradient (facePath, *faceG, woodFaceRect.getTopLeft (), woodFaceRect.getBottomLeft ());

            // Soft radial satin sheen
            const CPoint sheenCenter (woodFaceRect.left + woodFaceRect.getWidth () * 0.42,
                                      woodFaceRect.top + woodFaceRect.getHeight () * 0.25);
            const CCoord sheenR = std::max (woodFaceRect.getWidth (), woodFaceRect.getHeight ()) * 0.75;
            auto sheenG = owned (CGradient::create (0.0, 1.0, CColor (255, 255, 255, 33), CColor (0, 0, 0, 56)));
            ctx->fillRadialGradient (facePath, *sheenG, sheenCenter, sheenR);

            // Subtle inner inset edge shadow under silver rim
            ctx->setFrameColor (CColor (0, 0, 0, 115));
            ctx->setLineWidth (2.0);
            ctx->drawGraphicsPath (facePath, CDrawContext::kPathStroked);
        }

        // 5. Inner silver trim framing the wood face #cfd5dd -> #81878f -> #b5bbc3
        if (auto innerTrimPath = owned (ctx->createGraphicsPath ()))
        {
            const CRect trimR (silverInnerRect.left + 1.0, silverInnerRect.top + 1.0,
                               silverInnerRect.right - 1.0, silverInnerRect.bottom - 1.0);
            innerTrimPath->addRoundRect (trimR, 5.5);
            auto trimG = owned (CGradient::create (0.0, 1.0, CColor (207, 213, 221, 255), CColor (181, 187, 195, 255)));
            trimG->addColorStop (0.5, CColor (129, 135, 143, 255));
            ctx->setFrameColor (CColor (181, 187, 195, 255));
            ctx->setLineWidth (2.2);
            ctx->drawGraphicsPath (innerTrimPath, CDrawContext::kPathStroked);
        }

        // 6. Lower control module
        if (!bayRect.isEmpty ())
        {
            const CCoord lowerY = computeLowerModuleY (bayRect);
            const CCoord lowerH = bayRect.bottom - lowerY;

            // Drop shadow behind lower module
            drawRoundedRect (ctx, CRect (bayRect.left - 2.0, lowerY - 2.0, bayRect.right + 2.0, lowerY + lowerH + 2.0),
                             4.0, CColor (0, 0, 0, 166), CColor (0, 0, 0, 0));

            // Dark recess #141518
            drawRoundedRect (ctx, CRect (bayRect.left - 1.0, lowerY - 1.0, bayRect.right + 1.0, lowerY + lowerH + 1.0),
                             3.0, CColor (20, 21, 24, 255), CColor (0, 0, 0, 0));

            const CCoord midY = lowerY;
            const CCoord midH = 52.0;
            const CRect midStrip (bayRect.left, midY, bayRect.right, midY + midH);

            // Mid strip metallic gradient #3a3d42 to #26282c
            if (auto midPath = owned (ctx->createGraphicsPath ()))
            {
                midPath->addRoundRect (midStrip, 2.0);
                auto midG = owned (CGradient::create (0.0, 1.0, CColor (58, 61, 66, 255), CColor (38, 40, 44, 255)));
                ctx->fillLinearGradient (midPath, *midG, midStrip.getTopLeft (), midStrip.getBottomLeft ());

                ctx->setFrameColor (CColor (18, 19, 21, 255)); // #121315
                ctx->setLineWidth (1.0);
                ctx->drawGraphicsPath (midPath, CDrawContext::kPathStroked);
            }

            // Red diode LED at (midStrip.left + 24, midY + 20)
            const CPoint ledCenter (midStrip.left + 24.0, midY + 20.0);
            if (muted)
            {
                ctx->setFillColor (CColor (66, 22, 22, 255)); // dim #421616
                ctx->drawEllipse (CRect (ledCenter.x - 2.8, ledCenter.y - 2.8, ledCenter.x + 2.8, ledCenter.y + 2.8), kDrawFilled);
            }
            else
            {
                // Glow halo
                if (auto glowPath = owned (ctx->createGraphicsPath ()))
                {
                    glowPath->addEllipse (CRect (ledCenter.x - 7.0, ledCenter.y - 7.0, ledCenter.x + 7.0, ledCenter.y + 7.0));
                    auto glowG = owned (CGradient::create (0.0, 1.0, CColor (255, 61, 46, 120), CColor (255, 61, 46, 0)));
                    ctx->fillRadialGradient (glowPath, *glowG, ledCenter, 7.0);
                }
                // Diode center #ff3d2e
                ctx->setFillColor (CColor (255, 61, 46, 255));
                ctx->drawEllipse (CRect (ledCenter.x - 2.8, ledCenter.y - 2.8, ledCenter.x + 2.8, ledCenter.y + 2.8), kDrawFilled);
                ctx->setFillColor (CColor (255, 200, 190, 255));
                ctx->drawEllipse (CRect (ledCenter.x - 1.0, ledCenter.y - 1.0, ledCenter.x + 0.5, ledCenter.y + 0.5), kDrawFilled);
            }

            // Speaker Grille / Service Hatch below mid strip: grY to bayRect.bottom
            const CCoord grY = midY + midH + 4.0;
            const CCoord grH = bayRect.bottom - grY;
            if (grH > 12.0)
            {
                CRect grRect (bayRect.left, grY, bayRect.right, bayRect.bottom);

                if (!serviceOpen)
                {
                    // Closed Louvered Speaker Grille
                    drawRoundedRect (ctx, grRect, 2.0, CColor (16, 17, 19, 255), CColor (93, 98, 105, 255), 1.0);

                    for (CCoord ly = grY + 4.0; ly < grRect.bottom - 4.0; ly += 4.0)
                    {
                        ctx->setFillColor (CColor (174, 180, 188, 255)); // #aeb4bc
                        ctx->drawRect (CRect (bayRect.left + 3.0, ly, bayRect.right - 3.0, ly + 1.4), kDrawFilled);

                        ctx->setFillColor (CColor (7, 8, 9, 255)); // #070809
                        ctx->drawRect (CRect (bayRect.left + 3.0, ly + 1.4, bayRect.right - 3.0, ly + 3.4), kDrawFilled);
                    }
                }
                else
                {
                    // Popped-off Speaker Grille -> Recessed Dark Service Hatch Cavity!
                    drawRoundedRect (ctx, grRect, 2.0, CColor (11, 12, 14, 255), CColor (59, 63, 71, 255), 1.0);

                    // Enlarged Close-lid button [×] at top-right (14x11px, y: grY + 3)
                    const CRect closeBtn (bayRect.right - 17.0, grY + 3.0, bayRect.right - 3.0, grY + 14.0);
                    drawRoundedRect (ctx, closeBtn, 2.0, CColor (37, 40, 45, 255), CColor (71, 76, 86, 255), 0.8);
                    auto xFont = owned (new CFontDesc ("Menlo", 8.5, kBoldFace));
                    ctx->setFont (xFont);
                    ctx->setFontColor (CColor (216, 222, 232, 255));
                    CRect xTextR = closeBtn;
                    xTextR.top += 0.5;
                    ctx->drawString ("×", xTextR, kCenterText);

                    // Lowered SERVICE label at y: grY + 11
                    auto sFont = owned (new CFontDesc ("Menlo", 6.5, kBoldFace));
                    ctx->setFont (sFont);
                    ctx->setFontColor (CColor (111, 119, 130, 255));
                    ctx->drawString ("SERVICE", CRect (bayRect.left + 5.0, grY + 11.0, bayRect.left + 60.0, grY + 20.0), kLeftText);

                    const CCoord padIn = 5.0;
                    const CCoord innerW = bayRect.getWidth () - padIn * 2.0;

                    // Row 1: Small white tactile trimmer buttons [-1F][0s][+1F] + CRT toggle (y: grY + 21)
                    const CCoord r1Y = grY + 21.0;
                    const CCoord trimW = std::floor ((innerW - 14.0) / 4.0);
                    const CCoord bh = 9.0;
                    const bool crtActive = editor ? editor->isCrtFxEnabled () : true;
                    struct TrimDef {
                        const char* lbl;
                        bool white;
                        bool active;
                    } trims[4] = {
                        { "-1F", true,  false },
                        { "0s",  true,  false },
                        { "+1F", true,  false },
                        { "CRT", false, crtActive }
                    };

                    auto trFont = owned (new CFontDesc ("Menlo", 7.2, kBoldFace));
                    ctx->setFont (trFont);
                    for (int i = 0; i < 4; ++i)
                    {
                        const CCoord bx = bayRect.left + padIn + i * (trimW + 4.0);
                        const CRect trRect (bx, r1Y, bx + trimW, r1Y + bh);
                        if (trims[i].white)
                        {
                            if (auto trPath = owned (ctx->createGraphicsPath ()))
                            {
                                trPath->addRoundRect (trRect, 1.5);
                                auto wg = owned (CGradient::create (0.0, 1.0, CColor (255, 255, 255, 255), CColor (207, 212, 220, 255)));
                                ctx->fillLinearGradient (trPath, *wg, trRect.getTopLeft (), trRect.getBottomLeft ());
                                ctx->setFrameColor (CColor (24, 26, 30, 255));
                                ctx->setLineWidth (0.8);
                                ctx->drawGraphicsPath (trPath, CDrawContext::kPathStroked);
                            }
                        }
                        else
                        {
                            const CColor c = trims[i].active ? CColor (240, 165, 64, 255) : CColor (53, 56, 62, 255);
                            drawRoundedRect (ctx, trRect, 1.5, c, CColor (24, 26, 30, 255), 0.8);
                        }

                        // White label underneath
                        ctx->setFontColor (CColor (255, 255, 255, 255));
                        CRect trLblR (bx - 2.0, r1Y + bh + 1.0, bx + trimW + 2.0, r1Y + bh + 11.0);
                        ctx->drawString (trims[i].lbl, trLblR, kCenterText);
                    }

                    // Row 2: Channel switcher radio buttons CHANNEL 1 (Easter Egg) / CHANNEL 2 (Default DAW video)
                    const CCoord r2Y = grY + 43.0;
                    const CCoord chGap = 4.0;
                    const CCoord chW = (innerW - chGap) * 0.5;
                    const CCoord chH = 12.0;
                    const int activeCh = editor ? editor->getActiveChannel () : 2;
                    const char* chLabels[2] = { "CHANNEL 1", "CHANNEL 2" };
                    auto chFont = owned (new CFontDesc ("Menlo", 7.0, kBoldFace));
                    ctx->setFont (chFont);

                    for (int i = 0; i < 2; ++i)
                    {
                        const CCoord cxB = bayRect.left + padIn + i * (chW + chGap);
                        const CRect chRect (cxB, r2Y, cxB + chW, r2Y + chH);
                        const bool active = (activeCh == (i == 0 ? 1 : 2));
                        const CColor bg = active ? CColor (230, 157, 60, 255) : CColor (38, 41, 47, 255);
                        const CColor strk = active ? CColor (255, 208, 137, 255) : CColor (64, 69, 79, 255);
                        drawRoundedRect (ctx, chRect, 2.0, bg, strk, 0.8);

                        ctx->setFontColor (active ? CColor (21, 18, 13, 255) : CColor (183, 190, 201, 255));
                        CRect chTextR = chRect;
                        chTextR.top += 0.5;
                        ctx->drawString (chLabels[i], chTextR, kCenterText);
                    }

                    // Row 3: 4 Teletext Fastext color buttons (y: grY + grH - 13)
                    const CCoord r3Y = grY + grH - 13.0;
                    const CCoord ttGap = 4.0;
                    const CCoord ttW = (innerW - ttGap * 3.0) / 4.0;
                    const CCoord ttH = 9.0;
                    struct TTBtnDef {
                        CColor fill;
                        CColor hi;
                    } ttBtns[4] = {
                        { CColor (217, 56, 46, 255),  CColor (255, 112, 102, 255) }, // Red
                        { CColor (42, 166, 74, 255),  CColor (104, 255, 156, 255) }, // Green
                        { CColor (223, 178, 34, 255), CColor (255, 231, 112, 255) }, // Yellow
                        { CColor (45, 114, 214, 255), CColor (120, 180, 255, 255) }  // Blue
                    };

                    for (int i = 0; i < 4; ++i)
                    {
                        const CCoord tx = bayRect.left + padIn + i * (ttW + ttGap);
                        const CRect tbRect (tx, r3Y, tx + ttW, r3Y + ttH);
                        const bool active = (teletextPage == i);
                        const CColor bg = active ? ttBtns[i].hi : ttBtns[i].fill;
                        const CColor strk = active ? CColor (255, 255, 255, 255) : CColor (17, 18, 22, 255);
                        drawRoundedRect (ctx, tbRect, 2.0, bg, strk, active ? 1.2 : 0.8);
                    }
                }
            }
        }
    }

private:
    RetroTVEditor* editor {nullptr};
    bool serviceOpen {false};
    int teletextPage {-1};
    CRect cabinetRect;
    CRect grooveRect;
    CRect silverInnerRect;
    CRect woodFaceRect;
    CRect bayRect;
    bool muted {false};
};

//-----------------------------------------------------------------------------
// RetroCRTView: 4-Wall Mitered Bezel Funnel, Even-Odd Screen Masking,
// Video Frame, CRT Scanlines/Vignette/Glare & 3-Corner VCR OSD + Teletext
//-----------------------------------------------------------------------------
class RetroCRTView : public CView
{
public:
    explicit RetroCRTView (RetroTVEditor* owner)
    : CView (CRect (0, 0, 100, 100))
    , editor (owner)
    {
    }

    SharedPointer<IDropTarget> getDropTarget () override
    {
        class DropHandler : public DropTargetAdapter, public AtomicReferenceCounted
        {
        public:
            explicit DropHandler (RetroCRTView* v) : view (v) {}

            DragOperation onDragEnter (DragEventData data) override
            {
                view->setDragHighlight (true);
                return hasFile (data.drag) ? DragOperation::Copy : DragOperation::None;
            }
            DragOperation onDragMove (DragEventData data) override
            {
                return hasFile (data.drag) ? DragOperation::Copy : DragOperation::None;
            }
            void onDragLeave (DragEventData) override
            {
                view->setDragHighlight (false);
            }
            bool onDrop (DragEventData data) override
            {
                view->setDragHighlight (false);
                if (!data.drag)
                    return false;
                const uint32_t count = data.drag->getCount ();
                for (uint32_t i = 0; i < count; ++i)
                {
                    const void* buffer = nullptr;
                    IDataPackage::Type type = IDataPackage::kError;
                    const uint32_t size = data.drag->getData (i, buffer, type);
                    if (type == IDataPackage::kFilePath && buffer && size > 0)
                    {
                        const char* str = static_cast<const char*> (buffer);
                        std::string path (str, strnlen (str, size));
                        if (!path.empty () && view->editor)
                        {
                            view->editor->handleDroppedFile (path);
                            return true;
                        }
                    }
                }
                return false;
            }

            static bool hasFile (IDataPackage* pkg)
            {
                if (!pkg)
                    return false;
                for (uint32_t i = 0; i < pkg->getCount (); ++i)
                {
                    if (pkg->getDataType (i) == IDataPackage::kFilePath)
                        return true;
                }
                return false;
            }

            RetroCRTView* view;
        };

        return makeOwned<DropHandler> (this);
    }

    void setDragHighlight (bool active)
    {
        if (dragHover != active)
        {
            dragHover = active;
            invalid ();
        }
    }

    void setCrtFx (bool enabled)
    {
        if (crtFx != enabled)
        {
            crtFx = enabled;
            invalid ();
        }
    }

    void setOsdEnabled (bool enabled)
    {
        if (osdEnabled != enabled)
        {
            osdEnabled = enabled;
            invalid ();
        }
    }

    void setTeletextPage (int p)
    {
        if (teletextPage != p)
        {
            teletextPage = p;
            invalid ();
        }
    }

    void setMuted (bool m)
    {
        if (muted != m)
        {
            muted = m;
            invalid ();
        }
    }

    void setVolume (float v)
    {
        if (std::fabs (volume - v) > 0.005f)
        {
            volume = v;
            invalid ();
        }
    }

    void setOffsetSeconds (double off)
    {
        if (std::fabs (offsetSec - off) > 0.005)
        {
            offsetSec = off;
            invalid ();
        }
    }

    void setChannelOne (bool ch1)
    {
        if (channelOne != ch1)
        {
            channelOne = ch1;
            invalid ();
        }
    }

    void updateTransport (bool playing, double dawTimeSec, double fpsVal, bool hasVid, bool outOfRng, const std::string& err)
    {
        isPlaying = playing;
        videoPtsSec = dawTimeSec;
        fps = fpsVal;
        hasVideo = hasVid;
        outOfRange = outOfRng;
        errorText = err;
        invalid ();
    }

    PixelOrder queryPreferredPixelOrder ()
    {
        if (!bitmap)
            bitmap = owned (new CBitmap (32, 32));
        if (auto pb = bitmap->getPlatformBitmap ())
        {
            if (auto access = pb->lockPixels (true))
                return fromVstGuiPixelFormat (access->getPixelFormat ());
        }
#if defined(_WIN32)
        return PixelOrder::BGRA;
#else
        return PixelOrder::ARGB;
#endif
    }

    void updateFromSnapshot (const VideoFrameSnapshot& snap)
    {
        hasVideo = snap.hasVideo;
        outOfRange = snap.outOfRange;
        videoPtsSec = snap.videoPtsSec;
        fps = snap.fps;
        errorText = snap.errorMessage;

        if (snap.hasVideo && !snap.outOfRange && snap.width > 0 && snap.height > 0 && !snap.pixels.empty ())
        {
            if (!bitmap || static_cast<int> (bitmap->getWidth ()) != snap.width ||
                static_cast<int> (bitmap->getHeight ()) != snap.height)
            {
                bitmap = owned (new CBitmap (snap.width, snap.height));
            }

            if (auto pb = bitmap->getPlatformBitmap ())
            {
                if (auto access = pb->lockPixels (true))
                {
                    uint8_t* dst = access->getAddress ();
                    const uint32_t dstStride = access->getBytesPerRow ();
                    const int srcStride = snap.strideBytes;
                    const uint32_t copyBytes = std::min<uint32_t> (dstStride, static_cast<uint32_t> (srcStride));

                    if (dst && fromVstGuiPixelFormat (access->getPixelFormat ()) == snap.pixelOrder)
                    {
                        for (int y = 0; y < snap.height; ++y)
                        {
                            std::memcpy (dst + static_cast<size_t> (y) * dstStride,
                                         snap.pixels.data () + static_cast<size_t> (y) * srcStride,
                                         copyBytes);
                        }
                    }
                    else if (dst)
                    {
                        if (auto pixelAccess = owned (CBitmapPixelAccess::create (bitmap, true)))
                        {
                            for (int y = 0; y < snap.height; ++y)
                            {
                                pixelAccess->setPosition (0, static_cast<uint32_t> (y));
                                const uint8_t* row = snap.pixels.data () + static_cast<size_t> (y) * srcStride;
                                for (int x = 0; x < snap.width; ++x)
                                {
                                    const uint8_t* p = row + x * 4;
                                    CColor c (p[1], p[2], p[3], 255);
                                    pixelAccess->setColor (c);
                                    ++(*pixelAccess);
                                }
                            }
                        }
                    }
                }
            }
        }
        invalid ();
    }

    CMouseEventResult onMouseDown (CPoint&, const CButtonState& buttons) override
    {
        if (buttons.isLeftButton () && buttons.isDoubleClick () && editor)
        {
            editor->toggleCrtFx ();
            return kMouseEventHandled;
        }
        return kMouseEventNotHandled;
    }

    void draw (CDrawContext* ctx) override
    {
        const CRect bounds = getViewSize ();
        const CCoord bx0 = bounds.left;
        const CCoord by0 = bounds.top;
        const CCoord bx1 = bounds.right;
        const CCoord by1 = bounds.bottom;

        // 1. Mounted drop shadow behind CRT monitor bezel onto the wood
        drawRoundedRect (ctx, CRect (bx0 - 2.0, by0 - 2.0, bx1 + 2.0, by1 + 2.0),
                         4.0, CColor (0, 0, 0, 166), CColor (0, 0, 0, 0));

        // Inset screen opening
        const CCoord funnelInset = std::clamp (bounds.getWidth () * 0.048, 15.0, 22.0);
        const CRect screen (bounds.left + funnelInset,
                            bounds.top + funnelInset * 0.82,
                            bounds.right - funnelInset,
                            bounds.bottom - funnelInset * 0.82);
        const CCoord sx = screen.left;
        const CCoord sy = screen.top;
        const CCoord sw = screen.getWidth ();
        const CCoord sh = screen.getHeight ();
        const CCoord r = 16.0;
        const CCoord bow = 4.5;

        // 2. Video & CRT Glass Interior (expanded by bow so bezel walls mask corners and bowed edges seamlessly)
        const CRect screenBowed (sx - bow, sy - bow, sx + sw + bow, sy + sh + bow);
        ctx->saveGlobalState ();
        {
            CRect screenClip;
            ctx->getClipRect (screenClip);
            screenClip.bound (screenBowed);
            ctx->setClipRect (screenClip);

            // Black screen background
            ctx->setFillColor (CColor (0, 0, 0, 255));
            ctx->drawRect (screenBowed, kDrawFilled);

            if (teletextPage >= 0 && teletextPage < 4)
            {
                // Teletext Fastext Easter Egg display!
                drawTeletextPage (ctx, screen, teletextPage);
            }
            else
            {
                // Normal video frame or test bars or no signal hint
                if (hasVideo && !outOfRange && bitmap && bitmap->getWidth () > 0 && bitmap->getHeight () > 0)
                {
                    const CCoord bmpW = bitmap->getWidth ();
                    const CCoord bmpH = bitmap->getHeight ();
                    const double imgAspect = bmpW / bmpH;
                    const double scrW = std::max (1.0, screenBowed.getWidth ());
                    const double scrH = std::max (1.0, screenBowed.getHeight ());
                    const double scrAspect = scrW / scrH;

                    CRect dest = screenBowed;
                    if (imgAspect > scrAspect)
                    {
                        const CCoord targetH = scrW / imgAspect;
                        const CCoord pad = (scrH - targetH) * 0.5;
                        dest.top += pad;
                        dest.bottom -= pad;
                    }
                    else
                    {
                        const CCoord targetW = scrH * imgAspect;
                        const CCoord pad = (scrW - targetW) * 0.5;
                        dest.left += pad;
                        dest.right -= pad;
                    }
                    dest.makeIntegral ();

                    const double scx = dest.getWidth () / bmpW;
                    const double scy = dest.getHeight () / bmpH;
                    if (scx > 0.0 && scy > 0.0)
                    {
                        ctx->saveGlobalState ();
                        {
                            CRect destClip;
                            ctx->getClipRect (destClip);
                            destClip.bound (dest);
                            ctx->setClipRect (destClip);
                            ctx->setBitmapInterpolationQuality (BitmapInterpolationQuality::kMedium);

                            CGraphicsTransform xform;
                            xform.scale (scx, scy);
                            xform.translate (dest.left, dest.top);
                            CDrawContext::Transform scopedTransform (*ctx, xform);
                            ctx->drawBitmap (bitmap, CRect (0.0, 0.0, bmpW, bmpH));
                        }
                        ctx->restoreGlobalState ();
                    }
                }
                else if (hasVideo && outOfRange)
                {
                    drawTestBars (ctx, screenBowed, true);
                    CRect badge (screen.left + 20, screen.top + 20, screen.left + 220, screen.top + 52);
                    drawRoundedRect (ctx, badge, 4.0, CColor (12, 16, 14, 210), CColor (240, 180, 70, 220), 1.2);
                    auto f = owned (new CFontDesc ("Menlo", 12, kBoldFace));
                    ctx->setFont (f);
                    ctx->setFontColor (CColor (245, 195, 85, 255));
                    const char* label = (videoPtsSec < 0.0) ? "PRE-ROLL (NO VIDEO)" : "END OF REEL";
                    ctx->drawString (label, badge, kCenterText);
                }
                else
                {
                    drawTestBars (ctx, screenBowed, false);
                    const CCoord cx = screen.getCenter ().x;
                    const CCoord cy = screen.getCenter ().y;
                    CRect promptBox (cx - 180.0, cy - 32.0, cx + 180.0, cy + 32.0);
                    drawRoundedRect (ctx, promptBox, 6.0, CColor (14, 16, 18, 225), CColor (225, 185, 110, 220), 1.4);

                    auto titleFont = owned (new CFontDesc ("Menlo", 12, kBoldFace));
                    auto subFont = owned (new CFontDesc ("Menlo", 9.5));
                    ctx->setFont (titleFont);
                    ctx->setFontColor (CColor (245, 232, 198, 255));
                    ctx->drawString ("RETRO-TV NO SIGNAL", CRect (promptBox.left, promptBox.top + 8, promptBox.right, promptBox.top + 28), kCenterText);

                    ctx->setFont (subFont);
                    ctx->setFontColor (errorText.empty () ? CColor (160, 220, 175, 255) : CColor (255, 120, 100, 255));
                    const std::string msg = errorText.empty () ? "DRAG VIDEO FILE HERE OR CLICK LOAD" : errorText;
                    ctx->drawString (msg.c_str (), CRect (promptBox.left + 8, promptBox.top + 32, promptBox.right - 8, promptBox.bottom - 6), kCenterText);
                }

                // VCR 3-Corner OSD (only when live video picture is visible)
                if (osdEnabled)
                {
                    drawOsd (ctx, screen);
                }
            }

            // Optional CRT scanlines, radial vignette, and glass sheen (applied over picture and Teletext)
            if (crtFx)
            {
                // Scanlines
                ctx->setFillColor (CColor (0, 0, 0, 56)); // rgba(0,0,0,0.22)
                for (CCoord y = screenBowed.top; y < screenBowed.bottom; y += 3.0)
                {
                    ctx->drawRect (CRect (screenBowed.left, y, screenBowed.right, y + 1.0), kDrawFilled);
                }

                // Radial vignette
                const CCoord cx = screen.getCenter ().x;
                const CCoord cy = screen.top + screen.getHeight () * 0.48;
                const CCoord vigRadius = std::max (screen.getWidth (), screen.getHeight ()) * 0.72;
                if (auto vigPath = owned (ctx->createGraphicsPath ()))
                {
                    vigPath->addRect (screenBowed);
                    auto vigG = owned (CGradient::create (0.0, 1.0, CColor (255, 255, 255, 5), CColor (0, 0, 0, 140)));
                    vigG->addColorStop (0.68, CColor (0, 0, 0, 0));
                    ctx->fillRadialGradient (vigPath, *vigG, CPoint (cx, cy), vigRadius);
                }

                // Diagonal glass glare sheen
                if (auto sheenPath = owned (ctx->createGraphicsPath ()))
                {
                    sheenPath->addRect (screenBowed);
                    auto sheenG = owned (CGradient::create (0.0, 1.0, CColor (255, 255, 255, 33), CColor (255, 255, 255, 0)));
                    sheenG->addColorStop (0.5, CColor (255, 255, 255, 5));
                    ctx->fillLinearGradient (sheenPath, *sheenG, screen.getTopLeft (),
                                             CPoint (screen.left + screen.getWidth () * 0.45, screen.top + screen.getHeight () * 0.45));
                }
            }
        }
        ctx->restoreGlobalState ();

        // 3. 4-Wall Mitered 3D CRT Bezel Funnel (4 non-overlapping closed wall polygons meeting at corner midpoints)
        const CPoint tlM (sx + 0.25 * r, sy + 0.25 * r);
        const CPoint trM (sx + sw - 0.25 * r, sy + 0.25 * r);
        const CPoint brM (sx + sw - 0.25 * r, sy + sh - 0.25 * r);
        const CPoint blM (sx + 0.25 * r, sy + sh - 0.25 * r);

        // Top Wall:
        if (auto topPath = owned (ctx->createGraphicsPath ()))
        {
            topPath->beginSubpath (bx0, by0);
            topPath->addLine (bx1, by0);
            topPath->addLine (trM);
            addQuadBezier (topPath, trM, CPoint (sx + sw - 0.5 * r, sy), CPoint (sx + sw - r, sy));
            addQuadBezier (topPath, CPoint (sx + sw - r, sy), CPoint (sx + sw * 0.5, sy - bow), CPoint (sx + r, sy));
            addQuadBezier (topPath, CPoint (sx + r, sy), CPoint (sx + 0.5 * r, sy), tlM);
            topPath->closeSubpath ();

            auto topG = owned (CGradient::create (0.0, 1.0, CColor (30, 32, 35, 255), CColor (43, 45, 49, 255)));
            ctx->fillLinearGradient (topPath, *topG, CPoint (bx0, by0), CPoint (bx0, sy));
        }

        // Bottom Wall:
        if (auto botPath = owned (ctx->createGraphicsPath ()))
        {
            botPath->beginSubpath (bx1, by1);
            botPath->addLine (bx0, by1);
            botPath->addLine (blM);
            addQuadBezier (botPath, blM, CPoint (sx + 0.5 * r, sy + sh), CPoint (sx + r, sy + sh));
            addQuadBezier (botPath, CPoint (sx + r, sy + sh), CPoint (sx + sw * 0.5, sy + sh + bow), CPoint (sx + sw - r, sy + sh));
            addQuadBezier (botPath, CPoint (sx + sw - r, sy + sh), CPoint (sx + sw - 0.5 * r, sy + sh), brM);
            botPath->closeSubpath ();

            auto botG = owned (CGradient::create (0.0, 1.0, CColor (114, 119, 126, 255), CColor (61, 64, 69, 255)));
            botG->addColorStop (0.55, CColor (91, 96, 103, 255));
            ctx->fillLinearGradient (botPath, *botG, CPoint (bx0, sy + sh), CPoint (bx0, by1));
        }

        // Left Wall:
        if (auto leftPath = owned (ctx->createGraphicsPath ()))
        {
            leftPath->beginSubpath (bx0, by1);
            leftPath->addLine (bx0, by0);
            leftPath->addLine (tlM);
            addQuadBezier (leftPath, tlM, CPoint (sx, sy + 0.5 * r), CPoint (sx, sy + r));
            addQuadBezier (leftPath, CPoint (sx, sy + r), CPoint (sx - bow, sy + sh * 0.5), CPoint (sx, sy + sh - r));
            addQuadBezier (leftPath, CPoint (sx, sy + sh - r), CPoint (sx, sy + sh - 0.5 * r), blM);
            leftPath->closeSubpath ();

            auto leftG = owned (CGradient::create (0.0, 1.0, CColor (58, 61, 65, 255), CColor (88, 92, 98, 255)));
            ctx->fillLinearGradient (leftPath, *leftG, CPoint (bx0, by0), CPoint (sx, by0));
        }

        // Right Wall:
        if (auto rightPath = owned (ctx->createGraphicsPath ()))
        {
            rightPath->beginSubpath (bx1, by0);
            rightPath->addLine (bx1, by1);
            rightPath->addLine (brM);
            addQuadBezier (rightPath, brM, CPoint (sx + sw, sy + sh - 0.5 * r), CPoint (sx + sw, sy + sh - r));
            addQuadBezier (rightPath, CPoint (sx + sw, sy + sh - r), CPoint (sx + sw + bow, sy + sh * 0.5), CPoint (sx + sw, sy + r));
            addQuadBezier (rightPath, CPoint (sx + sw, sy + r), CPoint (sx + sw, sy + 0.5 * r), trM);
            rightPath->closeSubpath ();

            auto rightG = owned (CGradient::create (0.0, 1.0, CColor (88, 92, 98, 255), CColor (56, 59, 63, 255)));
            ctx->fillLinearGradient (rightPath, *rightG, CPoint (sx + sw, by0), CPoint (bx1, by0));
        }

        // 45-degree diagonal corner miter seams
        ctx->setFrameColor (CColor (0, 0, 0, 115)); // rgba(0,0,0,0.45)
        ctx->setLineWidth (1.0);
        ctx->drawLine (CPoint (bx0, by0), tlM);
        ctx->drawLine (CPoint (bx1, by0), trM);
        ctx->drawLine (CPoint (bx0, by1), blM);
        ctx->drawLine (CPoint (bx1, by1), brM);

        // Inner CRT aperture lip stroke
        if (auto lipPath = owned (ctx->createGraphicsPath ()))
        {
            addCrtCutoutSubpath (lipPath, sx, sy, sw, sh, r, bow);
            const CColor lipColor = dragHover ? CColor (110, 235, 150, 255) : CColor (0, 0, 0, 217);
            ctx->setFrameColor (lipColor);
            ctx->setLineWidth (dragHover ? 3.5 : 3.0);
            ctx->drawGraphicsPath (lipPath, CDrawContext::kPathStroked);
        }

        // Outer bezel outline #4c5056
        ctx->setFrameColor (CColor (76, 80, 86, 255));
        ctx->setLineWidth (1.2);
        ctx->drawRect (CRect (bx0 + 0.5, by0 + 0.5, bx1 - 0.5, by1 - 0.5), kDrawStroked);
    }

private:
    void drawTeletextPage (CDrawContext* ctx, const CRect& screen, int page)
    {
        ctx->setFillColor (CColor (5, 6, 8, 255)); // #050608
        ctx->drawRect (screen, kDrawFilled);

        const CCoord pad = 16.0;
        const CCoord cw = screen.getWidth () - pad * 2.0;

        struct TeletextLine {
            const char* text;
            CColor color;
        };

        struct TeletextData {
            const char* num;
            const char* title;
            CColor bannerBg;
            CColor bannerFg;
            TeletextLine lines[8];
        };

        static const TeletextData kPages[4] = {
            // Page 0 (Red / P100)
            {
                "P100", "RETRO TV",
                CColor (200, 40, 36, 255), CColor (255, 242, 77, 255),
                {
                    { "VIDEO DISPLAYING PLUGIN FOR AUDIO DAWS", CColor (85, 255, 255, 255) },
                    { "----------------------------------------", CColor (51, 102, 170, 255) },
                    { "LOCKED PICTURE & AUDIO MONITOR DRIVEN BY", CColor (255, 255, 255, 255) },
                    { "YOUR DAW HOST TRANSPORT + NATIVE CODEC.", CColor (255, 255, 255, 255) },
                    { "", CColor (255, 255, 255, 255) },
                    { "> DRAG & DROP ANY VIDEO ONTO THE CRT",   CColor (104, 255, 156, 255) },
                    { "> SYNC WITH [OFS] + [-1F / 0s / +1F]",  CColor (104, 255, 156, 255) },
                    { "> CREATED BY FLX.AGENCY  //  FELIX.SK",  CColor (255, 216, 77, 255) }
                }
            },
            // Page 1 (Green / P200)
            {
                "P200", "f+l~x // AUTHOR",
                CColor (25, 135, 61, 255), CColor (255, 255, 255, 255),
                {
                    { "AUTHOR: f+l~x   //   WEB: felix.sk",     CColor (255, 216, 77, 255) },
                    { "----------------------------------------", CColor (46, 168, 79, 255) },
                    { "dYsleXic Xennial polYmath",             CColor (85, 255, 255, 255) },
                    { "How, when and what of who I am describe", CColor (255, 255, 255, 255) },
                    { "+ / ~ the same phenOmenON.",            CColor (255, 255, 255, 255) },
                    { "I’// let U do the mæth.",               CColor (104, 255, 156, 255) },
                    { "", CColor (255, 255, 255, 255) },
                    { "created by flx.agency   +/~",           CColor (255, 216, 77, 255) }
                }
            },
            // Page 2 (Yellow / P300)
            {
                "P300", "SERVICE & SYNC",
                CColor (184, 142, 11, 255), CColor (8, 9, 12, 255),
                {
                    { "SECRET SERVICE BAY CONTROLS:",          CColor (255, 216, 77, 255) },
                    { "----------------------------------------", CColor (170, 136, 34, 255) },
                    { "[-1F][0s][+1F]  EXACT 1-FRAME TRIM",    CColor (255, 255, 255, 255) },
                    { "[CRT]           SCANLINE + GLARE FX",   CColor (255, 255, 255, 255) },
                    { "[CHANNEL 1 / 2] CH1 EGG / CH2 DAW FEED", CColor (255, 255, 255, 255) },
                    { "", CColor (255, 255, 255, 255) },
                    { "TIP: SHIFT+DRAG DIALS FOR FINE TWEAK",  CColor (85, 255, 255, 255) },
                    { "TIP: CLICK GRILLE DOOR TO POP HATCH!",  CColor (104, 255, 156, 255) }
                }
            },
            // Page 3 (Blue / P400)
            {
                "P400", "TECH SPEC",
                CColor (28, 84, 184, 255), CColor (255, 255, 255, 255),
                {
                    { "RETRO TV HARDWARE TELEMETRY",           CColor (85, 255, 255, 255) },
                    { "----------------------------------------", CColor (51, 102, 204, 255) },
                    { "AUDIO:  RESAMPLED LOCKED STEREO RING",  CColor (255, 255, 255, 255) },
                    { "VIDEO:  HW/SW RGBX TEXTURE + CRT MASK", CColor (255, 255, 255, 255) },
                    { "FINISH: STUDIO SILVER SATIN CHASSIS",   CColor (255, 255, 255, 255) },
                    { "", CColor (255, 255, 255, 255) },
                    { "CREATED BY FLX.AGENCY  //  FELIX.SK",   CColor (255, 216, 77, 255) },
                    { "PRESS BLUE AGAIN TO RESUME VIDEO FEED", CColor (104, 255, 156, 255) }
                }
            }
        };

        const size_t pIdx = std::clamp<size_t> (static_cast<size_t> (page), 0, 3);
        const TeletextData& p = kPages[pIdx];

        // 1. Top Teletext header strip
        auto hdrFont = owned (new CFontDesc ("Menlo", 10.5, kBoldFace));
        ctx->setFont (hdrFont);
        char leftHdr[32];
        std::snprintf (leftHdr, sizeof (leftHdr), "%s  TELETEXT", p.num);
        ctx->setFontColor (CColor (255, 255, 255, 255));
        ctx->drawString (leftHdr, CRect (screen.left + pad, screen.top + 8.0, screen.left + pad + 160.0, screen.top + 24.0), kLeftText);

        ctx->setFontColor (CColor (255, 216, 77, 255));
        ctx->drawString ("RETRO TV", CRect (screen.right - pad - 120.0, screen.top + 8.0, screen.right - pad, screen.top + 24.0), kRightText);

        // 2. Double-height blocky Teletext title banner
        const CRect bannerRect (screen.left + pad, screen.top + 26.0, screen.right - pad, screen.top + 52.0);
        ctx->setFillColor (p.bannerBg);
        ctx->drawRect (bannerRect, kDrawFilled);

        auto titleFont = owned (new CFontDesc ("Menlo", 14.5, kBoldFace));
        ctx->setFont (titleFont);
        ctx->setFontColor (p.bannerFg);
        CRect bTextR = bannerRect;
        bTextR.top += 1.0;
        ctx->drawString (p.title, bTextR, kCenterText);

        // 3. Content rows
        auto bodyFont = owned (new CFontDesc ("Menlo", 10.0, kBoldFace));
        ctx->setFont (bodyFont);
        const CCoord rowStart = screen.top + 58.0;
        const CCoord rowStep = std::clamp ((screen.getHeight () - 88.0) / 8.0, 14.0, 22.0);
        for (int i = 0; i < 8; ++i)
        {
            if (p.lines[i].text[0] == '\0')
                continue;
            ctx->setFontColor (p.lines[i].color);
            CRect lineRect (screen.left + pad, rowStart + i * rowStep, screen.right - pad, rowStart + (i + 1) * rowStep);
            ctx->drawString (p.lines[i].text, lineRect, kLeftText);
        }

        // 4. Fastext 4-color bottom prompt bar
        const CCoord fy = screen.bottom - 22.0;
        const CCoord slotW = cw / 4.0;
        struct FastextPrompt {
            const char* label;
            CColor color;
        } prompts[4] = {
            { "ABOUT",  CColor (255, 75, 66, 255) },  // Red
            { "AUTHOR", CColor (72, 224, 116, 255) }, // Green
            { "SYNC",   CColor (255, 224, 71, 255) }, // Yellow
            { "SPECS",  CColor (90, 166, 255, 255) }  // Blue
        };

        auto promptFont = owned (new CFontDesc ("Menlo", 9.5, kBoldFace));
        ctx->setFont (promptFont);
        for (int i = 0; i < 4; ++i)
        {
            ctx->setFontColor (prompts[i].color);
            CRect prRect (screen.left + pad + i * slotW, fy, screen.left + pad + (i + 1) * slotW, fy + 16.0);
            ctx->drawString (prompts[i].label, prRect, kCenterText);
        }
    }

    void drawOsd (CDrawContext* ctx, const CRect& screen)
    {
        const CCoord padX = 12.0;
        const CCoord padTop = 20.0;
        const CCoord padBot = 12.0;

        auto font = owned (new CFontDesc ("Menlo", 11.5, kBoldFace));
        ctx->setFont (font);

        auto drawPill = [&] (const char* text, CCoord x, CCoord y, bool rightAlign, const CColor& col) {
            const CCoord tw = ctx->getStringWidth (text);
            const CCoord px = rightAlign ? x - tw - 5.0 : x - 5.0;
            const CRect pillRect (px, y - 11.5, px + tw + 10.0, y + 4.0);
            drawRoundedRect (ctx, pillRect, 2.5, CColor (0, 0, 0, 199), CColor (0, 0, 0, 0));

            ctx->setFontColor (col);
            CRect tr (px + 5.0, y - 11.0, px + tw + 5.0, y + 3.0);
            ctx->drawString (text, tr, rightAlign ? kRightText : kLeftText);
        };

        // 1. Top-Left: Channel tag + Play/Stop + SMPTE
        char tcBuf[64];
        const std::string tc = formatSmpte (videoPtsSec, fps);
        if (channelOne)
            std::snprintf (tcBuf, sizeof (tcBuf), "CH 1  %s %s", isPlaying ? "▶" : "■", tc.c_str ());
        else
            std::snprintf (tcBuf, sizeof (tcBuf), "%s %s", isPlaying ? "▶" : "■", tc.c_str ());
        const CColor tcColor = isPlaying ? CColor (120, 255, 155, 255) : CColor (255, 184, 77, 255);
        drawPill (tcBuf, screen.left + padX, screen.top + padTop, false, tcColor);

        // 2. Top-Right: OFS +0.00s
        char offBuf[32];
        std::snprintf (offBuf, sizeof (offBuf), "OFS %+.2fs", offsetSec);
        drawPill (offBuf, screen.right - padX, screen.top + padTop, true, CColor (255, 216, 117, 255));

        // 3. Bottom-Left: MUTE or VOL XX%
        if (muted)
        {
            drawPill ("MUTE", screen.left + padX, screen.bottom - padBot, false, CColor (255, 110, 94, 255));
        }
        else
        {
            char volBuf[32];
            std::snprintf (volBuf, sizeof (volBuf), "VOL %d%%", static_cast<int> (std::lround (volume * 100.0f)));
            drawPill (volBuf, screen.left + padX, screen.bottom - padBot, false, CColor (184, 232, 255, 255));
        }
    }

    void drawTestBars (CDrawContext* ctx, const CRect& screen, bool dim)
    {
        static const CColor bars[7] = {
            CColor (192, 192, 192, 255),
            CColor (192, 192, 0,   255),
            CColor (0,   192, 192, 255),
            CColor (0,   192, 0,   255),
            CColor (192, 0,   192, 255),
            CColor (192, 0,   0,   255),
            CColor (0,   0,   192, 255),
        };
        const CCoord w = screen.getWidth () / 7.0;
        const CCoord h1 = screen.getHeight () * 0.72;
        for (int i = 0; i < 7; ++i)
        {
            CColor c = bars[i];
            if (dim)
            {
                c.red /= 3;
                c.green /= 3;
                c.blue /= 3;
            }
            ctx->setFillColor (c);
            ctx->drawRect (CRect (screen.left + i * w, screen.top, screen.left + (i + 1) * w + 1.0, screen.top + h1), kDrawFilled);
        }

        // Bottom pluge strip
        ctx->setFillColor (CColor (18, 20, 24, 255));
        ctx->drawRect (CRect (screen.left, screen.top + h1, screen.right, screen.bottom), kDrawFilled);
    }

    RetroTVEditor* editor {nullptr};
    SharedPointer<CBitmap> bitmap;
    bool hasVideo {false};
    bool outOfRange {false};
    bool crtFx {true};
    bool osdEnabled {true};
    bool channelOne {false};
    int teletextPage {-1};
    bool dragHover {false};
    bool isPlaying {false};
    bool muted {false};
    float volume {0.8f};
    double offsetSec {0.0};
    double videoPtsSec {0.0};
    double fps {24.0};
    std::string errorText;
};

//-----------------------------------------------------------------------------
// RetroTVEditor Implementation
//-----------------------------------------------------------------------------
RetroTVEditor::RetroTVEditor (RetroTVPlugin* p)
: VSTGUIEditor (p, nullptr)
, plugin (p)
{
    Steinberg::ViewRect vr (0, 0, static_cast<Steinberg::int32> (kDefaultWidth), static_cast<Steinberg::int32> (kDefaultHeight));
    setRect (vr);
}

//-----------------------------------------------------------------------------
RetroTVEditor::~RetroTVEditor () = default;

//-----------------------------------------------------------------------------
bool PLUGIN_API RetroTVEditor::open (void* parent, const PlatformType& platformType)
{
    const CRect frameRect (0, 0, rect.getWidth () > 0 ? rect.getWidth () : kDefaultWidth,
                           rect.getHeight () > 0 ? rect.getHeight () : kDefaultHeight);
    frame = new CFrame (frameRect, this);
    if (!frame->open (parent, platformType))
    {
        frame->forget ();
        frame = nullptr;
        return false;
    }

    chassisView = new RetroTVChassisView (frameRect, this);
    frame->addView (chassisView);

    crtView = new RetroCRTView (this);
    frame->addView (crtView);

    if (plugin)
        plugin->setPreferredPixelOrder (crtView->queryPreferredPixelOrder ());

    // Two big notched VHF rotary dials on wood at top of control bay
    offsetDial = new RetroVhfDial (CRect (0, 0, 60, 70), this, kParamOffsetSeconds, "OFS", 0.5f);
    frame->addView (offsetDial);

    volumeDial = new RetroVhfDial (CRect (0, 0, 60, 70), this, kParamVolume, "VOL", 0.8f);
    frame->addView (volumeDial);

    // Small rectangular LOAD button in lower module left column
    loadButton = new RetroLoadButton (CRect (0, 0, 34, 15), this, kTagLoadButton);
    frame->addView (loadButton);

    // Backlit round pushbuttons for MUTE and INFO in lower module right column
    muteButton = new RetroRoundButton (CRect (0, 0, 30, 30), this, kParamMute, "MUTE", CColor (255, 150, 66, 255)); // #ff9642
    frame->addView (muteButton);

    infoButton = new RetroRoundButton (CRect (0, 0, 30, 30), this, kTagInfoButton, "INFO", CColor (104, 255, 156, 255)); // #68ff9c
    infoButton->setValueNormalized (osdEnabled ? 1.0f : 0.0f);
    frame->addView (infoButton);

    layoutViews (frameRect.getWidth (), frameRect.getHeight ());
    syncControlsFromPlugin ();

    lastFrameSeq = 0;
    pollEngineAndRefresh ();

    refreshTimer = makeOwned<CVSTGUITimer> (this, 16); // ~60 Hz UI refresh
    refreshTimer->start ();

    return true;
}

//-----------------------------------------------------------------------------
void PLUGIN_API RetroTVEditor::close ()
{
    if (refreshTimer)
    {
        refreshTimer->stop ();
        refreshTimer = nullptr;
    }

    chassisView = nullptr;
    crtView = nullptr;
    offsetDial = nullptr;
    volumeDial = nullptr;
    loadButton = nullptr;
    muteButton = nullptr;
    infoButton = nullptr;

    if (frame)
    {
        frame->close ();
        frame = nullptr;
    }
}

//-----------------------------------------------------------------------------
Steinberg::tresult PLUGIN_API RetroTVEditor::checkSizeConstraint (Steinberg::ViewRect* vr)
{
    if (!vr)
        return Steinberg::kInvalidArgument;

    const Steinberg::int32 w = std::clamp<Steinberg::int32> (vr->getWidth (), static_cast<Steinberg::int32> (kMinWidth), static_cast<Steinberg::int32> (kMaxWidth));
    const Steinberg::int32 h = std::clamp<Steinberg::int32> (vr->getHeight (), static_cast<Steinberg::int32> (kMinHeight), static_cast<Steinberg::int32> (kMaxHeight));
    vr->right = vr->left + w;
    vr->bottom = vr->top + h;
    return Steinberg::kResultTrue;
}

//-----------------------------------------------------------------------------
Steinberg::tresult PLUGIN_API RetroTVEditor::onSize (Steinberg::ViewRect* newSize)
{
    Steinberg::tresult res = VSTGUIEditor::onSize (newSize);
    if (newSize && frame)
        layoutViews (newSize->getWidth (), newSize->getHeight ());
    return res;
}

//-----------------------------------------------------------------------------
void RetroTVEditor::layoutViews (CCoord width, CCoord height)
{
    if (!frame)
        return;

    const CCoord W = width;
    const CCoord H = height;
    const CCoord bodyH = H - 16.0;

    const CRect frameRect (0, 0, W, H);
    if (chassisView)
        chassisView->setViewSize (frameRect);

    // Frame geometry matching mockup
    const CRect cab (4.0, 4.0, W - 4.0, bodyH - 4.0);
    const CRect groove (cab.left + 4.0, cab.top + 4.0, cab.right - 4.0, cab.bottom - 4.0);
    const CRect silverInner (groove.left + 4.0, groove.top + 4.0, groove.right - 4.0, groove.bottom - 4.0);
    const CRect woodFace (silverInner.left + 2.5, silverInner.top + 2.5, silverInner.right - 2.5, silverInner.bottom - 2.5);

    const CCoord woodPadX = std::max (16.0, std::round (W * 0.025));
    const CCoord woodPadY = 15.0;
    const CCoord woodDivider = std::max (12.0, std::round (W * 0.018));

    // Bay width proportional to wood face (fits stacked 1.5x dials + compact lower module)
    const CCoord bayW = std::clamp (woodFace.getWidth () * 0.21, 122.0, 144.0);

    const CRect bay (woodFace.right - woodPadX - bayW,
                     woodFace.top + woodPadY,
                     woodFace.right - woodPadX,
                     woodFace.bottom - woodPadY);

    const CRect bezel (woodFace.left + woodPadX,
                       woodFace.top + woodPadY,
                       bay.left - woodDivider,
                       woodFace.bottom - woodPadY);

    if (chassisView)
        chassisView->setGeometries (cab, groove, silverInner, woodFace, bay);

    if (crtView)
        crtView->setViewSize (bezel);

    // Lower module anchored compactly at bottom so OFS and VOL stack vertically ~1.5x larger above it
    const CCoord lowerY = computeLowerModuleY (bay);
    const CCoord dialAreaH = std::max (80.0, lowerY - bay.top - 4.0);
    const CCoord dialSlotH = dialAreaH * 0.5;
    const CCoord maxRFromH = std::floor ((dialSlotH - 24.0) * 0.5);
    const CCoord maxRFromW = std::floor ((bay.getWidth () - 16.0) * 0.44);
    const CCoord knobR = std::clamp (std::min (maxRFromH, maxRFromW), 16.0, 42.0);
    const CCoord dialW = (knobR + 8.0) * 2.0;
    const CCoord dialH = knobR * 2.0 + 24.0;
    const CCoord cxKnob = bay.getCenter ().x;
    const CCoord slot1Top = bay.top + std::max (0.0, (dialSlotH - dialH) * 0.5);
    const CCoord slot2Top = bay.top + dialSlotH + std::max (0.0, (dialSlotH - dialH) * 0.5);

    if (offsetDial)
        offsetDial->setViewSize (CRect (cxKnob - dialW * 0.5, slot1Top, cxKnob + dialW * 0.5, slot1Top + dialH));

    if (volumeDial)
        volumeDial->setViewSize (CRect (cxKnob - dialW * 0.5, slot2Top, cxKnob + dialW * 0.5, slot2Top + dialH));

    const CCoord midY = lowerY;
    const CRect midStrip (bay.left, midY, bay.right, midY + 52.0);

    // Left column: small rectangular LOAD button (y = midY + 30, h = 15, w = 34)
    if (loadButton)
        loadButton->setViewSize (CRect (midStrip.left + 7.0, midY + 30.0, midStrip.left + 7.0 + 34.0, midY + 30.0 + 15.0));

    // Right side of module: MUTE and INFO round pushbuttons
    const CCoord btnR = 7.5;
    const CCoord btnD = (btnR + 6.0) * 2.0;
    const CCoord cxMute = midStrip.left + midStrip.getWidth () * 0.56;
    const CCoord cxInfo = midStrip.left + midStrip.getWidth () * 0.83;
    const CCoord cyBtns = midY + 22.0;

    if (muteButton)
        muteButton->setViewSize (CRect (cxMute - btnD * 0.5, cyBtns - btnR - 2.0, cxMute + btnD * 0.5, cyBtns + btnR + 14.0));

    if (infoButton)
        infoButton->setViewSize (CRect (cxInfo - btnD * 0.5, cyBtns - btnR - 2.0, cxInfo + btnD * 0.5, cyBtns + btnR + 14.0));

    frame->invalid ();
}

//-----------------------------------------------------------------------------
bool RetroTVEditor::isCrtFxEnabled () const
{
    return plugin ? plugin->isCrtFxEnabled () : true;
}

//-----------------------------------------------------------------------------
void RetroTVEditor::syncControlsFromPlugin ()
{
    if (!plugin)
        return;

    const double offSec = plugin->getOffsetSeconds ();
    if (offsetDial && !offsetDial->isEditing ())
    {
        offsetDial->setValueNormalized (static_cast<float> (RetroTVPlugin::offsetSecondsToNormalized (offSec)));
    }

    const float vol = plugin->getVolume ();
    if (volumeDial && !volumeDial->isEditing ())
    {
        volumeDial->setValueNormalized (vol);
    }

    const bool muted = plugin->isMuted ();
    if (muteButton && !muteButton->isEditing ())
    {
        const float targetMute = muted ? 1.0f : 0.0f;
        if (std::fabs (muteButton->getValueNormalized () - targetMute) > 0.01f)
        {
            muteButton->setValueNormalized (targetMute);
            muteButton->invalid ();
        }
    }

    if (chassisView)
        chassisView->setMuted (muted);

    if (infoButton && !infoButton->isEditing ())
    {
        const float targetInfo = osdEnabled ? 1.0f : 0.0f;
        if (std::fabs (infoButton->getValueNormalized () - targetInfo) > 0.01f)
        {
            infoButton->setValueNormalized (targetInfo);
            infoButton->invalid ();
        }
    }

    const bool crtOn = plugin->isCrtFxEnabled ();
    if (crtView)
    {
        crtView->setCrtFx (crtOn);
        crtView->setOsdEnabled (osdEnabled);
        crtView->setTeletextPage (teletextPage);
        crtView->setMuted (muted);
        crtView->setVolume (vol);
        crtView->setOffsetSeconds (offSec);
    }
}

//-----------------------------------------------------------------------------
void RetroTVEditor::pollEngineAndRefresh ()
{
    if (!plugin)
        return;

    const int ch = plugin->getActiveChannel ();
    VideoEngine* engine = (ch == 1 && plugin->getEasterEngine ()) ? plugin->getEasterEngine () : &plugin->getVideoEngine ();
    if (ch == 1)
        plugin->advanceEasterClockFromUI (0.016);

    if (engine->copyLatestFrameIfChanged (lastFrameSeq, cachedFrame))
    {
        if (crtView)
            crtView->updateFromSnapshot (cachedFrame);
    }

    bool hasVideo = false;
    double durationSec = 0.0;
    double fps = 24.0;
    double videoPtsSec = 0.0;
    bool outOfRange = false;
    std::string filePath;
    std::string error;
    engine->getStatus (hasVideo, durationSec, fps, videoPtsSec, outOfRange, filePath, error);

    if (ch == 1)
    {
        if (durationSec > 0.25)
            plugin->setEasterDurationSeconds (durationSec);

        if (crtView)
        {
            crtView->setChannelOne (true);
            crtView->updateTransport (true, plugin->getEasterTimeSeconds (), fps, hasVideo, false, error);
        }
    }
    else
    {
        const TransportSnapshot tr = engine->getTransportSnapshot ();
        const double offSec = plugin->getOffsetSeconds ();
        const double targetSec = tr.dawTimeSec + offSec;

        if (crtView)
        {
            crtView->setChannelOne (false);
            crtView->updateTransport (tr.isPlaying, targetSec, fps, hasVideo, outOfRange, error);
        }
    }

    syncControlsFromPlugin ();
}

//-----------------------------------------------------------------------------
CMessageResult RetroTVEditor::notify (CBaseObject* sender, const char* message)
{
    if (sender == refreshTimer && message == CVSTGUITimer::kMsgTimer)
    {
        pollEngineAndRefresh ();
        return kMessageNotified;
    }

    return VSTGUIEditor::notify (sender, message);
}

//-----------------------------------------------------------------------------
void RetroTVEditor::controlBeginEdit (CControl* pControl)
{
    if (!pControl || !plugin)
        return;
    const int32_t tag = pControl->getTag ();
    if (tag == kParamOffsetSeconds || tag == kParamVolume || tag == kParamMute || tag == kParamCrtFx)
        plugin->beginEdit (static_cast<Steinberg::Vst::ParamID> (tag));
}

//-----------------------------------------------------------------------------
void RetroTVEditor::controlEndEdit (CControl* pControl)
{
    if (!pControl || !plugin)
        return;
    const int32_t tag = pControl->getTag ();
    if (tag == kParamOffsetSeconds || tag == kParamVolume || tag == kParamMute || tag == kParamCrtFx)
        plugin->endEdit (static_cast<Steinberg::Vst::ParamID> (tag));
}

//-----------------------------------------------------------------------------
void RetroTVEditor::valueChanged (CControl* pControl)
{
    if (!pControl || !plugin)
        return;

    const int32_t tag = pControl->getTag ();
    const float val = pControl->getValueNormalized ();

    switch (tag)
    {
        case kParamOffsetSeconds:
        case kParamVolume:
        case kParamMute:
        case kParamCrtFx:
            plugin->performEdit (static_cast<Steinberg::Vst::ParamID> (tag), val);
            plugin->setParamNormalized (static_cast<Steinberg::Vst::ParamID> (tag), val);
            syncControlsFromPlugin ();
            break;

        case kTagLoadButton:
            if (val > 0.5f)
                requestOpenVideoDialog ();
            break;

        case kTagInfoButton:
            osdEnabled = (val > 0.5f);
            if (crtView)
            {
                crtView->setOsdEnabled (osdEnabled);
                crtView->invalid ();
            }
            break;

        default:
            break;
    }
}

//-----------------------------------------------------------------------------
void RetroTVEditor::toggleCrtFx ()
{
    if (!plugin)
        return;
    const bool current = plugin->isCrtFxEnabled ();
    const float nextVal = current ? 0.0f : 1.0f;
    plugin->beginEdit (kParamCrtFx);
    plugin->performEdit (kParamCrtFx, nextVal);
    plugin->setParamNormalized (kParamCrtFx, nextVal);
    plugin->endEdit (kParamCrtFx);
    syncControlsFromPlugin ();
}

//-----------------------------------------------------------------------------
void RetroTVEditor::nudgeOffset (int frames)
{
    if (!plugin)
        return;
    const double fps = (cachedFrame.fps > 1.0) ? cachedFrame.fps : 30.0;
    const double delta = static_cast<double> (frames) / fps;
    const double curSec = plugin->getOffsetSeconds ();
    const double nextSec = std::clamp (curSec + delta, kMinOffsetSeconds, kMaxOffsetSeconds);
    plugin->setParameterFromUI (kParamOffsetSeconds, RetroTVPlugin::offsetSecondsToNormalized (nextSec));
    syncControlsFromPlugin ();
}

//-----------------------------------------------------------------------------
void RetroTVEditor::resetOffset ()
{
    if (!plugin)
        return;
    plugin->setParameterFromUI (kParamOffsetSeconds, RetroTVPlugin::offsetSecondsToNormalized (0.0));
    syncControlsFromPlugin ();
}

//-----------------------------------------------------------------------------
void RetroTVEditor::setActiveChannel (int ch)
{
    if (!plugin)
        return;

    plugin->setActiveChannel (ch);
    lastFrameSeq = 0;

    if (teletextPage != -1)
    {
        teletextPage = -1;
        if (crtView)
            crtView->setTeletextPage (-1);
        if (chassisView)
            chassisView->setTeletextPage (-1);
    }

    if (chassisView)
        chassisView->invalid ();
    pollEngineAndRefresh ();
}

//-----------------------------------------------------------------------------
int RetroTVEditor::getActiveChannel () const
{
    return plugin ? plugin->getActiveChannel () : 2;
}

//-----------------------------------------------------------------------------
void RetroTVEditor::setTeletextPage (int page)
{
    if (teletextPage == page)
        teletextPage = -1; // Toggle off if already active
    else
        teletextPage = page;

    if (crtView)
    {
        crtView->setTeletextPage (teletextPage);
        crtView->invalid ();
    }
    if (chassisView)
    {
        chassisView->setTeletextPage (teletextPage);
        chassisView->invalid ();
    }
}

//-----------------------------------------------------------------------------
void RetroTVEditor::requestOpenVideoDialog ()
{
    if (!frame)
        return;

    if (CNewFileSelector* selector = CNewFileSelector::create (frame, CNewFileSelector::kSelectFile))
    {
        selector->addFileExtension (CFileExtension ("MP4 Video", "mp4"));
        selector->addFileExtension (CFileExtension ("QuickTime Movie", "mov"));
        selector->addFileExtension (CFileExtension ("M4V Video", "m4v"));
#if defined(_WIN32)
        selector->addFileExtension (CFileExtension ("AVI Video", "avi"));
        selector->addFileExtension (CFileExtension ("Windows Media Video", "wmv"));
#endif
        selector->setTitle ("Select Video File for Retro TV");
        selector->run ([self = VSTGUI::shared (this)] (CNewFileSelector* sel) {
            if (!self->frame || !self->plugin || !sel || sel->getNumSelectedFiles () == 0)
                return;

            if (UTF8StringPtr url = sel->getSelectedFile (0))
                self->plugin->setVideoFileFromUI (url);
        });
        selector->forget ();
    }
}

//-----------------------------------------------------------------------------
void RetroTVEditor::handleDroppedFile (const std::string& path)
{
    if (plugin && !path.empty ())
        plugin->setVideoFileFromUI (path);
}

} // namespace ReasonVideo
