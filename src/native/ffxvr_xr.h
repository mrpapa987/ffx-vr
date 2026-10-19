// ffxvr_xr.h -- OpenXR session for the FFX VR mod: submits the two eye
// images the duplication pipeline produces and reports the head pose the
// next frame should be rendered from. Everything it needs from the rest of
// the native helper comes through XrHost, so this file stays standalone.
#pragma once
#include <d3d11.h>

struct XrHost {
    ID3D11Device*        device;
    ID3D11DeviceContext* ctx;
    const wchar_t*       dir;                               // where openxr_loader.dll lives (next to our DLL)
    void (*diag)(const char* fmt, ...);                     // append a line to ffxvr_diag.log
    bool (*copy)(ID3D11Texture2D* src, ID3D11RenderTargetView* dst, UINT w, UINT h, bool keep_alpha);   // stretch copy; alpha forced to 1 unless kept
};

struct XrPoseOut { float q[4]; float p[3]; };               // orientation (x,y,z,w), position (m), in the reference space

struct XrFovTan { float l, r, u, d; };                     // tangents of the half angles (l and d negative)

struct XrFrameOut {
    bool      have_views;                                   // false: render as usual, nothing is submitted this frame
    XrPoseOut view[2];                                      // left / right eye as located for the frame about to render
    XrFovTan  fov[2];                                       // the headset's field of view per eye
};

/// The virtual screen: a flat panel in the reference space showing eye 1's
/// image, used for frames that are not 3D (title, menus, loading, FMV).
struct XrScreen {
    bool  on;                                               // submit the screen instead of the projection layers
    float q[4]; float p[3];                                 // pose of the panel centre in the reference space
    float width;                                            // metres; height follows the 16:9 image
};

/// The UI panel: the game's UI pass on a head-locked, alpha-blended quad.
struct XrUiPanel {
    ID3D11Texture2D* tex;                                   // null: no panel this frame
    float dist, width;                                      // metres; height follows 16:9
    ID3D11Texture2D* tex2;                                  // a second head-locked quad (the ImGui layer: Fahrenheit's menus, our overlay); null: none
};

/// Controller input through the OpenXR action system (Touch controllers;
/// khr/simple as the fallback). Values are the last xrSyncActions result.
struct XrHand {
    bool  active;                                           // the runtime reports a controller bound for this hand
    float stick[2];                                         // thumbstick x (right +), y (up +), -1..1
    bool  stick_click;
    bool  primary, secondary;                               // A / B on the right, X / Y on the left
    float trigger, squeeze;                                 // 0..1
    bool  menu;                                             // left controller's menu button (the right one's is the system button)
    bool  grip_valid; float grip_q[4], grip_p[3];           // grip pose in the reference space (orientation x,y,z,w; metres)
    bool  aim_valid;  float aim_q[4],  aim_p[3];            // aim pose (the pointing ray)
};
struct XrInput { bool ready; XrHand hand[2]; char profile[2][64]; };   // 0 left, 1 right; profile: the bound interaction profile per hand
void        xr_input_get(XrInput* out);                     // copies the state synced at the last xr_frame
void        xr_haptic(int hand, float amplitude, float seconds);   // 0 left, 1 right

bool        xr_start(const XrHost& host);                   // loads the loader, creates instance + session + swapchains
void        xr_stop();
bool        xr_active();                                    // session exists (may not be running yet)
const char* xr_status();
void xr_timing(double* wait_ms, double* end_ms);           // last frame: xrWaitFrame / xrEndFrame durations                                    // one line for the overlay

/// Per frame, from the Present hook. `eye1`/`eye2` are the finished images
/// of the frame just rendered (eye2 may be null: mono); `used` the field of
/// view each was rendered with. Submits them with the poses returned by the
/// previous call, then waits for the next frame and returns its poses and
/// the headset's field of view in `next`.
void        xr_frame(ID3D11Texture2D* eye1, ID3D11Texture2D* eye2, const XrFovTan used[2], const XrScreen* screen, const XrUiPanel* ui, XrFrameOut* next);
