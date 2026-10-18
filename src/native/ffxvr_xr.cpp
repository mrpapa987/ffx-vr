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
volatile bool g_watchdog_stop = false;
void stage(const char* s) { g_stage = s; g_stage_tick = GetTickCount(); }
DWORD WINAPI watchdog_main(LPVOID) {
    const char* last_reported = nullptr; DWORD last_tick_reported = 0;
    while (!g_watchdog_stop) {
        Sleep(1000);
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
#undef XRFN

XrInstance   g_instance = XR_NULL_HANDLE;
XrSystemId   g_system   = XR_NULL_SYSTEM_ID;
XrSession    g_session  = XR_NULL_HANDLE;
XrSpace      g_space    = XR_NULL_HANDLE;
XrSessionState g_state  = XR_SESSION_STATE_UNKNOWN;
bool         g_running  = false;
bool         g_frame_open = false;
XrTime       g_render_time = 0;
bool         g_should_render = false;
XrView       g_render_views[2] = {};            // located at the last wait: what the frame now rendering uses
bool         g_render_views_valid = false;
XrView       g_submit_views[2] = {};            // located one wait earlier: what the frame being submitted was rendered with
bool         g_submit_views_valid = false;

struct Eye {
    XrSwapchain sc = XR_NULL_HANDLE;
    UINT w = 0, h = 0;
    std::vector<ID3D11Texture2D*> images;
    ID3D11Texture2D* mid = nullptr;                 // UNORM intermediate: shader copy here, then a bit copy into the (sRGB) image
    ID3D11RenderTargetView* mid_rtv = nullptr;
};
Eye g_eye[4];                                   // 0/1: the eyes; 2: the virtual screen; 3: the UI panel
XrSpace g_view_space = XR_NULL_HANDLE;           // head-locked space for the UI panel
DXGI_FORMAT g_sc_format = DXGI_FORMAT_UNKNOWN;

void set_status(const char* s) { strncpy_s(g_status, s, _TRUNCATE); }

const char* res_str(XrResult r) {
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (g_instance != XR_NULL_HANDLE && p_xrResultToString && XR_SUCCEEDED(p_xrResultToString(g_instance, r, buf))) return buf;
    snprintf(buf, sizeof(buf), "XrResult %d", static_cast<int>(r));
    return buf;
}

bool fail(const char* what, XrResult r) {
    char line[200]; snprintf(line, sizeof(line), "%s: %s", what, res_str(r));
    g_h.diag("xr: %s", line); set_status(line);
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
    }
}

} // namespace

bool xr_active() { return g_session != XR_NULL_HANDLE; }
const char* xr_status() { return g_status; }

void xr_stop() {
    if (g_session != XR_NULL_HANDLE) {
        if (g_frame_open && p_xrEndFrame) {           // close the open frame with no layers
            XrFrameEndInfo end = { XR_TYPE_FRAME_END_INFO }; end.displayTime = g_render_time; end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            p_xrEndFrame(g_session, &end); g_frame_open = false;
        }
        if (g_running && p_xrEndSession) { p_xrEndSession(g_session); g_running = false; }
        g_submit_views_valid = false;
        destroy_eyes();
        if (g_space != XR_NULL_HANDLE && p_xrDestroySpace) p_xrDestroySpace(g_space);
        g_space = XR_NULL_HANDLE;
        if (g_view_space != XR_NULL_HANDLE && p_xrDestroySpace) p_xrDestroySpace(g_view_space);
        g_view_space = XR_NULL_HANDLE;
        if (p_xrDestroySession) p_xrDestroySession(g_session);
        g_session = XR_NULL_HANDLE;
    }
    if (g_instance != XR_NULL_HANDLE && p_xrDestroyInstance) { p_xrDestroyInstance(g_instance); g_instance = XR_NULL_HANDLE; }
    g_state = XR_SESSION_STATE_UNKNOWN; g_render_views_valid = false; g_running = false;
    set_status("off");
    stage("stopped");
    if (g_watchdog) { g_watchdog_stop = true; WaitForSingleObject(g_watchdog, 3000); CloseHandle(g_watchdog); g_watchdog = nullptr; }
    g_h.diag("xr: stopped");
}

bool xr_start(const XrHost& host) {
    if (xr_active()) return true;
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
#undef L

    XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
    if (XR_SUCCEEDED(p_xrGetInstanceProperties(g_instance, &ip)))
        g_h.diag("xr: runtime %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

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

    for (int i = 0; i < 4; ++i) {
        Eye& e = g_eye[i];
        if (i < 2) {
            // The headset's recommended width at the game's 16:9 aspect (the
            // configuration of the long stable run; full 3840x2160 is a
            // separate experiment).
            e.w = vcv[i].recommendedImageRectWidth; e.h = e.w * 9 / 16;
            if (e.h > vcv[i].maxImageRectHeight) { e.h = vcv[i].maxImageRectHeight; e.w = e.h * 16 / 9; }
        }
        else { e.w = 2560; e.h = 1440; }                   // the virtual screen (2) and the UI panel (3)
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
        g_h.diag("xr: %s %d swapchain %ux%u, %u images", i < 2 ? "eye" : i == 2 ? "screen" : "ui", i, e.w, e.h, n);
    }
    set_status("session created, waiting for the runtime");
    g_h.diag("xr: session created");
    stage("session created");
    if (g_watchdog == nullptr) { g_watchdog_stop = false; g_watchdog = CreateThread(nullptr, 0, watchdog_main, nullptr, 0, nullptr); }
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
                p_xrEndSession(g_session); g_running = false; g_render_views_valid = false; set_status("stopped by the runtime");
            }
            else if (g_state == XR_SESSION_STATE_LOSS_PENDING || g_state == XR_SESSION_STATE_EXITING) {
                xr_stop(); return;
            }
            else if (g_state == XR_SESSION_STATE_SYNCHRONIZED) set_status("synchronized");
            else if (g_state == XR_SESSION_STATE_VISIBLE) set_status("visible");
            else if (g_state == XR_SESSION_STATE_FOCUSED) set_status("focused");
        }
        else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) { xr_stop(); return; }
        ev = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

uint32_t g_wait_timeouts = 0;
bool submit_eye(int i, ID3D11Texture2D* src) {
    Eye& e = g_eye[i];
    uint32_t idx = 0;
    stage(i == 0 ? "acquire eye 0" : "acquire eye 1");
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (XR_FAILED(p_xrAcquireSwapchainImage(e.sc, &ai, &idx))) return false;
    stage(i == 0 ? "wait image eye 0" : "wait image eye 1");
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = 250000000;   // 250 ms, never forever
    XrResult r = p_xrWaitSwapchainImage(e.sc, &wi);
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    if (r == XR_TIMEOUT_EXPIRED) { g_wait_timeouts++; if (g_wait_timeouts <= 5) g_h.diag("xr: swapchain image wait timed out (eye %d)", i); p_xrReleaseSwapchainImage(e.sc, &ri); return false; }
    if (XR_FAILED(r)) { p_xrReleaseSwapchainImage(e.sc, &ri); return false; }
    stage(i == 0 ? "copy eye 0" : "copy eye 1");
    if (src != nullptr && g_h.copy(src, e.mid_rtv, e.w, e.h, i == 3)) g_h.ctx->CopyResource(e.images[idx], e.mid);
    stage("release image");
    p_xrReleaseSwapchainImage(e.sc, &ri);
    return true;
}

} // namespace

void xr_frame(ID3D11Texture2D* eye1, ID3D11Texture2D* eye2, const XrFovTan used[2], const XrScreen* screen, const XrUiPanel* ui, XrFrameOut* next) {
    if (next) next->have_views = false;
    if (!xr_active()) return;
    stage("poll events");
    poll_events();
    if (!xr_active() || !g_running) { stage("game (session not running)"); return; }

    // End the frame that was just rendered, with the poses it was rendered from.
    if (g_frame_open) {
        XrCompositionLayerProjectionView pv[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
        XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
        const bool use_screen = screen != nullptr && screen->on;
        bool ok = g_should_render && g_render_views_valid;
        if (ok && use_screen) {
            ok = submit_eye(2, eye1);
            quad.space = g_space; quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = g_eye[2].sc;
            quad.subImage.imageRect.offset = { 0, 0 };
            quad.subImage.imageRect.extent = { static_cast<int32_t>(g_eye[2].w), static_cast<int32_t>(g_eye[2].h) };
            quad.pose.orientation = { screen->q[0], screen->q[1], screen->q[2], screen->q[3] };
            quad.pose.position = { screen->p[0], screen->p[1], screen->p[2] };
            quad.size = { screen->width, screen->width * 9.f / 16.f };
        }
        else if (ok) {
            for (int i = 0; i < 2; ++i) {
                if (!submit_eye(i, i == 0 ? eye1 : (eye2 ? eye2 : eye1))) { ok = false; break; }
                pv[i].pose = g_render_views[i].pose;
                pv[i].fov.angleLeft = atanf(used[i].l); pv[i].fov.angleRight = atanf(used[i].r);
                pv[i].fov.angleUp   = atanf(used[i].u); pv[i].fov.angleDown  = atanf(used[i].d);
                pv[i].subImage.swapchain = g_eye[i].sc;
                pv[i].subImage.imageRect.offset = { 0, 0 };
                pv[i].subImage.imageRect.extent = { static_cast<int32_t>(g_eye[i].w), static_cast<int32_t>(g_eye[i].h) };
                pv[i].subImage.imageArrayIndex = 0;
            }
        }
        XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        layer.space = g_space; layer.viewCount = 2; layer.views = pv;
        // The UI panel: head-locked (VIEW space), alpha-blended over the scene.
        XrCompositionLayerQuad uiq = { XR_TYPE_COMPOSITION_LAYER_QUAD };
        bool have_ui = false;
        if (ok && !use_screen && ui != nullptr && ui->tex != nullptr && submit_eye(3, ui->tex)) {
            have_ui = true;
            uiq.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
            uiq.space = g_view_space; uiq.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            uiq.subImage.swapchain = g_eye[3].sc;
            uiq.subImage.imageRect.offset = { 0, 0 };
            uiq.subImage.imageRect.extent = { static_cast<int32_t>(g_eye[3].w), static_cast<int32_t>(g_eye[3].h) };
            uiq.pose.orientation = { 0, 0, 0, 1 };
            uiq.pose.position = { 0, 0, -ui->dist };
            uiq.size = { ui->width, ui->width * 9.f / 16.f };
        }
        const XrCompositionLayerBaseHeader* layers[2] = {
            use_screen ? reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad) : reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer),
            reinterpret_cast<XrCompositionLayerBaseHeader*>(&uiq) };
        XrFrameEndInfo end = { XR_TYPE_FRAME_END_INFO };
        end.displayTime = g_render_time; end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = ok ? (have_ui ? 2u : 1u) : 0u; end.layers = ok ? layers : nullptr;
        stage("xrEndFrame");
        XrResult r = p_xrEndFrame(g_session, &end);
        g_frame_open = false;
        if (XR_FAILED(r)) fail("xrEndFrame", r);
    }

    // Wait for the next frame and locate the eyes for it.
    stage("xrWaitFrame");
    XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs = { XR_TYPE_FRAME_STATE };
    XrResult r = p_xrWaitFrame(g_session, &fwi, &fs);
    if (XR_FAILED(r)) { fail("xrWaitFrame", r); stage("game"); return; }
    stage("xrBeginFrame");
    XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
    r = p_xrBeginFrame(g_session, &fbi);
    if (XR_FAILED(r) ) { fail("xrBeginFrame", r); stage("game"); return; }
    g_frame_open = true;
    g_render_time = fs.predictedDisplayTime;
    g_should_render = fs.shouldRender == XR_TRUE;

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
    memcpy(g_render_views, views, sizeof(views));
    if (next) {
        next->have_views = true;
        for (int i = 0; i < 2; ++i) {
            next->view[i].q[0] = views[i].pose.orientation.x; next->view[i].q[1] = views[i].pose.orientation.y;
            next->view[i].q[2] = views[i].pose.orientation.z; next->view[i].q[3] = views[i].pose.orientation.w;
            const bool pos_ok = (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
            next->view[i].p[0] = pos_ok ? views[i].pose.position.x : 0.f;
            next->view[i].p[1] = pos_ok ? views[i].pose.position.y : 0.f;
            next->view[i].p[2] = pos_ok ? views[i].pose.position.z : 0.f;
            next->fov[i].l = tanf(views[i].fov.angleLeft); next->fov[i].r = tanf(views[i].fov.angleRight);
            next->fov[i].u = tanf(views[i].fov.angleUp);   next->fov[i].d = tanf(views[i].fov.angleDown);
        }
    }
}
