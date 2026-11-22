// SPDX-License-Identifier: MIT
// ffxvr_xr.h -- OpenXR session for the FFX VR mod: submits the two eye
// images the duplication pipeline produces and reports the head pose the
// next frame should be rendered from. Everything it needs from the rest of
// the native helper comes through XrHost, so this file stays standalone.
#pragma once
#include <cstdint>
#include <d3d11.h>

struct XrHost {
    ID3D11Device*        device;
    ID3D11DeviceContext* ctx;
    const wchar_t*       dir;                               // where openxr_loader.dll lives (next to our DLL)
    void (*diag)(const char* fmt, ...);                     // append a line to ffxvr_diag.log
    bool (*copy)(ID3D11Texture2D* src, ID3D11RenderTargetView* dst, UINT w, UINT h, int mode);   // stretch copy. mode 0: alpha forced to 1; 1: alpha kept (the UI panel); 2: the ImGui layer, premultiplied (saveui)
    int  (*wait_begin)();                                   // load lock build: right before xrWaitFrame (may give up the game's render lock); null = none
    void (*wait_end)(int token);                            // right after xrWaitFrame returns, on every path, with wait_begin's result (takes the lock back)
    int  (*extra_wait_begin)();                             // stickrep: the same pair for an extra frame's xrWaitFrame (xr_extra_frame): gives up the game's
    void (*extra_wait_end)(int token);                      //   render lock the extra took for its xrEndFrame and takes it back for xrBeginFrame / sync / locate
    bool (*extra_abort)();                                  // SR-1: true = the game is exiting: an extra frame stops before its next OpenXR call (null = never)
    // vr-menu S2 (null = no menu panel): the panel's crop copy, 1:1 from (x0, y0) of src into dst (w x h), premultiplied, linear-light
    // for an sRGB swapchain
    bool (*copy_crop)(ID3D11Texture2D* src, ID3D11RenderTargetView* dst, UINT w, UINT h, int x0, int y0);
    // perf-gpu (null = none): right before a fresh frame's xrEndFrame in xr_frame, after every copy into the swapchains (never for
    // an extra frame): the GPU phase timers' last marker and the busy-GPU gate's event query
    void (*before_end)();
};

struct XrPoseOut { float q[4]; float p[3]; };               // orientation (x,y,z,w), position (m), in the reference space

struct XrFovTan { float l, r, u, d; };                     // tangents of the half angles (l and d negative)

struct XrFrameOut {
    bool      have_views;                                   // false: render as usual, nothing is submitted this frame
    XrPoseOut view[2];                                      // left / right eye as located for the frame about to render
    XrFovTan  fov[2];                                       // the headset's field of view per eye
    // stickrep (display-rate stick reprojection): the pacing facts of this call's xrEndFrame / xrWaitFrame / xrBeginFrame
    bool      began;                                        // a frame is open after this call (xrWaitFrame and xrBeginFrame succeeded)
    bool      should_render;                                // the begun frame's shouldRender
    int64_t   display_time;                                 // its predictedDisplayTime (XrTime, ns); 0 when nothing was begun
    int64_t   period_ns;                                    // its predictedDisplayPeriod
    double    wait_ms;                                      // the time spent in xrWaitFrame
    bool      wait_blocked;                                 // wait_ms >= 0.5: VDXR held the wait to its running start (the phase anchor)
    double    t_wait_return_ms;                             // QPC ms when xrWaitFrame returned
    bool      ended;                                        // this call ended a frame (xrEndFrame was called)
    bool      base_fresh;                                   // ... with a fresh base layer (new eye images): the image held from now on
    bool      base_sent;                                    // ... with a base layer at all (fresh or held); false: zero layers went out
    uint32_t  skipped_slots;                                // display slots between the previous begun frame and this one: round(dT / period) - 1
    double    end_ms, locate_ms;                            // time in xrEndFrame; in xrBeginFrame + xrSyncActions + xrLocateViews
};

/// stickrep: the rigid rotation an extra frame applies to the held projection layer's two poses, in the LOCAL space:
/// orientation' = q * orientation, position' = pivot + q * (position - pivot). Computed by the native side (3.3 of the spec).
struct XrExtraRot { bool on; float q[4]; float pivot[3]; };

/// The virtual screen: a flat panel in the reference space showing eye 1's
/// image, used for frames that are not 3D (title, menus, loading, FMV).
struct XrScreen {
    bool  on;                                               // submit the screen instead of the projection layers
    float q[4]; float p[3];                                 // pose of the panel centre in the reference space
    float width;                                            // metres; height follows the 16:9 image
    bool  stereo;                                           // a stereo screen: eye 1's image to the left eye, `eye2` to the right
    ID3D11Texture2D* eye2;                                  // the right eye's image when stereo
};

/// The UI panel: the game's UI pass on a head-locked, alpha-blended quad.
struct XrUiPanel {
    ID3D11Texture2D* tex;                                   // null: no panel this frame
    float dist, width;                                      // metres; height follows 16:9 (uidepth: the adaptive distance, width scaled with it)
    ID3D11Texture2D* tex2;                                  // a second head-locked quad (the ImGui layer: Fahrenheit's menus, our overlay); null: none
    float im_dist, im_width;                                // the ImGui layer's own distance / width (the ui_distance / ui_width settings; never adaptive)
    bool  im_world;                                         // saveui: the ImGui layer lies on the virtual screen (world-locked, the reference space) instead of head-locked
    float im_q[4], im_p[3], im_w;                           // ... with this pose (the screen's centre) and width (the screen's; height 16:9). Only with a screen base layer
    // vr-menu S2: the F8 window's own panel, a world-locked quad (the reference space). ptex: the panel's own capture of the F8
    // window's draws (null: no panel this frame; they are not in tex2); its rectangle (pcrop_x, pcrop_y, pcrop_w, pcrop_h, at most
    // 1536 x 1536) goes 1:1 into the panel's swapchain; pose and size in metres.
    ID3D11Texture2D* ptex;
    int   pcrop_x, pcrop_y, pcrop_w, pcrop_h;
    float pq[4], pp[3], pw, ph;
    // vr-menu S3: the pointer's beams (0..2), world-locked quads (the reference space) from btex (16 x 128, premultiplied: columns
    // 0-7 the owner's, 8-15 faint), written into their swapchain once. bq / bp: pose (its +Y along the beam), blen: length (m).
    ID3D11Texture2D* btex;
    int   nb;
    float bq[2][4], bp[2][3], blen[2];
    int   bfaint[2];
    // od-bar-art round 5: the overdrive bar in front of the eyes (Tidus's Swordplay bar, Lulu's countdown), a world-locked quad (the
    // reference space) right after the UI panel in the layer order, so the panel never draws over it. otex: its premultiplied image
    // (null: no bar layer this frame), copied whole into its own swapchain (xr_bar_ensure); oq / op: pose (+Z toward the eyes),
    // ow x oh: size (m). Only over a projection base (never the screens).
    ID3D11Texture2D* otex;
    float oq[4], op[3], ow, oh;
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
    // VR hands v2 (S2): the capacitive touch sensors of the Touch controllers (oculus/touch_controller only; false elsewhere)
    bool  trigger_touch;                                    // the index finger on the trigger
    bool  thumb_stick, thumb_rest, thumb_face;              // the thumb on the stick / on the thumbrest / on A-B (right) or X-Y (left)
    bool  grip_tracked;                                     // the grip's position is tracked (POSITION_TRACKED), not inferred (overdrive gestures)
};
struct XrInput { bool ready; XrHand hand[2]; char profile[2][64]; int64_t time; bool touch; };   // 0 left, 1 right; profile: the bound interaction profile per hand;
                                                            // time: the XrTime the poses were located at (predictedDisplayTime); touch: the touch actions are bound
void        xr_input_get(XrInput* out);                     // copies the state synced at the last xr_frame
void        xr_haptic(int hand, float amplitude, float seconds);   // 0 left, 1 right
void        xr_haptic_stop(int hand);                              // rumble to haptics: stop a running pulse (xrStopHapticFeedback)
bool        xr_focused();                                          // the session is FOCUSED (haptics only then)

bool        xr_start(const XrHost& host);                   // loads the loader, creates instance + session + swapchains
void        xr_stop();
bool        xr_active();                                    // session exists (may not be running yet)
const char* xr_status();
void        xr_debug(int sim_timeouts);                     // test hook: the next N swapchain image waits report a timeout
void        xr_counters(unsigned* held_frames, unsigned* wait_timeouts);   // frames held (no eye image, a wait timeout or error, no views), swapchain wait timeouts (for the heartbeat)
bool        xr_take_stuck();   // R8-3: true once after a B5-2 stop (a stuck swapchain): xr_tick counts it as a failed start, so the restart backs off
void        xr_eye_size(unsigned* w, unsigned* h);          // the eye swapchains' size: the headset's recommended width at 16:9 (0 x 0 while no session)
void        xr_imgui_size(unsigned* w, unsigned* h);        // the ImGui layer's swapchain size (saveui: 1920x1080; 0 x 0 while no session)
bool        xr_panel_ready();                               // vr-menu S2: the menu panel's swapchain exists (a running session with its 1536 x 1536 swapchain)
bool        xr_panel_possible();                            // ... or can be made: a running session where making it has not failed
bool        xr_panel_ensure();                              // render thread: makes it at the first use (review N6: nothing while the panel is unused); true = it exists
                                                            // (vr-menu S3: the beam's 16 x 128 swapchain with it; a failure there only drops the beams)
bool        xr_bar_possible();                              // od-bar-art round 5: the overdrive bar's swapchain exists or can be made (a running session where making it has not failed)
bool        xr_bar_ensure();                                // render thread: makes it (k_bar_w x k_bar_h) at the first use; true = it exists
void        xr_bar_size(unsigned* w, unsigned* h);          // its size (0 x 0 before it is made)
uint32_t    xr_max_layers();                                // the runtime's maxLayerCount (0 before the first session)
bool        xr_srgb();                                      // the swapchains are R8G8B8A8_UNORM_SRGB: the compositor decodes our UNORM bytes before it blends (uidepth's plate)
void xr_timing(double* wait_ms, double* end_ms);
const char* xr_stage();                                    // the render thread's current stage name (a string literal; the load profiler reads it)
bool        xr_take_space_change();                        // true once after the runtime recentred its reference space (the headset's own recentre gesture)           // last frame: xrWaitFrame / xrEndFrame durations                                    // one line for the overlay

/// Battle transition (research battle-transition.md 4.2, option B). Called before xr_frame: xr_pin(true) takes the
/// poses the frame about to be submitted was rendered with (the views located at the previous xr_frame) and the field of
/// view `used`; while pinned, every projection layer goes out with those poses and that field of view, so frames rendered
/// from a frozen head pose stay world-locked where they were. False (nothing pinned) without a running session or located
/// views. A runtime space change or a stop drops the pin (xr_pinned then says false).
bool        xr_pin(bool on, const XrFovTan used[2]);
bool        xr_pinned();
/// FMV relief (S3): the poses and field of view the next xr_frame submits a projection layer with (the views located at the
/// last wait, an extra frame's included), so a frame drawn with them is world-locked. False without an open frame to render.
bool        xr_render_views(XrPoseOut v[2], XrFovTan f[2]);
void        xr_hold_uncapped(bool on);                     // the held base layer may be re-sent past the 30-frame cap (the transition window only)

/// Per frame, from the Present hook. `eye1`/`eye2` are the finished images
/// of the frame just rendered (eye2 may be null: mono); `used` the field of
/// view each was rendered with. Submits them with the poses returned by the
/// previous call, then waits for the next frame and returns its poses and
/// the headset's field of view in `next`.
void        xr_frame(ID3D11Texture2D* eye1, ID3D11Texture2D* eye2, const XrFovTan used[2], const XrScreen* screen, const XrUiPanel* ui, XrFrameOut* next);

/// stickrep: an extra frame between two game frames (render thread, from the ring wait, never inside a command list).
/// Ends the open frame with the layers the last xr_frame call sent, re-referencing the swapchain images that call
/// released (no acquire, no copy): the base layer (projection or screen) rotated by `rot` when it is on (never a screen
/// or a pinned base); the UI panel and the ImGui quad as they were sent, only with `quads`. `zero`: zero layers. Then
/// xrWaitFrame (between the host's extra_wait_begin / extra_wait_end) -> xrBeginFrame -> xrSyncActions -> xrLocateViews,
/// as xr_frame's tail. Returns false when nothing was called (no running session, no open frame, shouldRender false,
/// nothing to send); `next` then says began = false.
bool        xr_extra_frame(const XrExtraRot* rot, bool quads, bool zero, XrFrameOut* next);
bool        xr_extra_possible(bool zero);                  // an extra would be sent now (the same tests as xr_extra_frame; zero: a zero-layer one)
bool        xr_frame_open();                               // a frame is begun and not yet ended
bool        xr_running();                                  // the session is running (between xrBeginSession and the stop)
/// The display: the last predictedDisplayPeriod (ms), the runtime's refresh rate (Hz; XR_FB_display_refresh_rate, else
/// 1000 / the shortest period seen; 0 unknown) and whether the period is about twice the refresh period (VD's SSW).
void        xr_display(double* period_ms, double* refresh_hz, bool* half_rate);
bool        xr_now_xrtime(int64_t* t);                     // the current XrTime (XR_KHR_win32_convert_performance_counter_time); false without it
