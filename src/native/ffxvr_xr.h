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
    bool (*copy)(ID3D11Texture2D* src, ID3D11RenderTargetView* dst, UINT w, UINT h);   // stretch copy, alpha 1
};

struct XrPoseOut { float q[4]; float p[3]; };               // orientation (x,y,z,w), position (m), in the reference space

struct XrFrameOut {
    bool      have_views;                                   // false: render as usual, nothing is submitted this frame
    XrPoseOut view[2];                                      // left / right eye as located for the frame about to render
};

bool        xr_start(const XrHost& host);                   // loads the loader, creates instance + session + swapchains
void        xr_stop();
bool        xr_active();                                    // session exists (may not be running yet)
const char* xr_status();                                    // one line for the overlay

/// Per frame, from the Present hook. `eye1`/`eye2` are the finished images
/// of the frame just rendered (eye2 may be null: mono); `tan_x`/`tan_y` the
/// tangents of the half field of view they were rendered with. Submits them
/// with the poses returned by the previous call, then waits for the next
/// frame and returns its poses in `next`.
void        xr_frame(ID3D11Texture2D* eye1, ID3D11Texture2D* eye2, float tan_x, float tan_y, XrFrameOut* next);
