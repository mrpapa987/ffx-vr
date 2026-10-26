// SPDX-License-Identifier: MIT
// ffxvr_xr.cpp -- see ffxvr_xr.h. The loader is loaded by full path and
// every entry point fetched through xrGetInstanceProcAddr, so no import
// library and no DLL search-path games.
#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <windows.h>
#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "ffxvr_xr.h"

namespace {

XrHost g_h = {};
HMODULE g_loader = nullptr;
char g_status[160] = "off";

// Stall watchdog: the render thread records the stage it is in; a thread
// checks every second and logs if a stage has lasted more than 5 s.
volatile const char* g_stage = "idle";
volatile DWORD g_stage_tick = 0;
HANDLE g_watchdog = nullptr;
HANDLE g_watchdog_wake = nullptr;                   // auto-reset: xr_stop wakes the watchdog at once instead of waiting out its 1 s sleep
volatile bool g_watchdog_stop = false;
void stage(const char* s) { g_stage = s; g_stage_tick = GetTickCount(); }
double g_t_wait_ms = 0, g_t_end_ms = 0;            // last frame: time in xrWaitFrame, time in submit+xrEndFrame
static double qpc_ms() { static LARGE_INTEGER f = {}; if (!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart * 1000.0 / f.QuadPart; }
DWORD WINAPI watchdog_main(LPVOID) {
    const char* last_reported = nullptr; DWORD last_tick_reported = 0;
    while (!g_watchdog_stop) {
        if (g_watchdog_wake) WaitForSingleObject(g_watchdog_wake, 1000); else Sleep(1000);
        if (g_watchdog_stop) break;
        const DWORD now = GetTickCount(); const DWORD t0 = g_stage_tick; const char* s = const_cast<const char*>(g_stage);
        if (now - t0 > 5000 && (s != last_reported || t0 != last_tick_reported)) {
            g_h.diag("xr: STALL -- render thread has been in stage '%s' for %lu ms", s, now - t0);
            last_reported = s; last_tick_reported = t0;
        }
    }
    return 0;
}

PFN_xrGetInstanceProcAddr p_gipa = nullptr;
#define XRFN(name) PFN_##name p_##name = nullptr;
XRFN(xrCreateInstance) XRFN(xrDestroyInstance) XRFN(xrGetInstanceProperties) XRFN(xrGetSystem) XRFN(xrGetSystemProperties)
XRFN(xrCreateSession) XRFN(xrDestroySession) XRFN(xrCreateReferenceSpace) XRFN(xrDestroySpace)
XRFN(xrEnumerateViewConfigurationViews) XRFN(xrEnumerateSwapchainFormats) XRFN(xrCreateSwapchain) XRFN(xrDestroySwapchain)
XRFN(xrEnumerateSwapchainImages) XRFN(xrPollEvent) XRFN(xrBeginSession) XRFN(xrEndSession) XRFN(xrRequestExitSession)
XRFN(xrWaitFrame) XRFN(xrBeginFrame) XRFN(xrEndFrame) XRFN(xrLocateViews)
XRFN(xrAcquireSwapchainImage) XRFN(xrWaitSwapchainImage) XRFN(xrReleaseSwapchainImage) XRFN(xrResultToString)
XRFN(xrGetD3D11GraphicsRequirementsKHR)
XRFN(xrStringToPath) XRFN(xrPathToString) XRFN(xrCreateActionSet) XRFN(xrDestroyActionSet) XRFN(xrCreateAction) XRFN(xrDestroyAction)
XRFN(xrSuggestInteractionProfileBindings) XRFN(xrAttachSessionActionSets) XRFN(xrCreateActionSpace) XRFN(xrSyncActions)
XRFN(xrGetActionStateBoolean) XRFN(xrGetActionStateFloat) XRFN(xrGetActionStateVector2f) XRFN(xrGetActionStatePose)
XRFN(xrLocateSpace) XRFN(xrApplyHapticFeedback) XRFN(xrStopHapticFeedback) XRFN(xrGetCurrentInteractionProfile)
#undef XRFN

XrInstance   g_instance = XR_NULL_HANDLE;
XrSystemId   g_system   = XR_NULL_SYSTEM_ID;
XrSession    g_session  = XR_NULL_HANDLE;
XrSpace      g_space    = XR_NULL_HANDLE;
XrSessionState g_state  = XR_SESSION_STATE_UNKNOWN;
bool g_space_changed = false;   // XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING seen since the last xr_take_space_change
bool         g_running  = false;
bool         g_frame_open = false;
XrTime       g_render_time = 0;
bool         g_should_render = false;
XrView       g_render_views[2] = {};            // located at the last wait: what the frame now rendering uses
bool         g_render_views_valid = false;
XrView       g_submit_views[2] = {};            // located one wait earlier: what the frame being submitted was rendered with
bool         g_submit_views_valid = false;

// Per swapchain (R8a): an image is acquired, then waited, then (only then) written and released. A wait that
// times out leaves it acquired, and the wait is retried next frame with a short timeout; an image that was not
// waited is never released.
enum ScState : uint8_t { SC_IDLE, SC_ACQUIRED, SC_WAITED };
struct Eye {
    XrSwapchain sc = XR_NULL_HANDLE;
    UINT w = 0, h = 0;
    ScState st = SC_IDLE; uint32_t idx = 0; uint32_t retries = 0; bool ever_released = false;
    uint32_t hard_fails = 0;                        // B5-2: consecutive hard errors (XR_FAILED, not timeouts) from acquire / wait
    std::vector<ID3D11Texture2D*> images;
    ID3D11Texture2D* mid = nullptr;                 // UNORM intermediate: shader copy here, then a bit copy into the (sRGB) image
    ID3D11RenderTargetView* mid_rtv = nullptr;
};
Eye g_eye[6];                                   // 0/1: the eyes; 2: the virtual screen (left eye when stereo); 3: the UI panel; 4: the ImGui layer; 5: the stereo screen's right eye
// R8b (with R15): the last layers submitted with fresh images. A frame whose base layer cannot go out fresh (no eye
// image, or a swapchain wait that timed out) re-sends the held base layer for up to 30 frames instead of a black
// zero-layer frame: the swapchains still hold those images (the compositor uses each one's last released image, legal
// while a newer one is acquired), and the held poses are the ones they were rendered with. The UI panel and the ImGui
// layer are held the same way, each with its own age, and only while a base layer goes out.
struct HeldBase {
    bool have = false, use_screen = false, stereo = false; uint32_t age = 0;
    XrCompositionLayerProjectionView pv[2] = {}; XrCompositionLayerQuad quad = {}, quad_r = {};
    bool have_ui = false, have_imgui = false; uint32_t ui_age = 0, imgui_age = 0; XrCompositionLayerQuad uiq = {}, imq = {};
    bool base_ui = false;                         // R8-1: the held base frame went out with a UI panel (only then may a held base bring one)
};
HeldBase g_held;
uint32_t g_held_frames = 0, g_held_episode = 0, g_held_quads = 0;
const uint32_t k_hold_max = 30, k_hard_fail_max = 300;   // frames a held layer may be re-sent; consecutive hard errors before a restart (B5-2, ~5 s)
int g_sc_stuck = -1;                                      // B5-2: a swapchain whose acquire / wait failed hard k_hard_fail_max times in a row
bool g_stuck_stop = false;                                // R8-3: set by the B5-2 stop (xr_stop resets g_sc_stuck, not this); xr_take_stuck clears it
bool g_frame_wait_failed = false;                         // R8-2: a swapchain wait failed in this frame's submits: later first waits use the short timeout
// Battle transition (xr_pin): the projection layer's poses and field of view while pinned; the hold cap lifted.
bool g_pin = false; XrPosef g_pin_pose[2] = {}; XrFovf g_pin_fov[2] = {};
bool g_hold_uncapped = false;
uint32_t g_wait_timeouts = 0;
int g_sim_timeouts = 0;                          // Debug: the next N swapchain waits report a timeout (xr_debug)
// R41: position tracking lost (cameras covered): hold the last valid head centre, eyes at +-IPD/2 along each view's
// own right axis, with the IPD measured from the located views while tracking was good.
constexpr float kIpdMin = 0.04f, kIpdMax = 0.09f, kIpdDefault = 0.063f;
bool g_have_last_pos = false, g_pos_lost = false; XrVector3f g_last_head = {}; float g_last_ipd = kIpdDefault;
XrSpace g_view_space = XR_NULL_HANDLE;           // head-locked space for the UI panel
DXGI_FORMAT g_sc_format = DXGI_FORMAT_UNKNOWN;

const char* res_str(XrResult r);                 // below

// ---------------------------------------------------------------- input
// One action set; every action has both hand subaction paths. Bindings are
// suggested for the Touch controllers and for khr/simple; the runtime picks.
XrActionSet g_aset = XR_NULL_HANDLE;
XrAction g_a_stick = XR_NULL_HANDLE, g_a_stick_click = XR_NULL_HANDLE, g_a_primary = XR_NULL_HANDLE, g_a_secondary = XR_NULL_HANDLE;
XrAction g_a_trigger = XR_NULL_HANDLE, g_a_squeeze = XR_NULL_HANDLE, g_a_menu = XR_NULL_HANDLE;
XrAction g_a_grip = XR_NULL_HANDLE, g_a_aim = XR_NULL_HANDLE, g_a_haptic = XR_NULL_HANDLE;
XrPath   g_hand_path[2] = {};
XrSpace  g_grip_space[2] = {}, g_aim_space[2] = {};
bool     g_input_ok = false, g_input_attached = false;
XrInput  g_input = {};
uint32_t g_sync_fail = 0;

XrPath path(const char* s) { XrPath p = XR_NULL_PATH; if (p_xrStringToPath) p_xrStringToPath(g_instance, s, &p); return p; }

bool make_action(XrAction& a, const char* name, const char* label, XrActionType type) {
    XrActionCreateInfo ci = { XR_TYPE_ACTION_CREATE_INFO };
    ci.actionType = type; ci.countSubactionPaths = 2; ci.subactionPaths = g_hand_path;
    strncpy_s(ci.actionName, name, _TRUNCATE); strncpy_s(ci.localizedActionName, label, _TRUNCATE);
    XrResult r = p_xrCreateAction(g_aset, &ci, &a);
    if (XR_FAILED(r)) { a = XR_NULL_HANDLE; g_h.diag("xr: action %s: %s", name, res_str(r)); return false; }
    return true;
}

/// Instance-level part: action set, actions, suggested bindings. Needs only the instance.
bool input_create() {
    if (!p_xrCreateActionSet) return false;
    g_hand_path[0] = path("/user/hand/left"); g_hand_path[1] = path("/user/hand/right");
    XrActionSetCreateInfo si = { XR_TYPE_ACTION_SET_CREATE_INFO };
    strncpy_s(si.actionSetName, "ffxvr", _TRUNCATE); strncpy_s(si.localizedActionSetName, "FFX VR", _TRUNCATE); si.priority = 0;
    XrResult r = p_xrCreateActionSet(g_instance, &si, &g_aset);
    if (XR_FAILED(r)) { g_aset = XR_NULL_HANDLE; g_h.diag("xr: xrCreateActionSet: %s", res_str(r)); return false; }
    bool ok = true;
    ok &= make_action(g_a_stick,       "thumbstick",       "Thumbstick",        XR_ACTION_TYPE_VECTOR2F_INPUT);
    ok &= make_action(g_a_stick_click, "thumbstick_click", "Thumbstick click",  XR_ACTION_TYPE_BOOLEAN_INPUT);
    ok &= make_action(g_a_primary,     "primary",          "A / X",             XR_ACTION_TYPE_BOOLEAN_INPUT);
    ok &= make_action(g_a_secondary,   "secondary",        "B / Y",             XR_ACTION_TYPE_BOOLEAN_INPUT);
    ok &= make_action(g_a_trigger,     "trigger",          "Trigger",           XR_ACTION_TYPE_FLOAT_INPUT);
    ok &= make_action(g_a_squeeze,     "squeeze",          "Grip",              XR_ACTION_TYPE_FLOAT_INPUT);
    ok &= make_action(g_a_menu,        "menu",             "Menu",              XR_ACTION_TYPE_BOOLEAN_INPUT);
    ok &= make_action(g_a_grip,        "grip_pose",        "Grip pose",         XR_ACTION_TYPE_POSE_INPUT);
    ok &= make_action(g_a_aim,         "aim_pose",         "Aim pose",          XR_ACTION_TYPE_POSE_INPUT);
    ok &= make_action(g_a_haptic,      "haptic",           "Haptic",            XR_ACTION_TYPE_VIBRATION_OUTPUT);
    if (!ok) return false;

    struct B { XrAction a; const char* p; };
    // Touch controllers: the right controller's menu button is the system button and never delivered.
    const B touch[] = {
        { g_a_stick, "/user/hand/left/input/thumbstick" },          { g_a_stick, "/user/hand/right/input/thumbstick" },
        { g_a_stick_click, "/user/hand/left/input/thumbstick/click" }, { g_a_stick_click, "/user/hand/right/input/thumbstick/click" },
        { g_a_primary, "/user/hand/left/input/x/click" },           { g_a_primary, "/user/hand/right/input/a/click" },
        { g_a_secondary, "/user/hand/left/input/y/click" },         { g_a_secondary, "/user/hand/right/input/b/click" },
        { g_a_trigger, "/user/hand/left/input/trigger/value" },     { g_a_trigger, "/user/hand/right/input/trigger/value" },
        { g_a_squeeze, "/user/hand/left/input/squeeze/value" },     { g_a_squeeze, "/user/hand/right/input/squeeze/value" },
        { g_a_menu, "/user/hand/left/input/menu/click" },
        { g_a_grip, "/user/hand/left/input/grip/pose" },            { g_a_grip, "/user/hand/right/input/grip/pose" },
        { g_a_aim, "/user/hand/left/input/aim/pose" },              { g_a_aim, "/user/hand/right/input/aim/pose" },
        { g_a_haptic, "/user/hand/left/output/haptic" },            { g_a_haptic, "/user/hand/right/output/haptic" },
    };
    const B simple[] = {
        { g_a_primary, "/user/hand/left/input/select/click" },      { g_a_primary, "/user/hand/right/input/select/click" },
        { g_a_menu, "/user/hand/left/input/menu/click" },           { g_a_menu, "/user/hand/right/input/menu/click" },
        { g_a_grip, "/user/hand/left/input/grip/pose" },            { g_a_grip, "/user/hand/right/input/grip/pose" },
        { g_a_aim, "/user/hand/left/input/aim/pose" },              { g_a_aim, "/user/hand/right/input/aim/pose" },
        { g_a_haptic, "/user/hand/left/output/haptic" },            { g_a_haptic, "/user/hand/right/output/haptic" },
    };
    auto suggest = [&](const char* profile, const B* b, size_t n) {
        std::vector<XrActionSuggestedBinding> v;
        for (size_t i = 0; i < n; ++i) { XrActionSuggestedBinding sb = { b[i].a, path(b[i].p) }; if (sb.binding != XR_NULL_PATH) v.push_back(sb); }
        XrInteractionProfileSuggestedBinding sbi = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        sbi.interactionProfile = path(profile); sbi.countSuggestedBindings = (uint32_t)v.size(); sbi.suggestedBindings = v.data();
        XrResult rr = p_xrSuggestInteractionProfileBindings(g_instance, &sbi);
        g_h.diag("xr: bindings for %s: %s (%u)", profile, XR_SUCCEEDED(rr) ? "ok" : res_str(rr), (unsigned)v.size());
        return XR_SUCCEEDED(rr);
    };
    bool any = suggest("/interaction_profiles/oculus/touch_controller", touch, sizeof(touch) / sizeof(touch[0]));
    any |= suggest("/interaction_profiles/khr/simple_controller", simple, sizeof(simple) / sizeof(simple[0]));
    return any;
}

/// Session-level part: action spaces and the one-time attach.
bool input_attach() {
    if (g_aset == XR_NULL_HANDLE || g_session == XR_NULL_HANDLE) return false;
    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo asci = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        asci.poseInActionSpace.orientation.w = 1.f; asci.subactionPath = g_hand_path[h];
        asci.action = g_a_grip; if (XR_FAILED(p_xrCreateActionSpace(g_session, &asci, &g_grip_space[h]))) g_grip_space[h] = XR_NULL_HANDLE;
        asci.action = g_a_aim;  if (XR_FAILED(p_xrCreateActionSpace(g_session, &asci, &g_aim_space[h])))  g_aim_space[h]  = XR_NULL_HANDLE;
    }
    XrSessionActionSetsAttachInfo ai = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO }; ai.countActionSets = 1; ai.actionSets = &g_aset;
    XrResult r = p_xrAttachSessionActionSets(g_session, &ai);
    if (XR_FAILED(r)) { g_h.diag("xr: xrAttachSessionActionSets: %s", res_str(r)); return false; }
    g_input_attached = true;
    g_h.diag("xr: action set attached");
    return true;
}

void input_destroy() {
    for (int h = 0; h < 2; ++h) {
        if (g_grip_space[h] != XR_NULL_HANDLE && p_xrDestroySpace) p_xrDestroySpace(g_grip_space[h]);
        if (g_aim_space[h] != XR_NULL_HANDLE && p_xrDestroySpace) p_xrDestroySpace(g_aim_space[h]);
        g_grip_space[h] = g_aim_space[h] = XR_NULL_HANDLE;
    }
    if (g_aset != XR_NULL_HANDLE && p_xrDestroyActionSet) p_xrDestroyActionSet(g_aset);   // destroys its actions
    g_aset = XR_NULL_HANDLE;
    g_a_stick = g_a_stick_click = g_a_primary = g_a_secondary = g_a_trigger = g_a_squeeze = g_a_menu = g_a_grip = g_a_aim = g_a_haptic = XR_NULL_HANDLE;
    g_input_ok = g_input_attached = false; g_input = {};
}

/// Once per frame after the wait: sync and read every action, locate the hands at the frame's display time.
void input_sync(XrTime t) {
    if (!g_input_attached) { g_input.ready = false; return; }
    XrActiveActionSet aas = { g_aset, XR_NULL_PATH };
    XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO }; si.countActiveActionSets = 1; si.activeActionSets = &aas;
    XrResult r = p_xrSyncActions(g_session, &si);
    if (r == XR_SESSION_NOT_FOCUSED) { g_input.ready = false; return; }     // another app has the controllers (the Quest home, say)
    if (XR_FAILED(r)) { if (++g_sync_fail <= 3) g_h.diag("xr: xrSyncActions: %s", res_str(r)); g_input.ready = false; return; }
    g_input.ready = true;
    for (int h = 0; h < 2; ++h) {
        XrHand& hd = g_input.hand[h];
        XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO }; gi.subactionPath = g_hand_path[h];
        XrActionStateVector2f v2 = { XR_TYPE_ACTION_STATE_VECTOR2F }; gi.action = g_a_stick;
        if (XR_SUCCEEDED(p_xrGetActionStateVector2f(g_session, &gi, &v2)) && v2.isActive) { hd.stick[0] = v2.currentState.x; hd.stick[1] = v2.currentState.y; } else hd.stick[0] = hd.stick[1] = 0.f;
        auto getb = [&](XrAction a, bool& out) { XrActionStateBoolean b = { XR_TYPE_ACTION_STATE_BOOLEAN }; gi.action = a; out = XR_SUCCEEDED(p_xrGetActionStateBoolean(g_session, &gi, &b)) && b.isActive && b.currentState == XR_TRUE; return b.isActive == XR_TRUE; };
        auto getf = [&](XrAction a, float& out) { XrActionStateFloat f = { XR_TYPE_ACTION_STATE_FLOAT }; gi.action = a; out = (XR_SUCCEEDED(p_xrGetActionStateFloat(g_session, &gi, &f)) && f.isActive) ? f.currentState : 0.f; return f.isActive == XR_TRUE; };
        bool active = v2.isActive == XR_TRUE;
        active |= getb(g_a_stick_click, hd.stick_click); active |= getb(g_a_primary, hd.primary); active |= getb(g_a_secondary, hd.secondary);
        active |= getf(g_a_trigger, hd.trigger); active |= getf(g_a_squeeze, hd.squeeze); getb(g_a_menu, hd.menu);
        XrActionStatePose ps = { XR_TYPE_ACTION_STATE_POSE }; gi.action = g_a_grip;
        const bool grip_active = XR_SUCCEEDED(p_xrGetActionStatePose(g_session, &gi, &ps)) && ps.isActive;
        active |= grip_active;
        hd.active = active;
        auto locate = [&](XrSpace sp, bool& valid, float* q, float* p) {
            valid = false; if (sp == XR_NULL_HANDLE) return;
            XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
            if (XR_FAILED(p_xrLocateSpace(sp, g_space, t, &loc))) return;
            const bool o = (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0, pp = (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
            if (!o || !pp) return;
            q[0] = loc.pose.orientation.x; q[1] = loc.pose.orientation.y; q[2] = loc.pose.orientation.z; q[3] = loc.pose.orientation.w;
            p[0] = loc.pose.position.x; p[1] = loc.pose.position.y; p[2] = loc.pose.position.z; valid = true;
        };
        locate(g_grip_space[h], hd.grip_valid, hd.grip_q, hd.grip_p);
        locate(g_aim_space[h],  hd.aim_valid,  hd.aim_q,  hd.aim_p);
        // Which profile the runtime bound (for the overlay), refreshed every 2 s.
        static ULONGLONG next_profile = 0; const ULONGLONG now = GetTickCount64();
        if (now >= next_profile && p_xrGetCurrentInteractionProfile) {
            if (h == 1) next_profile = now + 2000;
            XrInteractionProfileState ips = { XR_TYPE_INTERACTION_PROFILE_STATE };
            if (XR_SUCCEEDED(p_xrGetCurrentInteractionProfile(g_session, g_hand_path[h], &ips)) && ips.interactionProfile != XR_NULL_PATH) {
                uint32_t n = 0; p_xrPathToString(g_instance, ips.interactionProfile, sizeof(g_input.profile[h]), &n, g_input.profile[h]);
            }
            else strncpy_s(g_input.profile[h], "(none)", _TRUNCATE);
        }
    }
}

void set_status(const char* s) { strncpy_s(g_status, s, _TRUNCATE); }

const char* res_str(XrResult r) {
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (g_instance != XR_NULL_HANDLE && p_xrResultToString && XR_SUCCEEDED(p_xrResultToString(g_instance, r, buf))) return buf;
    snprintf(buf, sizeof(buf), "XrResult %d", static_cast<int>(r));
    return buf;
}

// One log line per distinct failure; a failure repeating every frame (xrWaitFrame, xrEndFrame) is summarised at
// most every 10 s. The status line still updates on every call.
char g_fail_last[200] = ""; uint32_t g_fail_reps = 0; DWORD g_fail_tick = 0;
void xr_fail_reset() { g_fail_last[0] = '\0'; g_fail_reps = 0; g_fail_tick = 0; }
bool fail(const char* what, XrResult r) {
    char line[200]; snprintf(line, sizeof(line), "%s: %s", what, res_str(r));
    const DWORD now = GetTickCount(); set_status(line);
    if (strcmp(line, g_fail_last) == 0) {
        ++g_fail_reps;
        if (now - g_fail_tick >= 10000) { g_h.diag("xr: %s (x%u in the last %lu s)", line, g_fail_reps, (unsigned long)((now - g_fail_tick) / 1000)); g_fail_reps = 0; g_fail_tick = now; }
        return false;
    }
    if (g_fail_reps) g_h.diag("xr: (previous failure repeated %u more times)", g_fail_reps);
    strncpy_s(g_fail_last, line, _TRUNCATE); g_fail_reps = 0; g_fail_tick = now;
    g_h.diag("xr: %s", line);
    return false;
}

template <typename T> bool load(T& fn, const char* name, XrInstance inst) {
    PFN_xrVoidFunction f = nullptr;
    XrResult r = p_gipa(inst, name, &f);
    if (XR_FAILED(r) || f == nullptr) { g_h.diag("xr: %s not available (%d)", name, static_cast<int>(r)); return false; }
    fn = reinterpret_cast<T>(f);
    return true;
}

void destroy_eyes() {
    for (Eye& e : g_eye) {
        if (e.mid_rtv) { e.mid_rtv->Release(); e.mid_rtv = nullptr; }
        if (e.mid)     { e.mid->Release();     e.mid = nullptr; }
        if (e.sc != XR_NULL_HANDLE && p_xrDestroySwapchain) p_xrDestroySwapchain(e.sc);
        e.sc = XR_NULL_HANDLE; e.images.clear(); e.w = e.h = 0;
        e.st = SC_IDLE; e.idx = 0; e.retries = 0; e.ever_released = false; e.hard_fails = 0;
    }
    g_held = HeldBase{}; g_held_episode = 0; g_sc_stuck = -1;
}

} // namespace

bool xr_active() { return g_session != XR_NULL_HANDLE; }
const char* xr_status() { return g_status; }
void xr_debug(int sim_timeouts) { if (sim_timeouts > 0) g_sim_timeouts += sim_timeouts; }
bool xr_take_stuck() { const bool v = g_stuck_stop; g_stuck_stop = false; return v; }
void xr_counters(unsigned* held_frames, unsigned* wait_timeouts) { if (held_frames) *held_frames = g_held_frames; if (wait_timeouts) *wait_timeouts = g_wait_timeouts; }   // held: base-layer frames (quads held alone are in the diag log)
void xr_eye_size(unsigned* w, unsigned* h) { if (w) *w = g_eye[0].w; if (h) *h = g_eye[0].h; }   // 0 x 0 while no session
bool xr_srgb() { return g_sc_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; }

void xr_stop() {
    if (g_session != XR_NULL_HANDLE) {
        if (g_frame_open && p_xrEndFrame) {           // close the open frame with no layers
            XrFrameEndInfo end = { XR_TYPE_FRAME_END_INFO }; end.displayTime = g_render_time; end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            p_xrEndFrame(g_session, &end); g_frame_open = false;
        }
        // xrEndSession is only valid in STOPPING. poll_events already ends the session there, so this is a backstop that
        // should never fire; otherwise the session is destroyed while running, which the spec allows.
        if (g_running && g_state == XR_SESSION_STATE_STOPPING && p_xrEndSession) p_xrEndSession(g_session);
        g_running = false;
        g_submit_views_valid = false;
        input_destroy();
        destroy_eyes();
        if (g_space != XR_NULL_HANDLE && p_xrDestroySpace) p_xrDestroySpace(g_space);
        g_space = XR_NULL_HANDLE;
        if (g_view_space != XR_NULL_HANDLE && p_xrDestroySpace) p_xrDestroySpace(g_view_space);
        g_view_space = XR_NULL_HANDLE;
        if (p_xrDestroySession) p_xrDestroySession(g_session);
        g_session = XR_NULL_HANDLE;
    }
    input_destroy();
    if (g_instance != XR_NULL_HANDLE && p_xrDestroyInstance) { p_xrDestroyInstance(g_instance); g_instance = XR_NULL_HANDLE; }
    g_state = XR_SESSION_STATE_UNKNOWN; g_render_views_valid = false; g_running = false;
    g_held = HeldBase{}; g_have_last_pos = false; g_pos_lost = false; g_last_ipd = kIpdDefault; g_sim_timeouts = 0; g_sc_stuck = -1;
    g_pin = false; g_hold_uncapped = false;   // battle transition: nothing pinned across a stop
    set_status("off");
    stage("stopped");
    if (g_watchdog) {
        g_watchdog_stop = true; if (g_watchdog_wake) SetEvent(g_watchdog_wake);
        if (WaitForSingleObject(g_watchdog, 3000) == WAIT_OBJECT_0) {   // exited: both handles can go
            CloseHandle(g_watchdog);
            if (g_watchdog_wake) { CloseHandle(g_watchdog_wake); g_watchdog_wake = nullptr; }
        }
        else g_h.diag("xr: watchdog did not exit within 3 s; its handles are left open");   // never close an event a live thread may wait on
        g_watchdog = nullptr;
    }
    g_h.diag("xr: stopped");
}

bool xr_start(const XrHost& host) {
    if (xr_active()) return true;
    xr_fail_reset();                                        // each attempt logs its failures afresh (the previous one may have been muted)
    g_h = host;
    if (g_loader == nullptr) {
        wchar_t path[MAX_PATH];
        _snwprintf_s(path, _TRUNCATE, L"%s\\openxr_loader.dll", g_h.dir);
        g_loader = LoadLibraryW(path);
        if (g_loader == nullptr) { g_h.diag("xr: cannot load %ls (error %lu)", path, GetLastError()); set_status("openxr_loader.dll not found next to the mod"); return false; }
        p_gipa = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(g_loader, "xrGetInstanceProcAddr"));
        if (p_gipa == nullptr) { set_status("loader has no xrGetInstanceProcAddr"); return false; }
    }
    if (!load(p_xrCreateInstance, "xrCreateInstance", XR_NULL_HANDLE)) { set_status("no xrCreateInstance"); return false; }

    const char* exts[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
    strncpy_s(ici.applicationInfo.applicationName, "FFX VR", _TRUNCATE);
    ici.applicationInfo.applicationVersion = 1;
    strncpy_s(ici.applicationInfo.engineName, "ffxvr", _TRUNCATE);
    ici.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    ici.enabledExtensionCount = 1; ici.enabledExtensionNames = exts;
    XrResult r = p_xrCreateInstance(&ici, &g_instance);
    if (XR_FAILED(r)) { g_instance = XR_NULL_HANDLE; return fail("xrCreateInstance (is SteamVR set as the OpenXR runtime, and 32-bit support present?)", r); }

#define L(n) if (!load(p_##n, #n, g_instance)) { xr_stop(); return false; }
    L(xrDestroyInstance) L(xrGetInstanceProperties) L(xrGetSystem) L(xrGetSystemProperties) L(xrCreateSession) L(xrDestroySession)
    L(xrCreateReferenceSpace) L(xrDestroySpace) L(xrEnumerateViewConfigurationViews) L(xrEnumerateSwapchainFormats)
    L(xrCreateSwapchain) L(xrDestroySwapchain) L(xrEnumerateSwapchainImages) L(xrPollEvent) L(xrBeginSession) L(xrEndSession)
    L(xrRequestExitSession) L(xrWaitFrame) L(xrBeginFrame) L(xrEndFrame) L(xrLocateViews) L(xrAcquireSwapchainImage)
    L(xrWaitSwapchainImage) L(xrReleaseSwapchainImage) L(xrResultToString) L(xrGetD3D11GraphicsRequirementsKHR)
    L(xrStringToPath) L(xrPathToString) L(xrCreateActionSet) L(xrDestroyActionSet) L(xrCreateAction) L(xrDestroyAction)
    L(xrSuggestInteractionProfileBindings) L(xrAttachSessionActionSets) L(xrCreateActionSpace) L(xrSyncActions)
    L(xrGetActionStateBoolean) L(xrGetActionStateFloat) L(xrGetActionStateVector2f) L(xrGetActionStatePose)
    L(xrLocateSpace) L(xrApplyHapticFeedback) L(xrStopHapticFeedback) L(xrGetCurrentInteractionProfile)
#undef L

    XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
    if (XR_SUCCEEDED(p_xrGetInstanceProperties(g_instance, &ip)))
        g_h.diag("xr: runtime %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

    g_input_ok = input_create();          // actions + suggested bindings (instance level); input stays off if this fails
    if (!g_input_ok) g_h.diag("xr: controller input not available");

    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO }; sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = p_xrGetSystem(g_instance, &sgi, &g_system);
    if (XR_FAILED(r)) { fail("xrGetSystem (headset not available?)", r); xr_stop(); return false; }
    XrSystemProperties sp = { XR_TYPE_SYSTEM_PROPERTIES };
    if (XR_SUCCEEDED(p_xrGetSystemProperties(g_instance, g_system, &sp))) g_h.diag("xr: system %s", sp.systemName);

    XrGraphicsRequirementsD3D11KHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    r = p_xrGetD3D11GraphicsRequirementsKHR(g_instance, g_system, &req);
    if (XR_FAILED(r)) { fail("xrGetD3D11GraphicsRequirementsKHR", r); xr_stop(); return false; }
    g_h.diag("xr: runtime wants adapter LUID %08X%08X, min feature level 0x%X; game device feature level 0x%X",
             static_cast<unsigned>(req.adapterLuid.HighPart), static_cast<unsigned>(req.adapterLuid.LowPart),
             static_cast<unsigned>(req.minFeatureLevel), static_cast<unsigned>(g_h.device->GetFeatureLevel()));

    XrGraphicsBindingD3D11KHR gb = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR }; gb.device = g_h.device;
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO }; sci.next = &gb; sci.systemId = g_system;
    r = p_xrCreateSession(g_instance, &sci, &g_session);
    if (XR_FAILED(r)) { g_session = XR_NULL_HANDLE; fail("xrCreateSession", r); xr_stop(); return false; }

    XrReferenceSpaceCreateInfo rsci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.f;
    r = p_xrCreateReferenceSpace(g_session, &rsci, &g_space);
    if (XR_FAILED(r)) { fail("xrCreateReferenceSpace", r); xr_stop(); return false; }
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    r = p_xrCreateReferenceSpace(g_session, &rsci, &g_view_space);
    if (XR_FAILED(r)) { fail("xrCreateReferenceSpace (view)", r); xr_stop(); return false; }
    if (g_input_ok && !input_attach()) g_input_ok = false;

    // Swapchains: the headset's recommended width, the game's 16:9 aspect.
    uint32_t nviews = 0; XrViewConfigurationView vcv[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    r = p_xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nviews, vcv);
    if (XR_FAILED(r) || nviews < 2) { fail("xrEnumerateViewConfigurationViews", r); xr_stop(); return false; }
    g_h.diag("xr: recommended per-eye %ux%u (max %ux%u)", vcv[0].recommendedImageRectWidth, vcv[0].recommendedImageRectHeight, vcv[0].maxImageRectWidth, vcv[0].maxImageRectHeight);

    uint32_t nfmt = 0; p_xrEnumerateSwapchainFormats(g_session, 0, &nfmt, nullptr);
    std::vector<int64_t> fmts(nfmt); p_xrEnumerateSwapchainFormats(g_session, nfmt, &nfmt, fmts.data());
    g_sc_format = DXGI_FORMAT_UNKNOWN;
    for (int64_t f : fmts) if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) g_sc_format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (g_sc_format == DXGI_FORMAT_UNKNOWN) for (int64_t f : fmts) if (f == DXGI_FORMAT_R8G8B8A8_UNORM) g_sc_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (g_sc_format == DXGI_FORMAT_UNKNOWN && !fmts.empty()) g_sc_format = static_cast<DXGI_FORMAT>(fmts[0]);
    g_h.diag("xr: %u swapchain formats, using %d", nfmt, static_cast<int>(g_sc_format));

    for (int i = 0; i < 6; ++i) {
        Eye& e = g_eye[i];
        if (i < 2) {
            // The headset's recommended width at the game's 16:9 aspect (the
            // configuration of the long stable run; full 3840x2160 is a
            // separate experiment).
            e.w = vcv[i].recommendedImageRectWidth; e.h = e.w * 9 / 16;
            if (e.h > vcv[i].maxImageRectHeight) { e.h = vcv[i].maxImageRectHeight; e.w = e.h * 16 / 9; }
        }
        else { e.w = 2560; e.h = 1440; }                   // the virtual screen (2, 5), the UI panel (3), the ImGui layer (4)
        XrSwapchainCreateInfo sc = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sc.format = g_sc_format; sc.sampleCount = 1; sc.width = e.w; sc.height = e.h; sc.faceCount = 1; sc.arraySize = 1; sc.mipCount = 1;
        r = p_xrCreateSwapchain(g_session, &sc, &e.sc);
        if (XR_FAILED(r)) { fail("xrCreateSwapchain", r); xr_stop(); return false; }
        uint32_t n = 0; p_xrEnumerateSwapchainImages(e.sc, 0, &n, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> imgs(n, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
        r = p_xrEnumerateSwapchainImages(e.sc, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
        if (XR_FAILED(r)) { fail("xrEnumerateSwapchainImages", r); xr_stop(); return false; }
        for (auto& im : imgs) e.images.push_back(im.texture);

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = e.w; td.Height = e.h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_h.device->CreateTexture2D(&td, nullptr, &e.mid)) || FAILED(g_h.device->CreateRenderTargetView(e.mid, nullptr, &e.mid_rtv))) {
            set_status("intermediate texture failed"); xr_stop(); return false;
        }
        g_h.diag("xr: %s %d swapchain %ux%u, %u images", i < 2 ? "eye" : i == 2 ? "screen" : i == 3 ? "ui" : i == 4 ? "imgui" : "screen right", i, e.w, e.h, n);
    }
    set_status("session created, waiting for the runtime");
    g_h.diag("xr: session created");
    stage("session created");
    if (g_watchdog == nullptr) {
        g_watchdog_stop = false;
        if (g_watchdog_wake == nullptr) g_watchdog_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // before the thread that waits on it
        g_watchdog = CreateThread(nullptr, 0, watchdog_main, nullptr, 0, nullptr);
    }
    return true;
}

namespace {

void poll_events() {
    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
    while (p_xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* sc = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            g_state = sc->state;
            g_h.diag("xr: session state %d", static_cast<int>(g_state));
            if (g_state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO }; bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XrResult r = p_xrBeginSession(g_session, &bi);
                if (XR_SUCCEEDED(r)) { g_running = true; set_status("running"); } else fail("xrBeginSession", r);
            }
            else if (g_state == XR_SESSION_STATE_STOPPING) {
                if (g_frame_open) { XrFrameEndInfo end = { XR_TYPE_FRAME_END_INFO }; end.displayTime = g_render_time; end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE; p_xrEndFrame(g_session, &end); g_frame_open = false; }
                p_xrEndSession(g_session); g_running = false; g_render_views_valid = false; g_held = HeldBase{}; g_pin = false; set_status("stopped by the runtime");   // no held layer (or pin) across a stop
            }
            else if (g_state == XR_SESSION_STATE_LOSS_PENDING || g_state == XR_SESSION_STATE_EXITING) {
                xr_stop(); return;
            }
            else if (g_state == XR_SESSION_STATE_SYNCHRONIZED) set_status("synchronized");
            else if (g_state == XR_SESSION_STATE_VISIBLE) set_status("visible");
            else if (g_state == XR_SESSION_STATE_FOCUSED) set_status("focused");
        }
        else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) { xr_stop(); return; }
        else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            // the runtime recentred its LOCAL space (Quest: hold the Meta button): our reference must follow
            auto* rc = reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&ev);
            if (rc->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                g_space_changed = true; g_held = HeldBase{}; g_have_last_pos = false; g_pin = false;   // held layers / a held head position / pinned poses belong to the old space
                g_h.diag("xr: LOCAL space recentred by the runtime");
            }
        }
        ev = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

// Acquire if idle, wait if acquired. Never releases. True when the image is waited (ours to write).
// short_wait: another member of the same set already failed this frame, so a first wait gets the short timeout too
// (a stall then costs one 250 ms wait per frame, not one per swapchain).
bool prepare(int i, bool short_wait) {
    Eye& e = g_eye[i];
    if (e.st == SC_IDLE) {
        stage(i == 0 ? "acquire eye 0" : i == 1 ? "acquire eye 1" : "acquire");
        XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        const XrResult ar = p_xrAcquireSwapchainImage(e.sc, &ai, &e.idx);
        if (XR_FAILED(ar)) {   // B5-2: a hard error too
            if (++e.hard_fails == 1 || e.hard_fails % 60 == 0) g_h.diag("xr: swapchain %d acquire %s (%u in a row)", i, res_str(ar), e.hard_fails);
            if (e.hard_fails >= k_hard_fail_max && g_sc_stuck < 0) g_sc_stuck = i;
            return false;
        }
        e.st = SC_ACQUIRED; e.retries = 0;
    }
    if (e.st == SC_ACQUIRED) {
        stage(i == 0 ? "wait image eye 0" : i == 1 ? "wait image eye 1" : "wait image");
        XrResult r;
        if (g_sim_timeouts > 0) { g_sim_timeouts--; r = XR_TIMEOUT_EXPIRED; }   // Debug: a simulated timeout
        else {
            XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            wi.timeout = (e.retries || short_wait || g_frame_wait_failed) ? 2000000 : 250000000;   // 250 ms after an acquire, 2 ms on retries and once any wait failed this frame (R8-2): a long stall costs the monitor ~2 ms a frame
            r = p_xrWaitSwapchainImage(e.sc, &wi);
        }
        if (r == XR_TIMEOUT_EXPIRED || XR_FAILED(r)) {               // a timeout or an error: stays acquired, wait again next frame
            g_frame_wait_failed = true;
                                                                     // (other success codes, e.g. SESSION_LOSS_PENDING, mean the image was waited)
            e.retries++; ++g_wait_timeouts;                          // count first, then decide whether to log
            if (g_wait_timeouts <= 5 || e.retries % 300 == 1) g_h.diag("xr: swapchain %d wait %s (try %u)", i, res_str(r), e.retries);
            if (XR_FAILED(r)) { if (++e.hard_fails >= k_hard_fail_max && g_sc_stuck < 0) g_sc_stuck = i; }   // B5-2: hard errors in a row escalate
            else e.hard_fails = 0;                                   // a timeout: the runtime answers normally
            return false;
        }
        if (e.retries) g_h.diag("xr: swapchain %d wait recovered after %u tries", i, e.retries);
        e.st = SC_WAITED; e.hard_fails = 0;
    }
    return true;
}
// Only for SC_WAITED: copy the source in and release.
void copy_release(int i, ID3D11Texture2D* src) {
    Eye& e = g_eye[i];
    stage(i == 0 ? "copy eye 0" : i == 1 ? "copy eye 1" : "copy");
    if (src != nullptr && g_h.copy(src, e.mid_rtv, e.w, e.h, i == 3 || i == 4)) g_h.ctx->CopyResource(e.images[e.idx], e.mid);
    stage("release image");
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    if (XR_SUCCEEDED(p_xrReleaseSwapchainImage(e.sc, &ri))) e.ever_released = true;
    e.st = SC_IDLE;
}
// All or nothing: prepare every needed swapchain (no short-circuit), then copy and release all. If any is not
// ready, nothing is released, and the ready ones stay SC_WAITED into the next frame (legal): a layer never gets a
// left-new / right-old pair.
bool submit_set(const int* ids, ID3D11Texture2D* const* srcs, int n) {
    bool all = true;
    for (int k = 0; k < n; ++k) all &= prepare(ids[k], !all);   // no short-circuit: every member is acquired; after a failure, short waits
    if (!all) return false;
    for (int k = 0; k < n; ++k) copy_release(ids[k], srcs[k]);
    return true;
}
bool submit_eye(int i, ID3D11Texture2D* src) { return submit_set(&i, &src, 1); }

// R41 helper: the view's right axis (x of its orientation).
XrVector3f right_axis(const XrQuaternionf& q) {
    return { 1.f - 2.f * (q.y * q.y + q.z * q.z), 2.f * (q.x * q.y + q.w * q.z), 2.f * (q.x * q.z - q.w * q.y) };
}

} // namespace

const char* xr_stage() { return const_cast<const char*>(g_stage); }

void xr_frame(ID3D11Texture2D* eye1, ID3D11Texture2D* eye2, const XrFovTan used[2], const XrScreen* screen, const XrUiPanel* ui, XrFrameOut* next) {
    if (next) next->have_views = false;
    if (!xr_active()) return;
    stage("poll events");
    poll_events();
    if (!xr_active() || !g_running) { stage("game (session not running)"); return; }

    // End the frame that was just rendered, with the poses it was rendered from.
    if (g_frame_open) {
        g_frame_wait_failed = false;   // R8-2: per frame
        if (!g_should_render) g_held = HeldBase{};   // R8-5: nothing from before a no-render stretch is ever re-sent
        XrCompositionLayerProjectionView pv[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
        XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
        XrCompositionLayerQuad quad_r = { XR_TYPE_COMPOSITION_LAYER_QUAD };   // the stereo screen's right eye
        bool use_screen = screen != nullptr && screen->on;
        bool stereo_screen = use_screen && screen->stereo && screen->eye2 != nullptr;
        bool ok = g_should_render && g_render_views_valid && eye1 != nullptr;   // R15: a frame without an eye image still runs (events, wait, begin, locate)
        if (ok && use_screen) {
            const int ids[2] = { 2, 5 }; ID3D11Texture2D* const srcs[2] = { eye1, screen->eye2 };
            ok = submit_set(ids, srcs, stereo_screen ? 2 : 1);
            quad.space = g_space; quad.eyeVisibility = stereo_screen ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = g_eye[2].sc;
            quad.subImage.imageRect.offset = { 0, 0 };
            quad.subImage.imageRect.extent = { static_cast<int32_t>(g_eye[2].w), static_cast<int32_t>(g_eye[2].h) };
            quad.pose.orientation = { screen->q[0], screen->q[1], screen->q[2], screen->q[3] };
            quad.pose.position = { screen->p[0], screen->p[1], screen->p[2] };
            quad.size = { screen->width, screen->width * 9.f / 16.f };
            if (stereo_screen) {
                quad_r = quad; quad_r.eyeVisibility = XR_EYE_VISIBILITY_RIGHT;
                quad_r.subImage.swapchain = g_eye[5].sc;
                quad_r.subImage.imageRect.extent = { static_cast<int32_t>(g_eye[5].w), static_cast<int32_t>(g_eye[5].h) };
            }
        }
        else if (ok) {
            const int ids[2] = { 0, 1 }; ID3D11Texture2D* const srcs[2] = { eye1, eye2 ? eye2 : eye1 };
            ok = submit_set(ids, srcs, 2);   // both eyes or neither
            for (int i = 0; i < 2 && ok; ++i) {
                if (g_pin) { pv[i].pose = g_pin_pose[i]; pv[i].fov = g_pin_fov[i]; }   // battle transition: rendered from the frozen pose
                else {
                    pv[i].pose = g_render_views[i].pose;
                    pv[i].fov.angleLeft = atanf(used[i].l); pv[i].fov.angleRight = atanf(used[i].r);
                    pv[i].fov.angleUp   = atanf(used[i].u); pv[i].fov.angleDown  = atanf(used[i].d);
                }
                pv[i].subImage.swapchain = g_eye[i].sc;
                pv[i].subImage.imageRect.offset = { 0, 0 };
                pv[i].subImage.imageRect.extent = { static_cast<int32_t>(g_eye[i].w), static_cast<int32_t>(g_eye[i].h) };
                pv[i].subImage.imageArrayIndex = 0;
            }
        }
        // R8b: keep the last fresh base layer; a frame whose base cannot go out fresh (no eye image, a wait that timed
        // out, no located views) re-sends it, up to k_hold_max frames, then zero layers (black) as before.
        bool held = false;
        if (ok) {
            g_held.have = true; g_held.age = 0; g_held.use_screen = use_screen; g_held.stereo = stereo_screen;
            if (use_screen) { g_held.quad = quad; g_held.quad_r = quad_r; } else { g_held.pv[0] = pv[0]; g_held.pv[1] = pv[1]; }
            if (g_held_episode) { g_h.diag("xr: fresh frames again after %u held (UI / ImGui quads held so far %u)", g_held_episode, g_held_quads); g_held_episode = 0; }
        }
        else if (g_held.have && g_held.use_screen != use_screen) {   // R8-4 (re-review nit): a mode change drops the held base for good
            g_held.have = false;
            g_h.diag("xr: mode change (%s): the held frame is dropped%s", use_screen ? "3D -> screen" : "screen -> 3D", g_held_episode ? " during a hold" : "");   // HP-7
        }
        else if (g_should_render && g_held.have && (g_held.age < k_hold_max || g_hold_uncapped)) {   // R8-4: the same mode only (mono/stereo screen may differ; a different mode was dropped above); no cap inside a battle transition window
            bool ready = g_held.use_screen ? (g_eye[2].ever_released && (!g_held.stereo || g_eye[5].ever_released)) : (g_eye[0].ever_released && g_eye[1].ever_released);
            if (ready) {
                held = true; g_held.age++; g_held_frames++;
                if (g_held_episode++ == 0) {
                    if (g_hold_uncapped) g_h.diag("xr: %s: holding the last frame (battle transition: no frame cap)", eye1 == nullptr ? "no eye image this frame" : !g_render_views_valid ? "no located views" : "a swapchain was not ready (wait timeout or error)");
                    else g_h.diag("xr: %s: holding the last frame (at most %u frames)", eye1 == nullptr ? "no eye image this frame" : !g_render_views_valid ? "no located views" : "a swapchain was not ready (wait timeout or error)", k_hold_max);
                }
                use_screen = g_held.use_screen; stereo_screen = g_held.stereo;
                if (use_screen) { quad = g_held.quad; quad_r = g_held.quad_r; } else { pv[0] = g_held.pv[0]; pv[1] = g_held.pv[1]; }   // the poses those images were rendered with
            }
        }
        const bool base = ok || held;
        XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        layer.space = g_space; layer.viewCount = 2; layer.views = pv;
        // The UI panel: head-locked (VIEW space), alpha-blended over the scene. Its distance and width are the host's (the adaptive
        // panel moves it in front of near geometry and scales the width with the distance); the texture may carry the backing plate.
        XrCompositionLayerQuad uiq = { XR_TYPE_COMPOSITION_LAYER_QUAD };
        bool have_ui = false;
        const bool ui_src = base && !use_screen && ui != nullptr && ui->tex != nullptr;
        if (ui_src && submit_eye(3, ui->tex)) {
            have_ui = true;
            uiq.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
            uiq.space = g_view_space; uiq.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            uiq.subImage.swapchain = g_eye[3].sc;
            uiq.subImage.imageRect.offset = { 0, 0 };
            uiq.subImage.imageRect.extent = { static_cast<int32_t>(g_eye[3].w), static_cast<int32_t>(g_eye[3].h) };
            uiq.pose.orientation = { 0, 0, 0, 1 };
            uiq.pose.position = { 0, 0, -ui->dist };
            uiq.size = { ui->width, ui->width * 9.f / 16.f };
            g_held.have_ui = true; g_held.ui_age = 0; g_held.uiq = uiq;
        }
        // R8b: the UI panel held when its own image could not go out (a timeout), or when the base is held (a no-image frame has
        // no UI image either); never in the screen modes, and not when the frame legitimately has no UI (a fresh base without it)
        else if (base && !use_screen && (ui_src || (held && g_held.base_ui)) && g_held.have_ui && g_held.ui_age < k_hold_max && g_eye[3].ever_released) {   // R8-1: a held base brings a UI only if it had one
            have_ui = true; uiq = g_held.uiq; g_held.ui_age++; g_held_quads++;
        }
        if (ok) g_held.base_ui = have_ui;   // R8-1: what the held base frame went out with
        XrCompositionLayerQuad imq = { XR_TYPE_COMPOSITION_LAYER_QUAD };
        bool have_imgui = false;
        const bool im_src = base && ui != nullptr && ui->tex2 != nullptr;   // the ImGui image exists whenever the overlay draws (no-image frames included)
        if (im_src && submit_eye(4, ui->tex2)) {
            have_imgui = true;
            imq.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
            imq.space = g_view_space; imq.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            imq.subImage.swapchain = g_eye[4].sc;
            imq.subImage.imageRect.offset = { 0, 0 };
            imq.subImage.imageRect.extent = { static_cast<int32_t>(g_eye[4].w), static_cast<int32_t>(g_eye[4].h) };
            imq.pose.orientation = { 0, 0, 0, 1 };
            imq.pose.position = { 0, 0, -(ui->im_dist * 0.95f) };   // uidepth: the settings' distance, not the UI panel's adaptive one
            imq.size = { ui->im_width * 0.95f, ui->im_width * 0.95f * 9.f / 16.f };
            g_held.have_imgui = true; g_held.imgui_age = 0; g_held.imq = imq;
        }
        else if (im_src && g_held.have_imgui && g_held.imgui_age < k_hold_max && g_eye[4].ever_released) {   // R8b: its own wait timed out
            have_imgui = true; imq = g_held.imq; g_held.imgui_age++; g_held_quads++;
        }
        const XrCompositionLayerBaseHeader* layers[4] = {
            use_screen ? reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad) : reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer),
            nullptr, nullptr, nullptr };
        uint32_t nl = 1;
        if (stereo_screen) layers[nl++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad_r);
        if (have_ui) layers[nl++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&uiq);
        if (have_imgui) layers[nl++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&imq);
        XrFrameEndInfo end = { XR_TYPE_FRAME_END_INFO };
        end.displayTime = g_render_time; end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = base ? nl : 0u; end.layers = base ? layers : nullptr;
        stage("xrEndFrame");
        const double t_end0 = qpc_ms();
        XrResult r = p_xrEndFrame(g_session, &end);
        g_t_end_ms = qpc_ms() - t_end0;
        g_frame_open = false;
        if (XR_FAILED(r)) fail("xrEndFrame", r);
        if (g_sc_stuck >= 0) {   // B5-2: a swapchain that keeps failing hard (not timing out) never recovers by itself: restart the session
            const int stuck = g_sc_stuck;
            g_h.diag("xr: swapchain %d stuck (%u hard errors in a row): stopping the session; the start back-off restarts it", stuck, k_hard_fail_max);
            xr_stop();
            g_stuck_stop = true;   // R8-3: counted as a failed start by xr_tick (after xr_stop, which leaves this flag alone)
            char st[64]; snprintf(st, sizeof(st), "swapchain %d stuck: restarting", stuck); set_status(st);
            return;
        }
    }

    // Wait for the next frame and locate the eyes for it.
    stage("xrWaitFrame");
    XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs = { XR_TYPE_FRAME_STATE };
    const int lk = g_h.wait_begin ? g_h.wait_begin() : 0;   // load lock build (load_lock_release, default off): may give up the game's lock for the wait
    const double t_wait0 = qpc_ms();
    XrResult r = p_xrWaitFrame(g_session, &fwi, &fs);
    g_t_wait_ms = qpc_ms() - t_wait0;   // the wait alone; taking the lock back is timed by the host (the prof block)
    if (lk) stage("lock re-acquire");      // LL-4: a hang here is ours (the game's lock), not the runtime's xrWaitFrame
    if (g_h.wait_end) g_h.wait_end(lk);   // every path: the lock is ours again before anything else runs
    if (XR_FAILED(r)) { fail("xrWaitFrame", r); stage("game"); return; }
    stage("xrBeginFrame");
    XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
    r = p_xrBeginFrame(g_session, &fbi);
    if (XR_FAILED(r) ) { fail("xrBeginFrame", r); stage("game"); return; }
    g_frame_open = true;
    g_render_time = fs.predictedDisplayTime;
    g_should_render = fs.shouldRender == XR_TRUE;
    stage("xrSyncActions");
    input_sync(g_render_time);

    XrViewLocateInfo vli = { XR_TYPE_VIEW_LOCATE_INFO };
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; vli.displayTime = g_render_time; vli.space = g_space;
    XrViewState vs = { XR_TYPE_VIEW_STATE };
    XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    uint32_t n = 0;
    stage("xrLocateViews");
    r = p_xrLocateViews(g_session, &vli, &vs, 2, &n, views);
    const bool valid = XR_SUCCEEDED(r) && n == 2 && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
    g_render_views_valid = valid;
    stage("game");
    if (!valid) return;
    // R41: with position tracking lost, hold the last valid head centre and keep the eyes apart (they used to
    // collapse to the origin: zero eye separation, the world jumping).
    const bool pos_ok = (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
    if (pos_ok) {
        const XrVector3f a = views[0].pose.position, b = views[1].pose.position;
        g_last_head = { 0.5f * (a.x + b.x), 0.5f * (a.y + b.y), 0.5f * (a.z + b.z) }; g_have_last_pos = true;
        const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z, ipd = sqrtf(dx*dx + dy*dy + dz*dz);
        if (ipd >= kIpdMin && ipd <= kIpdMax) g_last_ipd = ipd;
        if (g_pos_lost) { g_pos_lost = false; g_h.diag("xr: position tracking back"); }
    }
    else {
        if (!g_pos_lost) { g_pos_lost = true; g_h.diag("xr: position tracking lost: holding the head at (%.2f %.2f %.2f), eye separation %.3f m", g_have_last_pos ? g_last_head.x : 0.f, g_have_last_pos ? g_last_head.y : 0.f, g_have_last_pos ? g_last_head.z : 0.f, g_last_ipd); }
        const XrVector3f c = g_have_last_pos ? g_last_head : XrVector3f{ 0.f, 0.f, 0.f };
        for (int i = 0; i < 2; ++i) {
            const XrVector3f rx = right_axis(views[i].pose.orientation); const float h = (i == 0 ? -0.5f : 0.5f) * g_last_ipd;
            views[i].pose.position = { c.x + rx.x * h, c.y + rx.y * h, c.z + rx.z * h };
        }
    }
    memcpy(g_render_views, views, sizeof(views));
    if (next) {
        next->have_views = true;
        for (int i = 0; i < 2; ++i) {
            next->view[i].q[0] = views[i].pose.orientation.x; next->view[i].q[1] = views[i].pose.orientation.y;
            next->view[i].q[2] = views[i].pose.orientation.z; next->view[i].q[3] = views[i].pose.orientation.w;
            next->view[i].p[0] = views[i].pose.position.x;   // valid, or held by R41 above
            next->view[i].p[1] = views[i].pose.position.y;
            next->view[i].p[2] = views[i].pose.position.z;
            next->fov[i].l = tanf(views[i].fov.angleLeft); next->fov[i].r = tanf(views[i].fov.angleRight);
            next->fov[i].u = tanf(views[i].fov.angleUp);   next->fov[i].d = tanf(views[i].fov.angleDown);
        }
    }
}

void xr_input_get(XrInput* out) { if (out) *out = g_input; }
void xr_haptic_stop(int hand) {
    if (!g_input_attached || hand < 0 || hand > 1 || !p_xrStopHapticFeedback) return;
    XrHapticActionInfo hai = { XR_TYPE_HAPTIC_ACTION_INFO }; hai.action = g_a_haptic; hai.subactionPath = g_hand_path[hand];
    p_xrStopHapticFeedback(g_session, &hai);
}
bool xr_focused() { return g_session != XR_NULL_HANDLE && g_running && g_state == XR_SESSION_STATE_FOCUSED; }
void xr_haptic(int hand, float amplitude, float seconds) {
    if (!g_input_attached || hand < 0 || hand > 1 || !p_xrApplyHapticFeedback) return;
    XrHapticVibration hv = { XR_TYPE_HAPTIC_VIBRATION }; hv.amplitude = amplitude; hv.duration = (XrDuration)(seconds * 1e9); hv.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo hai = { XR_TYPE_HAPTIC_ACTION_INFO }; hai.action = g_a_haptic; hai.subactionPath = g_hand_path[hand];
    p_xrApplyHapticFeedback(g_session, &hai, reinterpret_cast<const XrHapticBaseHeader*>(&hv));
}

void xr_timing(double* wait_ms, double* end_ms) { if (wait_ms) *wait_ms = g_t_wait_ms; if (end_ms) *end_ms = g_t_end_ms; }

bool xr_take_space_change() { const bool v = g_space_changed; g_space_changed = false; return v; }

bool xr_pin(bool on, const XrFovTan used[2]) {
    if (!on) {
        if (g_pin && g_h.diag) g_h.diag("xr: transition pin released");
        g_pin = false; return true;
    }
    if (g_pin) return true;
    if (!xr_active() || !g_running || !g_render_views_valid || used == nullptr) return false;
    for (int i = 0; i < 2; ++i) {
        g_pin_pose[i] = g_render_views[i].pose;   // what the frame about to be submitted was rendered with (located at the last wait)
        g_pin_fov[i].angleLeft = atanf(used[i].l); g_pin_fov[i].angleRight = atanf(used[i].r);
        g_pin_fov[i].angleUp   = atanf(used[i].u); g_pin_fov[i].angleDown  = atanf(used[i].d);
    }
    g_pin = true;
    if (g_h.diag) g_h.diag("xr: transition pin taken: left eye at (%.2f %.2f %.2f), fov L %.0f R %.0f U %.0f D %.0f deg",
                           g_pin_pose[0].position.x, g_pin_pose[0].position.y, g_pin_pose[0].position.z,
                           g_pin_fov[0].angleLeft * 57.2958f, g_pin_fov[0].angleRight * 57.2958f, g_pin_fov[0].angleUp * 57.2958f, g_pin_fov[0].angleDown * 57.2958f);
    return true;
}
bool xr_pinned() { return g_pin; }
void xr_hold_uncapped(bool on) { g_hold_uncapped = on; }
