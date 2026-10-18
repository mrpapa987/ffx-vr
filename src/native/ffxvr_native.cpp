// SPDX-License-Identifier: MIT
//
// FFX VR - native helper.
//
// Stage one is diagnostics. The game uploads its view and projection matrices to
// the GPU through D3D11 constant buffers; identifying which buffer and which
// 64-byte slot holds them gives us the camera without needing any of
// PhyreEngine's internals.
//
// We patch ID3D11DeviceContext vtable entries on the context the GAME renders
// with. An earlier attempt created its own D3D11 device and patched that
// context's vtable, on the assumption that d3d11.dll shares one vtable across
// devices. It does not: every counter stayed at zero. The managed side now reads
// the real device and context out of ImGui's D3D11 backend data -- Fahrenheit
// initialised that backend with the game's own objects -- and passes the context
// here, so we patch exactly the object in use.
//
// Still instrumented: call counts per hooked entry point, written to a heartbeat
// log beside the DLL every few seconds. If Draw fires but Map does not, the game
// uploads constant buffers by another path, which UpdateSubresource covers.
//
// F9 writes a full dump of every constant buffer seen.

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include "ffxvr_xr.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

// Defined with the eye-2 twin code near the end; used from several places.
static ID3D11Texture2D* texture_of(ID3D11View* view);   // twins, below

#pragma comment(lib, "d3d11.lib")

namespace {

// ID3D11DeviceContext vtable indices.
//   0-2 IUnknown, 3-6 ID3D11DeviceChild, 7+ ID3D11DeviceContext
enum : int {
    VT_VSSETCONSTANTBUFFERS = 7,
    VT_DRAWINDEXED          = 12,
    VT_DRAW                 = 13,
    VT_MAP                  = 14,
    VT_UNMAP                = 15,
    VT_DRAWINDEXEDINST      = 20,
    VT_DRAWINST             = 21,
    VT_UPDATESUBRESOURCE    = 48,
};

enum CounterId {
    C_VSSETCB = 0, C_DRAWINDEXED, C_DRAW, C_DRAWIDXINST, C_DRAWINST,
    C_MAP, C_UNMAP, C_UPDATESUB, C_CREATEBUF, C_CB_CAPTURED, C_COUNT
};

const char* const kCounterName[C_COUNT] = {
    "VSSetConstantBuffers", "DrawIndexed", "Draw", "DrawIndexedInstanced",
    "DrawInstanced", "Map", "Unmap", "UpdateSubresource",
    "CreateBuffer (const, w/ data)", "constant buffer captures"
};

volatile LONG g_counters[C_COUNT] = {};

inline void bump(CounterId id) { InterlockedIncrement(&g_counters[id]); }

// ---------------------------------------------------------------- originals

using PFN_VSSetConstantBuffers = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
using PFN_DrawIndexed          = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using PFN_Draw                 = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using PFN_DrawIndexedInstanced = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using PFN_DrawInstanced        = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using PFN_Map                  = HRESULT (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using PFN_Unmap                = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using PFN_UpdateSubresource    = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
using PFN_CreateBuffer         = HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);

PFN_VSSetConstantBuffers g_orig_vssetcb        = nullptr;
PFN_DrawIndexed          g_orig_drawindexed    = nullptr;
PFN_Draw                 g_orig_draw           = nullptr;
PFN_DrawIndexedInstanced g_orig_drawidxinst    = nullptr;
PFN_DrawInstanced        g_orig_drawinst       = nullptr;
PFN_Map                  g_orig_map            = nullptr;
PFN_Unmap                g_orig_unmap          = nullptr;
PFN_UpdateSubresource    g_orig_updatesub      = nullptr;
PFN_CreateBuffer         g_orig_createbuffer   = nullptr;

// ---------------------------------------------------------------- state

struct CbRecord {
    UINT                 size    = 0;
    uint64_t             updates = 0;
    const char*          via     = "?";
    std::vector<uint8_t> data;
};

std::mutex                                      g_mutex;
std::map<ID3D11Buffer*, CbRecord>               g_buffers;
std::map<void*, std::pair<ID3D11Buffer*, UINT>> g_in_flight;

// A full-frame capture. Keeping only the latest value per buffer was useless:
// the game recycles a handful of constant buffers for every draw, so by the time
// F9 fires during the overlay render, the camera matrices written early in the
// frame are long gone. Instead F9 arms a capture that records every write in
// sequence until the buffer fills.
struct Write {
    uint32_t             seq;
    ID3D11Buffer*        buf;
    UINT                 size;
    const char*          via;
    std::vector<uint8_t> data;
};

// One frame of a busy scene is a few thousand writes. The old limit of 1200 was
// swallowed whole by the overlay's own draw calls before the game's 3D pass came
// round again, which is why early captures held nothing but UI matrices.
constexpr size_t kCaptureLimit = 20000;

std::vector<Write> g_capture;
bool               g_capturing     = false;
bool               g_capture_done  = false;
bool               g_arm_next      = false;   // F9 pressed; start at the next frame
uint32_t           g_seq           = 0;
uint32_t           g_frames        = 0;
int                g_frames_left   = 0;       // frames remaining in this capture
constexpr int      kCaptureFrames  = 10;
uint32_t           g_frame_writes  = 0;       // writes seen in the frame just finished

// Which buffers actually get bound to the vertex shader during a captured
// frame. The game binds far more often than it writes, so this census shows
// whether the bound buffers are ones we ever saw filled.
std::map<ID3D11Buffer*, int> g_bound;

// Live camera readback. RenderDoc located the camera in vertex shader constant
// block 0 of the large per-draw buffers: view matrix at byte 64, projection at
// byte 256, inverse view at byte 320. Once a frame we copy whatever is bound to
// VS slot 0 into a staging buffer and read those out.
constexpr UINT CAM_VIEW_OFFSET = 64;
constexpr UINT CAM_PROJ_OFFSET = 256;
constexpr UINT CAM_INV_OFFSET  = 320;
constexpr UINT CAM_MIN_SIZE    = CAM_INV_OFFSET + 64;

ID3D11Device*        g_device       = nullptr;
ID3D11DeviceContext* g_ctx          = nullptr;
ID3D11Buffer*        g_slot0_cur    = nullptr;   // actually bound to VS slot 0 right now
ID3D11Buffer*        g_vs_cb[14]    = {};        // every VS constant buffer slot, as bound
uint32_t             g_slot_binds[14] = {};      // how often each slot is bound
bool                 g_slot0_big    = false;     // ...and it is camera-sized
ID3D11Buffer*        g_slot0        = nullptr;   // latched at the frame's first 3D draw
int                  g_slot0_seen   = 0;         // distinct camera-sized slot-0 binds, this frame
int                  g_slot0_report = 0;         // the above, for the frame the UI is drawing
bool                 g_latched      = false;

// Vtable audit: what we put in each patched slot, so the heartbeat can report
// whether it is still there and, if not, whose code replaced it.
void**  g_vt = nullptr;
struct Patched { void** vt; int index; const char* name; void* ours; void* original; };
Patched g_patched[12] = {};
int     g_patched_n   = 0;
ID3D11Buffer*        g_staging      = nullptr;
UINT                 g_staging_size = 0;

float    g_cam[48] = {};     // view, projection, inverse view
float    g_cam_last_view[12] = {};
uint32_t g_cam_changes = 0;  // frames in which the view matrix differed from the previous one

// Whole-buffer readback of the latched slot-0 buffer, and a diff against the
// previous frame, so we can see which bytes move while walking even if none of
// them is a view matrix we recognise.
std::vector<uint8_t> g_full, g_full_prev;
int      g_diff_count   = 0;         // floats that changed since last frame
uint32_t g_diff_offs[8] = {};        // first few changed byte offsets
int      g_draws_latched_at = -1;    // which draw of the frame latched (0 = first)
int      g_draw_in_frame    = 0;
bool     g_cam_valid = false;
uint32_t g_cam_reads = 0;

bool    g_installed   = false;
void*   g_patched_context = nullptr;
int     g_dump_index  = 0;
bool    g_f9_was_down = false;
DWORD   g_last_beat   = 0;
wchar_t g_dir[MAX_PATH] = {};

// ---------------------------------------------------------------- helpers

bool as_constant_buffer(ID3D11Resource* res, ID3D11Buffer** out_buf, UINT* out_size) {
    if (res == nullptr) return false;

    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return false;

    ID3D11Buffer* buf = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buf))) || buf == nullptr)
        return false;

    D3D11_BUFFER_DESC desc = {};
    buf->GetDesc(&desc);
    buf->Release();   // identity key only

    if ((desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) == 0) return false;

    *out_buf  = buf;
    *out_size = desc.ByteWidth;
    return true;
}

// ---------------------------------------------------------------------------
// Live camera scanner
//
// The latched slot-0 readback assumes the big per-draw buffers carry the live
// camera. If they turn out to be filled once per area instead, the real camera
// must arrive through one of the handful of buffers the game writes each frame.
// So every CPU write is also scanned directly for a view matrix: a 3x4 block
// whose first three columns are orthonormal (a rotation) and whose last column
// is a non-trivial translation. That shape is specific enough that false hits
// are rare, and finding it needs no prior knowledge of offsets.
//
// Candidates are collected per frame and reported with the camera position each
// implies, so watching which entry moves while walking identifies the live one.
// ---------------------------------------------------------------------------

struct ScanHit {
    uint32_t offset;
    uint32_t size;
    int      via;        // 0 CreateBuffer, 1 Map/Unmap, 2 UpdateSubresource
    float    pos[3];
};

constexpr int kScanMax = 6;

// All of these are touched only under g_mutex.
ScanHit g_scan_live[kScanMax]   = {};
int     g_scan_live_n           = 0;
ScanHit g_scan_report[kScanMax] = {};
int     g_scan_report_n         = 0;

inline bool near_one(float v)  { return v > 0.985f && v < 1.015f; }
inline bool near_zero(float v) { return v > -0.015f && v < 0.015f; }

/// Tests a 3x4 row-major block for the shape of a world-to-view matrix and,
/// if it matches, returns the camera's world position (-R^T * t).
static bool looks_like_view(const float* m, float out_pos[3]) {
    for (int i = 0; i < 12; ++i) {
        if (!std::isfinite(m[i])) return false;
    }

    // Columns 0..2 form the rotation; column 3 is the translation.
    const float r[3][3] = { { m[0], m[1], m[2]  },
                            { m[4], m[5], m[6]  },
                            { m[8], m[9], m[10] } };
    const float t[3]    = { m[3], m[7], m[11] };

    for (int i = 0; i < 3; ++i) {
        if (!near_one(r[i][0]*r[i][0] + r[i][1]*r[i][1] + r[i][2]*r[i][2])) return false;
    }
    for (int i = 0; i < 3; ++i) {
        const int j = (i + 1) % 3;
        if (!near_zero(r[i][0]*r[j][0] + r[i][1]*r[j][1] + r[i][2]*r[j][2])) return false;
    }

    const float mag2 = t[0]*t[0] + t[1]*t[1] + t[2]*t[2];
    if (mag2 < 1e-4f || mag2 > 1e12f) return false;   // identity, or nonsense

    for (int i = 0; i < 3; ++i) {                     // -R^T * t
        out_pos[i] = -(r[0][i]*t[0] + r[1][i]*t[1] + r[2][i]*t[2]);
    }
    return true;
}

/// Scans one freshly written constant buffer for view-matrix-shaped blocks.
/// Caller holds g_mutex.
static void scan_for_camera(const void* data, UINT size, const char* via) {
    if (data == nullptr || size < 48) return;

    // A hit from CreateBuffer was filled once, at area load; one from
    // Map/Unmap or UpdateSubresource was written this frame. Telling them
    // apart is the whole point of the scan.
    const int via_code = (via != nullptr && via[0] == 'C') ? 0
                       : (via != nullptr && via[0] == 'M') ? 1
                       : (via != nullptr && via[0] == 'R') ? 3 : 2;

    const char* base = static_cast<const char*>(data);

    // Constant buffer registers are 16-byte aligned, so a matrix never starts
    // off that grid.
    for (UINT off = 0; off + 48 <= size; off += 16) {
        float m[12];
        memcpy(m, base + off, sizeof(m));

        float pos[3];
        if (!looks_like_view(m, pos)) continue;

        bool seen = false;
        for (int i = 0; i < g_scan_live_n; ++i) {
            if (g_scan_live[i].offset == off && g_scan_live[i].size == size) {
                g_scan_live[i].via = via_code;
                memcpy(g_scan_live[i].pos, pos, sizeof(pos));   // refresh
                seen = true;
                break;
            }
        }
        if (seen) continue;

        if (g_scan_live_n < kScanMax) {
            ScanHit& h = g_scan_live[g_scan_live_n++];
            h.offset   = off;
            h.size     = size;
            h.via      = via_code;
            memcpy(h.pos, pos, sizeof(pos));
        }
    }
}

// ---------------------------------------------------------------------------
// Camera override
//
// Every per-object constant buffer (the ~1 KB ones and the ~7 KB skinned ones
// alike) carries the camera three ways: a position float3 at +48, the view
// matrix at +64 and the inverse view at +320. UpdateSubresource hands us those
// bytes before upload, so rewriting all three in a private copy moves the
// camera for everything drawn with that buffer -- no shader work needed.
//
// The override is expressed relative to the game's camera: a yaw/pitch
// rotation about the camera's own axes, then a translation along the (new)
// camera's right/up/forward. "Freeze" swaps the live base for the one captured
// when freezing was turned on, so the authored camera motion stops.
// ---------------------------------------------------------------------------

constexpr UINT CAM_POS_OFFSET = 48;

bool  g_ovr_enabled = false;
bool  g_ovr_freeze  = false;
float g_ovr_yaw = 0.f, g_ovr_pitch = 0.f;          // degrees
float g_ovr_move[3] = { 0.f, 0.f, 0.f };           // right, up, forward (world units)
float g_ovr_base[12] = {};                         // frozen view (3x4), valid when g_ovr_have_base
bool  g_ovr_have_base = false;
uint32_t g_ovr_applied = 0;
unsigned g_ovr_mask    = 0;     // union of blocks ever patched: 1 view 2 inv 4 pos 8 VP 16 VW 32 PVW 64 IVW

// Per buffer size: how many writes were patched and which blocks validated
// in all of them (AND) versus any of them (OR). A size whose AND lacks PVW
// while its OR has VP is an object left behind by a camera move.
struct OvrStat { uint32_t count; unsigned mask_all; unsigned mask_any; };
std::map<UINT, OvrStat> g_ovr_stats;

std::vector<uint8_t> g_scratch;

// ---------------------------------------------------------------------------
// Second eye
//
// The scene pass is one contiguous run of draws into a depth-bound target.
// For each such draw, after the game's own, the same object buffer is
// re-uploaded with the other eye's camera, the render target switched to our
// eye-2 textures, the draw issued again, and everything restored. The
// detours are re-entered during that, so a guard makes them pass-through.
// ---------------------------------------------------------------------------
bool  g_stereo       = false;
int   g_stereo_scope = 0;             // 0 all draws, 1 camera-buffer draws only, 2 camera + product-only
bool  g_overlay_phase = false;        // from our per-frame hook (inside Present) to the game's first clear; used to find the backbuffer
ID3D11Buffer* g_imgui_cb = nullptr;   // ImGui's constant buffer: the one bound at the first draw after our hook
bool  g_first_draw_pending = false;   // set by the hook; cleared by that first draw
float g_ipd          = 6.0f;          // lateral offset between eyes, world units (chars ~20 tall)
float g_eye_shift    = 0.f;           // applied by build_override_view along the camera's right axis
bool  g_in_eye2      = false;         // re-entrancy guard
// Recent camera-carrying uploads, with both eyes' images, keyed by buffer.
// The game uploads the next object's buffer before drawing the current one,
// so the image for a draw is usually not the most recent upload.
struct EyeImage { ID3D11Buffer* cb; UINT size; std::vector<uint8_t> eye1, eye2; uint32_t frame; int kind; };
EyeImage g_eye_ring[32];
int      g_eye_ring_next = 0;
EyeImage* g_eye2_cur = nullptr;       // the entry in use for the current eye-2 draw
EyeImage* g_eye2_slots[14] = {};      // entries uploaded for each VS slot in the current eye-2 draw
uint32_t g_eye2_draws = 0, g_eye2_draws_report = 0;
// why a draw did not get an eye-2 pass (cumulative)
uint32_t g_e2_no_stereo = 0, g_e2_no_image = 0, g_e2_cb_mismatch = 0, g_e2_no_dsv = 0, g_e2_small = 0, g_e2_ok = 0, g_e2_begin_fail = 0;
// Twins: for every texture the game renders into, an identical texture of
// ours. In eye-2 mode a draw's targets are swapped for their twins, and any
// sampled texture that has a twin is swapped too, so post-processing,
// reflections and the final composite all happen for eye 2 as well.
struct Twin {
    ID3D11Texture2D* game; ID3D11Texture2D* tex;
    ID3D11RenderTargetView* rtv; ID3D11DepthStencilView* dsv;
    std::map<ID3D11ShaderResourceView*, ID3D11ShaderResourceView*> srvs;   // game SRV -> twin SRV
    UINT w, h; DXGI_FORMAT fmt;
};
std::map<ID3D11Texture2D*, Twin> g_twins;
ID3D11Texture2D* g_backbuffer = nullptr;    // the swapchain texture, whose twin is eye 2's final image
bool g_backbuffer_seen_this_frame = false;
ID3D11ShaderResourceView* g_cur_ps_srv[16] = {};   // PS shader resources, tracked from the detour
uint32_t g_twin_count = 0;
// backbuffer diagnostics
uint32_t g_bb_draws_seen = 0, g_bb_draws_overlay = 0, g_bb_begin = 0, g_bb_twin_null = 0, g_bb_copies = 0;
uint32_t g_e2_uploads = 0, g_e2_identical = 0, g_e2_reentered_updates = 0;
// which twin received the most eye-2 draws this frame: the scene target
ID3D11Texture2D* g_scene_tex = nullptr; uint32_t g_scene_draws = 0;
ID3D11Texture2D* g_scene_live_tex = nullptr; uint32_t g_scene_live_draws = 0;
std::map<ID3D11Texture2D*, uint32_t> g_draws_per_target;
// snapshot of the scene target's twin taken when the scene pass ends (the
// target gets cleared and reused by post-processing later in the frame)
ID3D11Texture2D* g_snap_tex = nullptr; ID3D11ShaderResourceView* g_snap_srv = nullptr;
ID3D11Texture2D* g_snap_from = nullptr;         // which game texture it mirrors
ID3D11Texture2D* g_snap_game = nullptr;         // the game's own scene texture at the same moment
ID3D11Texture2D* g_prev_e2_target = nullptr; uint32_t g_prev_e2_run = 0;
ID3D11ShaderResourceView* g_snap_game_srv = nullptr;   // eye 1's copy, for the preview
uint32_t g_resolves = 0, g_resolves_mirrored = 0, g_omrt_uav_calls = 0;
uint32_t g_snap_by = 0;                               // what triggered the last snapshot: 1 SRV bind, 2 copy/resolve, 3 clear
uint32_t g_snap_frame = 0;                            // frame the snapshot was taken in
bool g_debug_red = false;              // experiment: clear a depth-bound twin to red on its first eye-2 draw of the frame
bool g_debug_leave = false;            // experiment: leave the eye-2 image in the buffer after the eye-2 draw (main view should shift)
uint32_t g_red_clears = 0;
uint32_t g_draw_ctx_same = 0, g_draw_ctx_other = 0; void* g_draw_ctx_last_other = nullptr;
std::map<ID3D11Texture2D*, uint32_t> g_red_done_frame;
// current output-merger state, tracked from the OMSetRenderTargets detour
ID3D11RenderTargetView* g_cur_rtv[4] = {};
UINT                    g_cur_rtv_n  = 0;
ID3D11DepthStencilView* g_cur_dsv    = nullptr;

// World positions of skinned objects this frame (translation column of the
// world matrix at +640 in the 1872 B+ family), for comparing against the
// game's own actor positions.
float g_world_live[32][3];   int g_world_live_n = 0;
float g_world_report[32][3]; int g_world_report_n = 0;
static void census_write(UINT size);   // defined with the census code below

// Memory-scan results (see ffxvr_memscan below).
struct MemHit { uintptr_t addr; const char* kind; };
uintptr_t g_scan_struct = 0;   // heap camera object found by the scan (position at +0, view at +12)
std::vector<MemHit> g_scan_hits;   // every hit of the last scan
std::vector<uintptr_t> g_scan_cams; // camera-state array entries: position at +0, column-major view at +0x10

// Write-watch state (see ffxvr_watch below; kept for completeness, the stage-0
// debugger swallows the exceptions so the static xref scan is what we use).
struct Writer { uintptr_t eip; uint32_t count; };
Writer   g_writers[32] = {};
int      g_writers_n   = 0;
uint32_t g_watch_hits  = 0;
uintptr_t g_watch_addr[2] = {};
DWORD    g_main_tid    = 0;
void*    g_veh         = nullptr;

static void mul3(const float a[9], const float b[9], float out[9]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[r*3+c] = a[r*3+0]*b[0*3+c] + a[r*3+1]*b[1*3+c] + a[r*3+2]*b[2*3+c];
}

constexpr UINT CAM_PROJN_OFFSET = 128;   // projection, near-plane variant
constexpr UINT CAM_VP_OFFSET    = 192;   // proj(+128) * view        -- what the vertex shader uses
constexpr UINT CAM_W_OFFSET     = 480;   // world (per object)
constexpr UINT CAM_VW_OFFSET    = 544;   // view * world
constexpr UINT CAM_WI_OFFSET    = 608;   // inverse world
constexpr UINT CAM_PVW_OFFSET   = 672;   // proj * view * world
constexpr UINT CAM_IVW_OFFSET   = 736;   // inverse(view * world) = inv world * inv view

static void mul4(const float* a, const float* b, float* out) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[r*4+c] = a[r*4+0]*b[0*4+c] + a[r*4+1]*b[1*4+c] + a[r*4+2]*b[2*4+c] + a[r*4+3]*b[3*4+c];
}

/// Relative comparison, loose enough for float products of large translations.
static bool same4(const float* a, const float* b) {
    float scale = 1.f;
    for (int i = 0; i < 16; ++i) scale = (std::max)(scale, fabsf(b[i]));
    for (int i = 0; i < 16; ++i) if (fabsf(a[i] - b[i]) > 0.01f * scale) return false;
    return true;
}

/// Builds the override view V' (4x4) and its inverse from a base 3x4 view.
// Head tracking (OpenXR). The located left-eye pose, relative to the
// recentre reference, is applied on top of the manual override: rotation
// in view space (the game's view axes match OpenXR's: x right, y up, -z
// forward), translation along the base camera's axes scaled by the world
// scale (game units per metre). Eye 2 adds the right-eye offset.
bool  g_xr_want     = false;       // the user asked for the headset
bool  g_xr_active   = false;       // poses valid this frame
bool  g_xr_rotate   = true, g_xr_translate = true;
float g_xr_scale    = 94.f;        // units per metre; 94 gives a 6-unit eye separation
float g_xr_q[4]     = { 0, 0, 0, 1 };   // left eye orientation, relative to the reference
float g_xr_p[3]     = {};               // left eye position (m), relative to the reference
float g_xr_eye_off[3] = {};             // right eye minus left eye (m), in reference axes
float g_xr_ref_q[4] = { 0, 0, 0, 1 };   // yaw-only reference orientation
float g_xr_ref_p[3] = {};
bool  g_xr_recenter_pending = true;     // first valid pose becomes the reference
bool  g_xr_use_fov  = true;             // replace the game's projection with the headset's per-eye field of view
bool  g_xr_seat     = true;             // head owns orientation; the authored camera contributes position and heading only
XrFovTan g_xr_fov[2] = { { -1.f, 1.f, 1.f, -1.f }, { -1.f, 1.f, 1.f, -1.f } };   // from the runtime
XrFovTan g_xr_fov_used[2] = {};         // what this frame is rendered with (submitted with the layers)
bool  g_xr_fov_applied = false;
float g_xr_raw_q[4] = { 0, 0, 0, 1 }, g_xr_raw_p[3] = {};

static void quat_to_mat(const float q[4], float M[9]) {   // rows: world axes of the rotated frame? No: M maps body -> reference
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    M[0] = 1 - 2*(y*y + z*z); M[1] = 2*(x*y - w*z);     M[2] = 2*(x*z + w*y);
    M[3] = 2*(x*y + w*z);     M[4] = 1 - 2*(x*x + z*z); M[5] = 2*(y*z - w*x);
    M[6] = 2*(x*z - w*y);     M[7] = 2*(y*z + w*x);     M[8] = 1 - 2*(x*x + y*y);
}
static void quat_mul(const float a[4], const float b[4], float o[4]) {   // o = a * b
    o[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    o[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    o[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    o[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
}
static void quat_rotate(const float q[4], const float v[3], float o[3]) {
    float M[9]; quat_to_mat(q, M);
    for (int i = 0; i < 3; ++i) o[i] = M[i*3+0]*v[0] + M[i*3+1]*v[1] + M[i*3+2]*v[2];
}

/// Takes the located poses for the frame about to render and derives the
/// relative pose the override uses.
static void xr_apply_poses(const XrFrameOut& f) {
    if (!f.have_views) { g_xr_active = false; return; }
    memcpy(g_xr_raw_q, f.view[0].q, sizeof(g_xr_raw_q)); memcpy(g_xr_raw_p, f.view[0].p, sizeof(g_xr_raw_p));
    if (g_xr_recenter_pending) {
        // Reference = the head's yaw and position now, so "forward" is where the user looks.
        const float fwd[3] = { 0, 0, -1 }; float d[3]; quat_rotate(g_xr_raw_q, fwd, d);
        const float a = atan2f(-d[0], -d[2]);
        g_xr_ref_q[0] = 0; g_xr_ref_q[1] = sinf(a * 0.5f); g_xr_ref_q[2] = 0; g_xr_ref_q[3] = cosf(a * 0.5f);
        memcpy(g_xr_ref_p, g_xr_raw_p, sizeof(g_xr_ref_p));
        g_xr_recenter_pending = false;
    }
    const float ref_inv[4] = { -g_xr_ref_q[0], -g_xr_ref_q[1], -g_xr_ref_q[2], g_xr_ref_q[3] };
    quat_mul(ref_inv, g_xr_raw_q, g_xr_q);
    const float dp[3] = { g_xr_raw_p[0] - g_xr_ref_p[0], g_xr_raw_p[1] - g_xr_ref_p[1], g_xr_raw_p[2] - g_xr_ref_p[2] };
    quat_rotate(ref_inv, dp, g_xr_p);
    const float de[3] = { f.view[1].p[0] - f.view[0].p[0], f.view[1].p[1] - f.view[0].p[1], f.view[1].p[2] - f.view[0].p[2] };
    quat_rotate(ref_inv, de, g_xr_eye_off);
    g_xr_active = true;
    g_xr_fov[0] = f.fov[0]; g_xr_fov[1] = f.fov[1];
}

static void build_override_view(const float v[12], float Vn[16], float In[16], float Pn[3]) {
    float R[9] = { v[0], v[1], v[2],  v[4], v[5], v[6],  v[8], v[9], v[10] };
    const float t[3] = { v[3], v[7], v[11] };
    float P[3];
    for (int i = 0; i < 3; ++i) P[i] = -(R[0*3+i]*t[0] + R[1*3+i]*t[1] + R[2*3+i]*t[2]);

    // Rotate in view space: R' = Rx(pitch) * Ry(yaw) * R.
    const float yaw = g_ovr_yaw * 0.0174533f, pit = g_ovr_pitch * 0.0174533f;
    const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pit), sp = sinf(pit);
    const float Ry[9] = {  cy, 0.f,  sy,   0.f, 1.f, 0.f,   -sy, 0.f,  cy };
    const float Rx[9] = { 1.f, 0.f, 0.f,   0.f,  cp, -sp,   0.f,  sp,  cp };
    float tmp[9], Rb[9], Rn[9];
    mul3(Ry, R, tmp);
    mul3(Rx, tmp, Rb);                                  // base camera: the game's, turned by the manual override

    // Head rotation, applied in the base camera's frame: the new axes are the
    // head's axes expressed in base-camera coordinates, i.e. Rn = Rq^T * Rb.
    const bool eye2 = (g_eye_shift != 0.f);
    const bool xr = g_xr_active && g_ovr_enabled;
    if (xr && g_xr_rotate) {
        float Mq[9], MqT[9]; quat_to_mat(g_xr_q, Mq);
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) MqT[r*3+c] = Mq[c*3+r];
        if (g_xr_seat) {
            // Seat: the authored camera's pitch and roll are discarded; its
            // heading becomes the reference frame the head rotates in. World
            // up is +y in render space.
            float f[3] = { -Rb[6], -Rb[7], -Rb[8] };            // authored forward
            f[1] = 0.f;
            float len = sqrtf(f[0]*f[0] + f[2]*f[2]);
            if (len < 1e-4f) { f[0] = 0.f; f[2] = -1.f; len = 1.f; }
            f[0] /= len; f[2] /= len;
            const float up[3] = { 0.f, 1.f, 0.f };
            const float right[3] = { f[1]*up[2] - f[2]*up[1], f[2]*up[0] - f[0]*up[2], f[0]*up[1] - f[1]*up[0] };   // f x up
            const float Rh[9] = { right[0], right[1], right[2],  up[0], up[1], up[2],  -f[0], -f[1], -f[2] };
            memcpy(Rb, Rh, sizeof(Rh));                          // position offsets follow the heading frame too
        }
        mul3(MqT, Rb, Rn);
    }
    else memcpy(Rn, Rb, sizeof(Rn));

    // Move along the new camera's axes (rows of R'; forward is -row 2). The
    // eye shift is a further lateral offset, used for the second eye. With
    // the headset, the head position and the right-eye offset are reference-
    // space vectors: along the base camera's axes (x right, y up, z back).
    const float lateral = xr ? 0.f : g_eye_shift;
    float hx = 0.f, hy = 0.f, hz = 0.f;
    if (xr) {
        if (g_xr_translate) { hx = g_xr_p[0]; hy = g_xr_p[1]; hz = g_xr_p[2]; }
        if (eye2) { hx += g_xr_eye_off[0]; hy += g_xr_eye_off[1]; hz += g_xr_eye_off[2]; }
        hx *= g_xr_scale; hy *= g_xr_scale; hz *= g_xr_scale;
    }
    for (int i = 0; i < 3; ++i)
        Pn[i] = P[i] + Rn[0*3+i]*(g_ovr_move[0] + lateral) + Rn[1*3+i]*g_ovr_move[1] - Rn[2*3+i]*g_ovr_move[2]
                     + Rb[0*3+i]*hx + Rb[1*3+i]*hy + Rb[2*3+i]*hz;

    float tn[3];
    for (int r = 0; r < 3; ++r) tn[r] = -(Rn[r*3+0]*Pn[0] + Rn[r*3+1]*Pn[1] + Rn[r*3+2]*Pn[2]);

    for (int r = 0; r < 3; ++r) {
        Vn[r*4+0] = Rn[r*3+0]; Vn[r*4+1] = Rn[r*3+1]; Vn[r*4+2] = Rn[r*3+2]; Vn[r*4+3] = tn[r];
        In[r*4+0] = Rn[0*3+r]; In[r*4+1] = Rn[1*3+r]; In[r*4+2] = Rn[2*3+r]; In[r*4+3] = Pn[r];
    }
    Vn[12] = Vn[13] = Vn[14] = 0.f; Vn[15] = 1.f;
    In[12] = In[13] = In[14] = 0.f; In[15] = 1.f;
}

// Last known camera products from a fully-laid-out buffer this frame, so
// buffers that carry only the product (e.g. the 336-byte ones) can be patched
// by recognising it.
float g_last_PV[16]  = {};
float g_last_PVn[16] = {};
float g_last_PVn2[16] = {};   // the same product for eye 2
bool  g_have_PV      = false;

// Game-memory camera write (defined below, used by write_camera).
int       g_cam_mem_mode = 0;          // 0 off, 1 at first upload, 2 at present, 3 both
uintptr_t g_cam_mem_obj  = 0;          // heap camera object (position at +0)
uint32_t  g_cam_mem_writes = 0;
static void write_game_camera();

/// Rewrites every camera-derived block in a buffer image. `orig` is the
/// untouched original, `p` the copy being patched, `base` the byte offset of
/// the view matrix (64 in the common layout, 48 in the shifted one), `v` the
/// 3x4 view to base the override on. Each derived block is replaced only if
/// the original equals the product it is expected to be. Returns a bitmask.
static unsigned write_camera(const uint8_t* orig, uint8_t* p, UINT size, UINT base, const float v[12]) {
    float Vn[16], In[16], Pn[3];
    build_override_view(v, Vn, In, Pn);

    // Layout relative to the view block.
    const UINT o_pos = base - 16, o_view = base, o_projn = base + 64, o_vp = base + 128,
               o_inv = base + 256, o_w = base + 416;

    const float* V  = reinterpret_cast<const float*>(orig + o_view);
    const float* Pj = reinterpret_cast<const float*>(orig + o_projn);
    const UINT o_proj = base + 192;                     // the main projection (+256 in the common layout)
    unsigned mask = 0;

    memcpy(p + o_view, Vn, 64);  mask |= 1;
    memcpy(p + o_pos,  Pn, 12);  mask |= 4;
    if (size >= o_inv + 64) { memcpy(p + o_inv, In, 64);  mask |= 2; }

    // Headset field of view: both projection variants get the eye's
    // asymmetric frustum in x and y; their depth rows are left alone.
    // Column-vector convention (w = -z): ndc_x = P[0]*t - P[2] for a
    // direction with tangent t, so P[0] = 2/(r-l), P[2] = (r+l)/(r-l).
    float Pjn[16]; memcpy(Pjn, Pj, sizeof(Pjn));
    const bool fov = g_xr_active && g_ovr_enabled && g_xr_use_fov;
    const XrFovTan& f = g_xr_fov_used[(g_eye_shift != 0.f) ? 1 : 0];
    const bool fov_ok = std::isfinite(f.l) && std::isfinite(f.r) && std::isfinite(f.u) && std::isfinite(f.d)
                     && f.r - f.l > 0.1f && f.u - f.d > 0.1f && f.r - f.l < 20.f && f.u - f.d < 20.f;
    if (fov && fov_ok) {
        static int logged = 0;
        if (logged < 2) {
            logged++;
            diag("fov apply (eye %d): tan l %.3f r %.3f u %.3f d %.3f; original near-variant proj row0 (%.4f %.4f %.4f %.4f) row1 (%.4f %.4f %.4f %.4f) row2 (%.4f %.4f %.4f %.4f) row3 (%.4f %.4f %.4f %.4f)",
                 (g_eye_shift != 0.f) ? 2 : 1, f.l, f.r, f.u, f.d, Pj[0], Pj[1], Pj[2], Pj[3], Pj[4], Pj[5], Pj[6], Pj[7], Pj[8], Pj[9], Pj[10], Pj[11], Pj[12], Pj[13], Pj[14], Pj[15]);
            if (size >= o_proj + 64) { const float* Pm = reinterpret_cast<const float*>(orig + o_proj);
                diag("    main proj row0 (%.4f %.4f %.4f %.4f) row1 (%.4f %.4f %.4f %.4f) row2 (%.4f %.4f %.4f %.4f) row3 (%.4f %.4f %.4f %.4f)",
                     Pm[0], Pm[1], Pm[2], Pm[3], Pm[4], Pm[5], Pm[6], Pm[7], Pm[8], Pm[9], Pm[10], Pm[11], Pm[12], Pm[13], Pm[14], Pm[15]); }
        }
        auto apply = [&](float* P) {
            const float sx = (P[0] < 0.f) ? -1.f : 1.f, sy = (P[5] < 0.f) ? -1.f : 1.f;
            P[0] = sx * 2.f / (f.r - f.l);  P[2] = sx * (f.r + f.l) / (f.r - f.l);
            P[5] = sy * 2.f / (f.u - f.d);  P[6] = sy * (f.u + f.d) / (f.u - f.d);
        };
        apply(Pjn);
        memcpy(p + o_projn, Pjn, 64);
        if (size >= o_proj + 64) { float Pm[16]; memcpy(Pm, orig + o_proj, 64); apply(Pm); memcpy(p + o_proj, Pm, 64); }
        g_xr_fov_applied = true;
    }

    float PV[16], PVn[16];
    mul4(Pj, V, PV);
    mul4(Pjn, Vn, PVn);
    if (size >= o_vp + 64 && same4(reinterpret_cast<const float*>(orig + o_vp), PV)) {
        memcpy(p + o_vp, PVn, 64);  mask |= 8;
        memcpy(g_last_PV,  PV,  sizeof(PV));
        memcpy(g_last_PVn, PVn, sizeof(PVn));
        if (!g_have_PV && g_ovr_enabled && (g_cam_mem_mode & 1)) write_game_camera();
        g_have_PV = true;
    }

    // World-derived blocks come in two placements: right after the camera
    // (+480.. in the ~1 KB family) or 160 bytes later (+640.. in everything
    // from 1872 B up, skinned characters included). Try both; each block is
    // only replaced if the original is the product it should be.
    for (UINT wb : { o_w, o_w + 160 }) {
        const UINT w_w = wb, w_vw = wb + 64, w_wi = wb + 128, w_pvw = wb + 192, w_ivw = wb + 256;
        if (size < w_ivw + 64) continue;

        const float* W  = reinterpret_cast<const float*>(orig + w_w);
        const float* Wi = reinterpret_cast<const float*>(orig + w_wi);
        const float* I  = reinterpret_cast<const float*>(orig + o_inv);
        float VW[16], PVW[16], IVW[16], tmp[16];
        unsigned got = 0;

        mul4(V, W, VW);
        if (same4(reinterpret_cast<const float*>(orig + w_vw), VW)) {
            mul4(Vn, W, tmp); memcpy(p + w_vw, tmp, 64);  got |= 16;
        }
        mul4(PV, W, PVW);
        if (same4(reinterpret_cast<const float*>(orig + w_pvw), PVW)) {
            mul4(PVn, W, tmp); memcpy(p + w_pvw, tmp, 64);  got |= 32;
        }
        mul4(Wi, I, IVW);
        if (same4(reinterpret_cast<const float*>(orig + w_ivw), IVW)) {
            mul4(Wi, In, tmp); memcpy(p + w_ivw, tmp, 64);  got |= 64;
        }
        mask |= got;
        if (got) break;      // found this buffer's placement
    }
    return mask;
}

/// For buffers with no view of their own: replace any block equal to this
/// frame's known proj*view with the override's -- and any block equal to
/// proj*view*X, where X is some other block in the same buffer (an object's
/// world matrix), with the override's proj*view*X. Returns the number replaced.
static int replace_known_products(uint8_t* p, UINT size, const float* PVn) {
    if (!g_have_PV) return 0;
    int n = 0;
    const UINT nblocks = size / 16;

    // Pass 1: bare proj*view.
    for (UINT off = 0; off + 64 <= size; off += 16) {
        if (same4(reinterpret_cast<const float*>(p + off), g_last_PV)) {
            memcpy(p + off, PVn, 64);
            n++;
        }
    }

    // Pass 2: proj*view*X for each affine-looking block X in the buffer.
    // Buffers of this kind are small, so the quadratic search is cheap.
    if (nblocks > 64) return n;
    for (UINT xo = 0; xo + 64 <= size; xo += 16) {
        const float* X = reinterpret_cast<const float*>(p + xo);
        if (X[12] != 0.f || X[13] != 0.f || X[14] != 0.f || fabsf(X[15] - 1.f) > 1e-4f) continue;   // not affine
        if (same4(X, g_last_PV)) continue;
        float PVX[16], PVnX[16];
        mul4(g_last_PV, X, PVX);
        bool used = false;
        for (UINT off = 0; off + 64 <= size; off += 16) {
            if (off == xo) continue;
            if (same4(reinterpret_cast<const float*>(p + off), PVX)) {
                if (!used) { mul4(PVn, X, PVnX); used = true; }
                memcpy(p + off, PVnX, 64);
                n++;
            }
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
// Writing the override camera into the game's own memory
//
// Rendering is already handled by patching uploads. The battle cursor and
// other CPU-projected UI read the camera from game memory instead: a static
// column-major matrix at FFX.exe+0x88FD3C and (found by the F10 scan) a heap
// object holding position / row-major / column-major copies. Writing the
// override camera there, at a chosen moment in the frame, tests whether the
// UI projection picks it up -- and when in the frame it runs.
// ---------------------------------------------------------------------------


static void write_game_camera() {
    if (!g_cam_valid) return;

    float v[12];
    memcpy(v, g_cam, sizeof(v));                 // the game's own view (recorded before override)
    if (g_ovr_freeze && g_ovr_have_base) memcpy(v, g_ovr_base, sizeof(v));

    float Vn[16], In[16], Pn[3];
    build_override_view(v, Vn, In, Pn);

    float col[16] = {};
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) col[c*4 + r] = Vn[r*4 + c];
    col[12] = Vn[3]; col[13] = Vn[7]; col[14] = Vn[11]; col[15] = 1.f;

    HMODULE exe = GetModuleHandleW(nullptr);
    if (exe != nullptr) {
        float* st = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(exe) + 0x88FD3C);
        memcpy(st, col, sizeof(col));
    }
    if (g_cam_mem_obj != 0) {
        uint8_t* o = reinterpret_cast<uint8_t*>(g_cam_mem_obj);
        memcpy(o + 0,    Pn, 12);                // position
        memcpy(o + 12,   Vn, 48);                // row-major 3x4
        memcpy(o + 0x60, col, 64);               // column-major 4x4
    }
    if (g_cam_mem_mode & 4) {                    // the camera-state array: position, column-major view
        for (uintptr_t a : g_scan_cams) {
            uint8_t* o = reinterpret_cast<uint8_t*>(a);
            memcpy(o,        Pn,  12);
            memcpy(o + 0x10, col, 60);
        }
    }
    g_cam_mem_writes++;
}

/// Dumps the game's static camera struct (FFX.exe+0x88FB78, 0x400 bytes) and
/// the heap camera object (-0x100..+0x300) as floats, so what they store
/// besides the matrix -- position, target, angles -- can be worked out.
extern "C" __declspec(dllexport) int ffxvr_dump_structs() {
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\ffxvr_structs.log", g_dir);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a") != 0 || f == nullptr) return -1;

    const float* c = g_cam;
    const float t0 = c[3], t1 = c[7], t2 = c[11];
    fprintf(f, "tick %lu  game camera pos %.3f %.3f %.3f  yaw %.1f\n", GetTickCount(),
            -(c[0]*t0 + c[4]*t1 + c[8]*t2), -(c[1]*t0 + c[5]*t1 + c[9]*t2), -(c[2]*t0 + c[6]*t1 + c[10]*t2),
            atan2f(c[8], c[10]) * 57.2958f);

    struct Range { const char* name; uintptr_t base; size_t len; } ranges[2];
    HMODULE exe = GetModuleHandleW(nullptr);
    ranges[0] = { "static struct FFX.exe+88FB78", exe ? reinterpret_cast<uintptr_t>(exe) + 0x88FB78 : 0, 0x400 };
    ranges[1] = { "heap object", g_scan_struct ? g_scan_struct - 0x100 : 0, 0x400 };

    for (const Range& r : ranges) {
        if (r.base == 0) continue;
        fprintf(f, "  == %s @ %08X ==\n", r.name, static_cast<unsigned>(r.base));
        const uint8_t* p = reinterpret_cast<const uint8_t*>(r.base);
        for (size_t off = 0; off < r.len; off += 16) {
            float v[4]; memcpy(v, p + off, 16);
            uint32_t u[4]; memcpy(u, p + off, 16);
            fprintf(f, "  +%03X:", static_cast<unsigned>(off));
            for (int i = 0; i < 4; ++i) {
                if (std::isfinite(v[i]) && fabsf(v[i]) > 1e-6f && fabsf(v[i]) < 1e7f) fprintf(f, " %12.4f", v[i]);
                else fprintf(f, "   0x%08X", u[i]);
            }
            fprintf(f, "\n");
        }
    }
    fprintf(f, "\n");
    fclose(f);
    return 0;
}

// ---------------------------------------------------------------------------
// Call-stack census of 2D UI uploads
//
// The 2D sprites (battle cursors among them) reach the GPU through small
// constant buffers written with UpdateSubresource. Capturing the call stack
// at those writes, for one frame, names the game functions that draw the UI;
// the one that projected the cursor is among them. No exceptions involved.
// ---------------------------------------------------------------------------

struct StackRec { uintptr_t frames[12]; int n; uint32_t count; UINT size; };
std::vector<StackRec> g_stacks;
bool     g_stacks_arm  = false;
bool     g_stacks_live = false;
UINT     g_stacks_min  = 0, g_stacks_max = 0;

/// True if `a` looks like a return address: inside the game's code and
/// immediately preceded by a call instruction (E8 rel32, FF 15 [mem],
/// FF 5x [reg+disp8], FF 9x [reg+disp32], FF Dx reg).
static bool looks_like_return(uintptr_t a, uintptr_t lo, uintptr_t hi) {
    if (a < lo + 8 || a >= hi) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(a);
    __try {
        if (p[-5] == 0xE8) return true;
        if (p[-6] == 0xFF && p[-5] == 0x15) return true;
        if (p[-3] == 0xFF && (p[-2] & 0xF8) == 0x50) return true;
        if (p[-6] == 0xFF && (p[-5] & 0xF8) == 0x90) return true;
        if (p[-2] == 0xFF && (p[-1] & 0xF8) == 0xD0) return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

static void census_write(UINT size) {
    if (!g_stacks_live || size < g_stacks_min || size > g_stacks_max) return;

    HMODULE exe = GetModuleHandleW(nullptr);
    const uintptr_t lo = reinterpret_cast<uintptr_t>(exe);
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const uint8_t*>(exe) + reinterpret_cast<const IMAGE_DOS_HEADER*>(exe)->e_lfanew);
    const uintptr_t hi = lo + nt->OptionalHeader.SizeOfImage;

    // Scan the raw stack upward from here for return addresses into game
    // code. Frame pointers are not relied on, so callers compiled without
    // them are found too. The stack base bounds the walk.
    const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const uintptr_t* sp  = reinterpret_cast<const uintptr_t*>(&tib);   // a local: current stack position
    const uintptr_t* top = reinterpret_cast<const uintptr_t*>(tib->StackBase);
    if (top - sp > 4096) top = sp + 4096;

    StackRec r = {};
    r.size = size; r.count = 1;
    for (const uintptr_t* w = sp; w < top && r.n < 12; ++w) {
        if (looks_like_return(*w, lo, hi)) {
            const uintptr_t off = *w - lo;
            if (r.n > 0 && r.frames[r.n - 1] == off) continue;
            r.frames[r.n++] = off;
        }
    }
    if (r.n == 0) return;
    for (StackRec& e : g_stacks) {
        if (e.n == r.n && e.size == r.size && memcmp(e.frames, r.frames, sizeof(uintptr_t) * r.n) == 0) { e.count++; return; }
    }
    if (g_stacks.size() < 400) g_stacks.push_back(r);
}

static void census_dump() {
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\ffxvr_stacks.log", g_dir);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a") != 0 || f == nullptr) return;
    fprintf(f, "call stacks of constant buffer writes (any path) to %u..%u byte buffers, one frame: %zu distinct\n",
            g_stacks_min, g_stacks_max, g_stacks.size());
    for (const StackRec& r : g_stacks) {
        fprintf(f, "  x%-4u %5uB :", r.count, r.size);
        for (int i = 0; i < r.n; ++i) fprintf(f, " +%06X", static_cast<unsigned>(r.frames[i]));
        fprintf(f, "\n");
    }
    fprintf(f, "\n");
    fclose(f);
}

/// Arms a one-frame census of writes to buffers in [min_size, max_size].
extern "C" __declspec(dllexport) void ffxvr_census_arm(unsigned min_size, unsigned max_size) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stacks.clear();
    g_stacks_min = min_size; g_stacks_max = max_size;
    g_stacks_arm = true;
}

// ---------------------------------------------------------------------------
// Support for the mod's own in-world markers
// ---------------------------------------------------------------------------

/// Copies `len` bytes from game memory under SEH. Returns 1 on success, 0 if
/// the read faulted -- stale battle pointers outside a battle, for instance.
extern "C" __declspec(dllexport) int ffxvr_safe_read(const void* src, void* dst, unsigned len) {
    __try { memcpy(dst, src, len); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/// Projects a world point through the camera the player currently sees --
/// the override view when enabled, else the game's -- and the game's main
/// projection. Returns 1 and NDC x,y in out[] if the point is in front.
extern "C" __declspec(dllexport) int ffxvr_project(float x, float y, float z, float* out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_cam_valid || out == nullptr) return 0;

    float v[12];
    memcpy(v, g_cam, sizeof(v));
    if (g_ovr_enabled && g_ovr_freeze && g_ovr_have_base) memcpy(v, g_ovr_base, sizeof(v));

    float V[16];
    if (g_ovr_enabled) {
        float In[16], Pn[3];
        build_override_view(v, V, In, Pn);
    }
    else {
        memcpy(V, v, 48);
        V[12] = V[13] = V[14] = 0.f; V[15] = 1.f;
    }
    const float* P = &g_cam[16];                    // main projection (+256)

    const float w[4] = { x, y, z, 1.f };
    float e[4], c[4];
    for (int r = 0; r < 4; ++r) e[r] = V[r*4+0]*w[0] + V[r*4+1]*w[1] + V[r*4+2]*w[2] + V[r*4+3]*w[3];
    for (int r = 0; r < 4; ++r) c[r] = P[r*4+0]*e[0] + P[r*4+1]*e[1] + P[r*4+2]*e[2] + P[r*4+3]*e[3];
    if (c[3] <= 1e-4f) return 0;
    out[0] = c[0] / c[3];
    out[1] = c[1] / c[3];
    return 1;
}

/// Distinct world positions of skinned objects drawn last frame; out holds
/// up to max*3 floats. Returns the count.
extern "C" __declspec(dllexport) int ffxvr_world_positions(float* out, int max) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const int n = (g_world_report_n < max) ? g_world_report_n : max;
    for (int i = 0; i < n; ++i) { out[i*3] = g_world_report[i][0]; out[i*3+1] = g_world_report[i][1]; out[i*3+2] = g_world_report[i][2]; }
    return n;
}

// ---------------------------------------------------------------------------
// Frame-structure census
//
// One frame's sequence of render-target switches, viewports, clears and the
// runs of draws between them, with the constant-buffer families the draws
// used. This is the map a second (per-eye) rendering of the scene needs.
// ---------------------------------------------------------------------------

struct FrameEv { char text[160]; };
std::vector<FrameEv> g_frame_ev;
bool  g_frame_arm = false, g_frame_live = false;
int   g_frame_draw_run = 0;        // draws since the last non-draw event
int   g_frame_run_small = 0, g_frame_run_mid = 0, g_frame_run_big = 0, g_frame_run_none = 0;

static void frame_flush_draws() {
    if (g_frame_draw_run == 0) return;
    FrameEv e;
    snprintf(e.text, sizeof(e.text), "      %4d draws   (slot0: none %d, <=512B %d, ~1KB %d, >=1872B %d)",
             g_frame_draw_run, g_frame_run_none, g_frame_run_small, g_frame_run_mid, g_frame_run_big);
    if (g_frame_ev.size() < 2000) g_frame_ev.push_back(e);
    g_frame_draw_run = g_frame_run_small = g_frame_run_mid = g_frame_run_big = g_frame_run_none = 0;
}

static void frame_event(const char* text) {
    if (!g_frame_live) return;
    frame_flush_draws();
    FrameEv e; strncpy_s(e.text, text, _TRUNCATE);
    if (g_frame_ev.size() < 2000) g_frame_ev.push_back(e);
}

static void describe_view(ID3D11View* view, char* out, size_t n) {
    if (view == nullptr) { snprintf(out, n, "null"); return; }
    ID3D11Resource* res = nullptr;
    view->GetResource(&res);
    if (res == nullptr) { snprintf(out, n, "%p", static_cast<void*>(view)); return; }
    D3D11_RESOURCE_DIMENSION dim; res->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) {
            D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
            snprintf(out, n, "%p tex %ux%u fmt %u%s%s", static_cast<void*>(res), d.Width, d.Height, static_cast<unsigned>(d.Format),
                     (d.BindFlags & D3D11_BIND_SHADER_RESOURCE) ? " +SRV" : "", d.SampleDesc.Count > 1 ? " MSAA" : "");
            tex->Release();
        }
        else snprintf(out, n, "%p tex2d", static_cast<void*>(res));
    }
    else snprintf(out, n, "%p dim %d", static_cast<void*>(res), static_cast<int>(dim));
    res->Release();
}

extern "C" __declspec(dllexport) void ffxvr_on_rendertargets(unsigned n, void* const* rtvs, void* dsv) {
    if (g_in_eye2) return;
    // The first target bound after our per-frame hook is the overlay's view of
    // the real backbuffer (Fahrenheit binds it to draw ImGui). Re-identified
    // every frame, so a swapchain resize is followed.
    if (g_overlay_phase && !g_backbuffer_seen_this_frame && n > 0 && rtvs && rtvs[0]) {
        ID3D11Texture2D* t = texture_of(static_cast<ID3D11View*>(rtvs[0]));
        if (t) { g_backbuffer = t; g_backbuffer_seen_this_frame = true; }
    }
    g_cur_rtv_n = n < 4 ? n : 4;
    for (UINT i = 0; i < 4; ++i) g_cur_rtv[i] = (i < g_cur_rtv_n && rtvs) ? static_cast<ID3D11RenderTargetView*>(rtvs[i]) : nullptr;
    g_cur_dsv = static_cast<ID3D11DepthStencilView*>(dsv);
    if (!g_frame_live) return;
    char a[96], b[96];
    describe_view(n > 0 && rtvs ? static_cast<ID3D11View*>(rtvs[0]) : nullptr, a, sizeof(a));
    describe_view(static_cast<ID3D11View*>(dsv), b, sizeof(b));
    char t[160];
    snprintf(t, sizeof(t), "RT  n=%u  rtv0 %s   dsv %s%s%s", n, a, b,
             (n > 0 && rtvs && texture_of(static_cast<ID3D11View*>(rtvs[0])) == g_backbuffer) ? "  [BACKBUFFER]" : "",
             g_overlay_phase ? "  [overlay phase]" : "");
    std::lock_guard<std::mutex> lock(g_mutex);
    frame_event(t);
    for (unsigned i = 1; i < n && i < 4 && rtvs; ++i) {           // the other targets of an MRT pass
        describe_view(static_cast<ID3D11View*>(rtvs[i]), a, sizeof(a));
        snprintf(t, sizeof(t), "      rtv%u %s", i, a);
        frame_event(t);
    }
}

extern "C" __declspec(dllexport) void ffxvr_on_viewport(float w, float h, float x, float y) {
    if (!g_frame_live) return;
    char t[160];
    snprintf(t, sizeof(t), "VP  %gx%g at %g,%g", w, h, x, y);
    std::lock_guard<std::mutex> lock(g_mutex);
    frame_event(t);
}

extern "C" __declspec(dllexport) void ffxvr_on_clear(void* view, int depth) {
    if (!g_in_eye2) g_overlay_phase = false;
    if (!g_frame_live) return;
    char a[96]; describe_view(static_cast<ID3D11View*>(view), a, sizeof(a));
    char t[160];
    snprintf(t, sizeof(t), "CLR %s %s", depth ? "depth" : "color", a);
    std::lock_guard<std::mutex> lock(g_mutex);
    frame_event(t);
}

extern "C" __declspec(dllexport) void ffxvr_frame_arm() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_frame_ev.clear();
    g_frame_arm = true;
}

static void frame_dump() {
    frame_flush_draws();
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\ffxvr_frame.log", g_dir);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a") != 0 || f == nullptr) return;
    fprintf(f, "frame structure, %zu events\n", g_frame_ev.size());
    for (const FrameEv& e : g_frame_ev) fprintf(f, "  %s\n", e.text);
    fprintf(f, "\n");
    fclose(f);
}

extern "C" __declspec(dllexport) void ffxvr_cam_mem_set(int mode, uintptr_t obj) {
    g_cam_mem_mode = mode;
    g_cam_mem_obj  = obj;
}
extern "C" __declspec(dllexport) int ffxvr_cam_mem_writes() { return static_cast<int>(g_cam_mem_writes); }

/// If the override is on and this write carries a camera, returns a patched
/// private copy of the bytes to upload instead; otherwise returns src.
static const void* maybe_override(const void* src, UINT size, ID3D11Buffer* buf) {
    if (g_in_eye2) return src;
    if (!g_ovr_enabled || size < 64) return src;

    const uint8_t* p = static_cast<const uint8_t*>(src);

    // Where is the view? Common layout has it at +64, the shifted one at +48.
    UINT base = 0;
    float v[12], pos[3];
    for (UINT cand : { 64u, 48u }) {
        const UINT o_inv = cand + 256;
        if (size < o_inv + 64) continue;        // no inverse to validate against: not this layout
        memcpy(v, p + cand, sizeof(v));
        if (!looks_like_view(v, pos)) continue;
        // The inverse view must agree, or this is a look-alike (a mirrored
        // camera for a reflection pass, say).
        const float* inv = reinterpret_cast<const float*>(p + o_inv);
        if (fabsf(inv[3] - pos[0]) > 1.f || fabsf(inv[7] - pos[1]) > 1.f || fabsf(inv[11] - pos[2]) > 1.f) continue;
        // And the projection must be perspective: the shadow pass carries the
        // light's view, also a rotation plus translation, with an orthographic
        // projection. Moving that would misalign every shadow.
        const float* proj = reinterpret_cast<const float*>(p + cand + 192);   // main projection (+256 in the common layout)
        if (!(proj[14] < -0.99f && proj[14] > -1.01f && fabsf(proj[15]) < 1e-4f)) continue;
        base = cand;
        break;
    }

    if (g_scratch.size() < size) g_scratch.resize(size);
    memcpy(g_scratch.data(), p, size);

    unsigned mask = 0;
    if (base != 0) {
        if (g_ovr_freeze) {
            if (!g_ovr_have_base) { memcpy(g_ovr_base, v, sizeof(v)); g_ovr_have_base = true; }
            memcpy(v, g_ovr_base, sizeof(v));
        }
        else {
            g_ovr_have_base = false;
        }
        mask = write_camera(p, g_scratch.data(), size, base, v);
        if (base == 48) mask |= 128;

        if (g_stereo && buf != nullptr) {               // the same buffer seen from the other eye
            EyeImage& e = g_eye_ring[g_eye_ring_next];
            g_eye_ring_next = (g_eye_ring_next + 1) % 32;
            e.cb = buf; e.size = size; e.frame = g_frames; e.kind = 1;
            e.eye1.assign(g_scratch.data(), g_scratch.data() + size);
            e.eye2.assign(p, p + size);

            // write_camera also publishes the frame's known product for the
            // product-only buffers; that must stay eye 1's, so save and restore.
            float savePV[16], savePVn[16]; const bool saveHave = g_have_PV;
            memcpy(savePV, g_last_PV, sizeof(savePV)); memcpy(savePVn, g_last_PVn, sizeof(savePVn));
            g_eye_shift = (g_ipd != 0.f) ? g_ipd : 1e-6f;   // non-zero marks "eye 2"; with the headset the real offset is used instead
            write_camera(p, e.eye2.data(), size, base, v);
            g_eye_shift = 0.f;
            if (mask & 8) memcpy(g_last_PVn2, g_last_PVn, sizeof(g_last_PVn2));   // eye 2's proj*view
            memcpy(g_last_PV, savePV, sizeof(savePV)); memcpy(g_last_PVn, savePVn, sizeof(savePVn)); g_have_PV = saveHave;
        }
    }
    else {
        if (replace_known_products(g_scratch.data(), size, g_last_PVn) == 0) return src;   // nothing of ours in here
        mask = 256;
        if (g_stereo && buf != nullptr) {
            EyeImage& e = g_eye_ring[g_eye_ring_next];
            g_eye_ring_next = (g_eye_ring_next + 1) % 32;
            e.cb = buf; e.size = size; e.frame = g_frames; e.kind = 2;
            e.eye1.assign(g_scratch.data(), g_scratch.data() + size);
            e.eye2.assign(p, p + size);
            replace_known_products(e.eye2.data(), size, g_last_PVn2);
        }
    }

    g_ovr_mask |= mask;
    g_ovr_applied++;
    auto it = g_ovr_stats.find(size);
    if (it == g_ovr_stats.end()) g_ovr_stats[size] = { 1, mask, mask };
    else { it->second.count++; it->second.mask_all &= mask; it->second.mask_any |= mask; }
    return g_scratch.data();
}

// Camera taken straight from the CPU-side write of a per-object buffer.
// No latch, no GPU readback: UpdateSubresource hands us the bytes, and the
// shape test at +64 rejects same-sized buffers that carry something else.
static void camera_from_write(const void* src, UINT size) {
    if (size < CAM_MIN_SIZE || size > 2048) return;

    const uint8_t* p = static_cast<const uint8_t*>(src);
    float m[12], pos[3];
    memcpy(m, p + CAM_VIEW_OFFSET, sizeof(m));
    if (!looks_like_view(m, pos)) return;

    if (memcmp(g_cam_last_view, m, sizeof(m)) != 0) {
        memcpy(g_cam_last_view, m, sizeof(m));
        g_cam_changes++;
    }
    memcpy(&g_cam[0],  p + CAM_VIEW_OFFSET, 64);
    memcpy(&g_cam[16], p + CAM_PROJ_OFFSET, 64);
    memcpy(&g_cam[32], p + CAM_INV_OFFSET,  64);
    g_cam_valid = true;
    g_cam_reads++;
}

void record_buffer(ID3D11Buffer* buf, UINT size, const void* src, const char* via) {
    if (size == 0 || size > 65536 || src == nullptr) return;

    census_write(size);     // all write paths: Map/Unmap carries the 2D UI buffers

    camera_from_write(src, size);
    scan_for_camera(src, size, via);

    if (size >= 1872 && size < 16384) {
        const float* W = reinterpret_cast<const float*>(static_cast<const uint8_t*>(src) + 640);
        const float t[3] = { W[3], W[7], W[11] };
        if (std::isfinite(t[0]) && std::isfinite(t[1]) && std::isfinite(t[2]) && W[15] == 1.f) {
            bool seen = false;
            for (int i = 0; i < g_world_live_n; ++i) {
                if (fabsf(g_world_live[i][0]-t[0]) < 0.5f && fabsf(g_world_live[i][1]-t[1]) < 0.5f && fabsf(g_world_live[i][2]-t[2]) < 0.5f) { seen = true; break; }
            }
            if (!seen && g_world_live_n < 32) { memcpy(g_world_live[g_world_live_n++], t, sizeof(t)); }
        }
    }

    CbRecord& rec = g_buffers[buf];
    rec.size    = size;
    rec.via     = via;
    rec.updates += 1;
    rec.data.resize(size);
    memcpy(rec.data.data(), src, size);
    bump(C_CB_CAPTURED);

    if (g_capturing && g_capture.size() < kCaptureLimit) {
        Write w;
        w.seq  = ++g_seq;
        w.buf  = buf;
        w.size = size;
        w.via  = via;
        w.data.assign(static_cast<const uint8_t*>(src), static_cast<const uint8_t*>(src) + size);
        g_capture.push_back(std::move(w));

        // Dumping here would deadlock: we already hold the lock. Flag it and let
        // tick() write the file once the lock is released.
        if (g_capture.size() >= kCaptureLimit) {   // safety valve only
            g_capturing    = false;
            g_capture_done = true;
        }
    }
}

// Does this 64-byte block look like a projection matrix? Perspective projections
// put +/-1 in m[2][3] and 0 in m[3][3]; that pattern is rare in other data.
bool looks_like_projection(const float* m) {
    return (fabsf(fabsf(m[11]) - 1.0f) < 0.001f) && (fabsf(m[15]) < 0.001f);
}

// A view or world matrix: affine, so the last column is 0,0,0,1.
bool looks_like_affine(const float* m) {
    return fabsf(m[3]) < 1e-6f && fabsf(m[7]) < 1e-6f
        && fabsf(m[11]) < 1e-6f && fabsf(m[15] - 1.0f) < 1e-6f;
}

// Heartbeat: proves whether the hooks fire at all, without needing a keypress.
static void describe_address(void* p, char* out, size_t n) {
    HMODULE mod = nullptr;
    if (p != nullptr && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                         | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           static_cast<LPCWSTR>(p), &mod) && mod != nullptr) {
        wchar_t path[MAX_PATH] = L"";
        GetModuleFileNameW(mod, path, MAX_PATH);
        const wchar_t* base = wcsrchr(path, L'\\');
        base = base ? base + 1 : path;
        snprintf(out, n, "%p  %ls+0x%X", p, base,
                 static_cast<unsigned>(reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(mod)));
    }
    else {
        snprintf(out, n, "%p  (no module)", p);
    }
}

void heartbeat(bool force) {
    const DWORD now = GetTickCount();
    if (!force && (now - g_last_beat) < 1000) return;
    g_last_beat = now;

    {   // one appended line per beat: the camera as a time series
        wchar_t tpath[MAX_PATH];
        _snwprintf_s(tpath, _TRUNCATE, L"%s\\ffxvr_trace.log", g_dir);
        FILE* t = nullptr;
        if (_wfopen_s(&t, tpath, L"a") == 0 && t != nullptr) {
            const float* c = g_cam;
            const float t0 = c[3], t1 = c[7], t2 = c[11];
            fprintf(t, "t=%7.1fs  updates=%lu  cam_writes=%u  changes=%u  yaw=%7.1f pitch=%6.1f  pos=%8.2f %8.2f %8.2f  inv=%8.2f %8.2f %8.2f\n",
                    now / 1000.0, g_counters[C_UPDATESUB], g_cam_reads, g_cam_changes,
                    atan2f(c[8], c[10]) * 57.2958f,
                    asinf(-c[9] < -1.f ? -1.f : (-c[9] > 1.f ? 1.f : -c[9])) * 57.2958f,
                    -(c[0]*t0 + c[4]*t1 + c[8]*t2), -(c[1]*t0 + c[5]*t1 + c[9]*t2), -(c[2]*t0 + c[6]*t1 + c[10]*t2),
                    c[32+3], c[32+7], c[32+11]);
            fclose(t);
        }
    }

    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\ffxvr_native.log", g_dir);

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || f == nullptr) return;

    fprintf(f, "FFX VR native helper - hook activity\n");
    fprintf(f, "hooks installed: %s\n", g_installed ? "yes" : "no");
    fprintf(f, "patched context: %p\n\n", g_patched_context);

    for (int i = 0; i < C_COUNT; ++i) {
        fprintf(f, "  %-26s %ld\n", kCounterName[i], g_counters[i]);
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        fprintf(f, "\n  distinct constant buffers:  %zu\n", g_buffers.size());
        for (const auto& [buf, rec] : g_buffers) {
            fprintf(f, "      %p  %5u bytes  %8llu updates  via %s\n",
                    static_cast<void*>(buf), rec.size,
                    static_cast<unsigned long long>(rec.updates), rec.via);
        }
    }

    if (g_patched_n > 0) {
        fprintf(f, "\n  vtable audit (slot: what is there now / what we installed / original):\n");
        for (int i = 0; i < g_patched_n; ++i) {
            const Patched& pt = g_patched[i];
            void* now = IsBadReadPtr(&pt.vt[pt.index], sizeof(void*)) ? nullptr : pt.vt[pt.index];
            char a[160], b[160];
            describe_address(now,         a, sizeof(a));
            describe_address(pt.original, b, sizeof(b));
            fprintf(f, "    [%2d] %-22s %s  <- %s\n      original: %s\n",
                    pt.index, pt.name, a, (now == pt.ours) ? "OURS" : "REPLACED", b);
        }
    }

    fprintf(f, "\n  latched slot-0 buffer: %zu bytes, latched at draw #%d, reads %u\n",
            g_full.size(), g_draws_latched_at, g_cam_reads);
    fprintf(f, "  view matrix changed in %u frames so far\n", g_cam_changes);
    fprintf(f, "  changed since previous frame: %d floats", g_diff_count);
    if (g_diff_count > 0) {
        fprintf(f, " at bytes");
        for (int i = 0; i < g_diff_count && i < 8; ++i) fprintf(f, " %u", g_diff_offs[i]);
    }
    fprintf(f, "\n");
    if (g_cam_valid) {
        const float* c = g_cam;
        const float t0 = c[3], t1 = c[7], t2 = c[11];
        fprintf(f, "  view yaw %.1f deg, pitch %.1f deg\n",
                atan2f(c[8], c[10]) * 57.2958f, asinf(-c[9] < -1.f ? -1.f : (-c[9] > 1.f ? 1.f : -c[9])) * 57.2958f);
        fprintf(f, "  byte 64 as view -> camera at %.2f %.2f %.2f;  byte 320 translation %.2f %.2f %.2f\n",
                -(c[0]*t0 + c[4]*t1 + c[8]*t2), -(c[1]*t0 + c[5]*t1 + c[9]*t2), -(c[2]*t0 + c[6]*t1 + c[10]*t2),
                c[32+3], c[32+7], c[32+11]);
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        static const char* const via_name[4] = { "CreateBuffer", "Map", "UpdateSubresource", "Readback" };
        fprintf(f, "  scanner hits (last frame with any): %d\n", g_scan_report_n);
        for (int i = 0; i < g_scan_report_n; ++i) {
            const ScanHit& h = g_scan_report[i];
            fprintf(f, "      +%-5u in %6u B  %9.2f %9.2f %9.2f  via %s\n",
                    h.offset, h.size, h.pos[0], h.pos[1], h.pos[2],
                    via_name[(h.via >= 0 && h.via < 4) ? h.via : 2]);
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_ovr_stats.empty()) {
            fprintf(f, "\n  camera override, by buffer size (blocks patched in ALL writes / in ANY write):\n");
            static const char* const names[9] = { "view", "inv", "pos", "VP", "VW", "PVW", "IVW", "shifted", "product-only" };
            for (const auto& [size, st] : g_ovr_stats) {
                fprintf(f, "    %6u B  %8u writes   all:", size, st.count);
                for (int i = 0; i < 9; ++i) if (st.mask_all & (1u << i)) fprintf(f, " %s", names[i]);
                fprintf(f, "   any:");
                for (int i = 0; i < 9; ++i) if (st.mask_any & (1u << i)) fprintf(f, " %s", names[i]);
                fprintf(f, "\n");
            }
        }
    }

    if (g_watch_addr[0] || g_watch_addr[1]) {
        fprintf(f, "\n  write watch on %08X / %08X: %u hits\n",
                static_cast<unsigned>(g_watch_addr[0]), static_cast<unsigned>(g_watch_addr[1]), g_watch_hits);
        for (int i = 0; i < g_writers_n; ++i) {
            char where[160]; describe_address(reinterpret_cast<void*>(g_writers[i].eip), where, sizeof(where));
            fprintf(f, "    writer eip %s  x%u\n", where, g_writers[i].count);
        }
    }

    fprintf(f, "\n  eye 2: stereo=%d twins=%u backbuffer=%p draws last frame=%u\n", g_stereo ? 1 : 0,
            g_twin_count, static_cast<void*>(g_backbuffer), g_eye2_draws_report);
    fprintf(f, "         skipped: no stereo %u, no image %u, cb mismatch %u, no depth target %u, small cb %u; ok %u; begin failed %u\n",
            g_e2_no_stereo, g_e2_no_image, g_e2_cb_mismatch, g_e2_no_dsv, g_e2_small, g_e2_ok, g_e2_begin_fail);
    fprintf(f, "         backbuffer: draws seen %u (of which in overlay phase %u), eye2 begins %u (twin null %u), copies into it %u, overlay phase now %d\n",
            g_bb_draws_seen, g_bb_draws_overlay, g_bb_begin, g_bb_twin_null, g_bb_copies, g_overlay_phase ? 1 : 0);
    fprintf(f, "\n  dumps written: %d\n", g_dump_index);
    fprintf(f, "\nIf every counter is zero, the vtable patch did not reach the\n");
    fprintf(f, "game's device context and the hooking approach must change.\n");
    fclose(f);
}

void dump_capture() {
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\ffxvr_cbdump_%02d.txt", g_dir, g_dump_index++);

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || f == nullptr) return;

    std::lock_guard<std::mutex> lock(g_mutex);

    fprintf(f, "FFX VR constant buffer capture #%d\n", g_dump_index - 1);
    fprintf(f, "writes recorded: %zu (limit %zu) across %d frames\n\n", g_capture.size(), kCaptureLimit, kCaptureFrames);
    fprintf(f, "Buffers BOUND to the vertex shader during this frame: %zu\n", g_bound.size());
    for (const auto& [buf, n] : g_bound) {
        auto it = g_buffers.find(buf);
        fprintf(f, "   %p  bound %4d x   %s\n", static_cast<void*>(buf), n,
                it == g_buffers.end() ? "NEVER SEEN FILLED"
                                      : "contents known");
    }
    fprintf(f, "\n");
    fprintf(f, "Every constant buffer write, in order. Blocks flagged PROJ look like\n");
    fprintf(f, "a projection matrix; AFFINE like a view or world matrix. Compare two\n");
    fprintf(f, "captures from different camera positions.\n");

    for (const Write& w : g_capture) {
        const size_t n  = w.data.size() / sizeof(float);
        const float* fp = reinterpret_cast<const float*>(w.data.data());

        fprintf(f, "\n--- seq %u   buffer %p   %u bytes   via %s ---\n",
                w.seq, static_cast<void*>(w.buf), w.size, w.via);

        // Print every float, whatever the size. Hiding small writes was a
        // mistake: it concealed two thirds of what the game actually sends.
        if (n < 16) {
            for (size_t i = 0; i < n; i += 4) {
                fprintf(f, "     ");
                for (size_t j = 0; j < 4 && (i + j) < n; ++j) fprintf(f, "%12.5f ", fp[i + j]);
                fprintf(f, "\n");
            }
            continue;
        }

        for (size_t blk = 0; blk * 16 + 16 <= n; ++blk) {
            const float* m = fp + blk * 16;
            const char* tag = looks_like_projection(m) ? "  [PROJ]"
                            : looks_like_affine(m)     ? "  [AFFINE]" : "";
            fprintf(f, "  block %zu (byte %zu)%s\n", blk, blk * 64, tag);
            for (int r = 0; r < 4; ++r) {
                fprintf(f, "     %12.5f %12.5f %12.5f %12.5f\n",
                        m[r*4+0], m[r*4+1], m[r*4+2], m[r*4+3]);
            }
        }
    }
    fclose(f);
}

void tick() {
    const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;

    if (down && !g_f9_was_down) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_arm_next = true;   // begin at the next frame boundary, not mid-overlay
    }
    g_f9_was_down = down;

    if (g_capture_done) {
        g_capture_done = false;
        dump_capture();
    }

    heartbeat(false);
}

// ---------------------------------------------------------------- hooks

void STDMETHODCALLTYPE hk_VSSetConstantBuffers(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b) {
    bump(C_VSSETCB);

    if (b != nullptr && s == 0 && n > 0 && b[0] != nullptr) {
        D3D11_BUFFER_DESC bd = {};
        b[0]->GetDesc(&bd);
        bool big = (bd.ByteWidth >= CAM_MIN_SIZE);
        if (big && g_slot0_cur != b[0]) g_slot0_seen++;
        g_slot0_cur = b[0];
        g_slot0_big = big;
    }
    if (b != nullptr) {
        for (UINT i = 0; i < n && s + i < 14; ++i) { g_vs_cb[s + i] = b[i]; if (b[i]) g_slot_binds[s + i]++; }
    }

    if (g_capturing && b != nullptr) {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (UINT i = 0; i < n && i < 8; ++i) {
            if (b[i] != nullptr) g_bound[b[i]] += 1;
        }
    }

    g_orig_vssetcb(c, s, n, b);
}

// The game creates constant buffers already populated, which is why so little
// shows up through Map/Unmap. This catches the contents at creation time.
HRESULT STDMETHODCALLTYPE hk_CreateBuffer(ID3D11Device* dev, const D3D11_BUFFER_DESC* desc,
                                          const D3D11_SUBRESOURCE_DATA* init, ID3D11Buffer** out) {
    const HRESULT hr = g_orig_createbuffer(dev, desc, init, out);

    if (SUCCEEDED(hr) && desc != nullptr && init != nullptr && init->pSysMem != nullptr
        && (desc->BindFlags & D3D11_BIND_CONSTANT_BUFFER) && out != nullptr && *out != nullptr) {
        bump(C_CREATEBUF);
        std::lock_guard<std::mutex> lock(g_mutex);
        record_buffer(*out, desc->ByteWidth, init->pSysMem, "CreateBuffer");
    }
    return hr;
}

void STDMETHODCALLTYPE hk_DrawIndexed(ID3D11DeviceContext* c, UINT n, UINT i, INT v) {
    bump(C_DRAWINDEXED);
    tick();
    g_orig_drawindexed(c, n, i, v);
}

void STDMETHODCALLTYPE hk_Draw(ID3D11DeviceContext* c, UINT n, UINT s) {
    bump(C_DRAW);
    tick();
    g_orig_draw(c, n, s);
}

// The game draws through the instanced variants only. Latching the slot-0
// binding at the FIRST such draw of a frame targets the buffer feeding real
// geometry, rather than whichever happened to be bound last.
void STDMETHODCALLTYPE hk_DrawIndexedInstanced(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, INT e, UINT f) {
    bump(C_DRAWIDXINST);
    tick();
    g_orig_drawidxinst(c, a, b, d, e, f);
}

void STDMETHODCALLTYPE hk_DrawInstanced(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, UINT e) {
    bump(C_DRAWINST);
    tick();
    g_orig_drawinst(c, a, b, d, e);
}

HRESULT STDMETHODCALLTYPE hk_Map(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP t, UINT fl, D3D11_MAPPED_SUBRESOURCE* m) {
    bump(C_MAP);
    const HRESULT hr = g_orig_map(c, r, sub, t, fl, m);
    if (FAILED(hr) || m == nullptr || m->pData == nullptr) return hr;

    ID3D11Buffer* buf = nullptr; UINT size = 0;
    if (as_constant_buffer(r, &buf, &size)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_in_flight[m->pData] = { buf, size };
    }
    return hr;
}

void STDMETHODCALLTYPE hk_Unmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) {
    bump(C_UNMAP);

    ID3D11Buffer* buf = nullptr; UINT size = 0;
    if (as_constant_buffer(r, &buf, &size)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto it = g_in_flight.begin(); it != g_in_flight.end(); ++it) {
            if (it->second.first != buf) continue;
            record_buffer(buf, size, it->first, "Map/Unmap");
            g_in_flight.erase(it);
            break;
        }
    }

    g_orig_unmap(c, r, sub);
    tick();
}

void STDMETHODCALLTYPE hk_UpdateSubresource(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub,
                                            const D3D11_BOX* box, const void* src, UINT rp, UINT dp) {
    bump(C_UPDATESUB);

    ID3D11Buffer* buf = nullptr; UINT size = 0;
    if (box == nullptr && as_constant_buffer(r, &buf, &size)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        record_buffer(buf, size, src, "UpdateSubresource");
    }

    g_orig_updatesub(c, r, sub, box, src, rp, dp);
    tick();
}

// ---------------------------------------------------------------- install

// Copy the bound slot-0 buffer somewhere the CPU can read, and pull out the
// camera. Uses the original Map/Unmap so our own hooks neither recurse nor
// record the staging buffer as game data.
void read_camera() {
    if (g_slot0 == nullptr || g_device == nullptr || g_ctx == nullptr) return;
    if (g_orig_map == nullptr || g_orig_unmap == nullptr) return;

    D3D11_BUFFER_DESC src = {};
    g_slot0->GetDesc(&src);
    if (src.ByteWidth < 48) return;

    if (g_staging == nullptr || g_staging_size < src.ByteWidth) {
        if (g_staging) { g_staging->Release(); g_staging = nullptr; }
        D3D11_BUFFER_DESC sd = {};
        sd.ByteWidth      = src.ByteWidth;
        sd.Usage          = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(g_device->CreateBuffer(&sd, nullptr, &g_staging))) { g_staging = nullptr; return; }
        g_staging_size = src.ByteWidth;
    }

    g_ctx->CopyResource(g_staging, g_slot0);

    D3D11_MAPPED_SUBRESOURCE ms = {};
    if (FAILED(g_orig_map(g_ctx, g_staging, 0, D3D11_MAP_READ, 0, &ms)) || ms.pData == nullptr) return;

    const uint8_t* p = static_cast<const uint8_t*>(ms.pData);

    g_full_prev.swap(g_full);
    g_full.assign(p, p + src.ByteWidth);


    g_orig_unmap(g_ctx, g_staging, 0);

    // Diff against last frame: same buffer, same size only.
    g_diff_count = 0;
    if (g_full_prev.size() == g_full.size()) {
        const float* a = reinterpret_cast<const float*>(g_full.data());
        const float* b = reinterpret_cast<const float*>(g_full_prev.data());
        const size_t n = g_full.size() / 4;
        for (size_t i = 0; i < n; ++i) {
            if (a[i] != b[i]) {
                if (g_diff_count < 8) g_diff_offs[g_diff_count] = static_cast<uint32_t>(i * 4);
                g_diff_count++;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        scan_for_camera(g_full.data(), static_cast<UINT>(g_full.size()), "Readback");
    }
}

bool patch(void** vtable, int index, void* replacement, void** out_original, const char* name = "?") {
    DWORD old = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old)) return false;
    *out_original = vtable[index];
    vtable[index] = replacement;
    VirtualProtect(&vtable[index], sizeof(void*), old, &old);

    if (g_patched_n < 12) g_patched[g_patched_n++] = { vtable, index, name, replacement, *out_original };
    return true;
}

} // namespace


// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) int ffxvr_init(void* game_device, void* game_context) {
    if (g_installed) return 0;
    if (game_context == nullptr) return -1;

    // The caller hands us the context the game actually renders with, read out
    // of ImGui's D3D11 backend data. Patching this object's own vtable removes
    // the guesswork that sank the previous attempt: a vtable obtained from a
    // separately created device turned out to share nothing with the game's.
    ID3D11DeviceContext* context = reinterpret_cast<ID3D11DeviceContext*>(game_context);
    g_ctx    = context;
    g_device = reinterpret_cast<ID3D11Device*>(game_device);

    void** vt = *reinterpret_cast<void***>(context);
    if (vt == nullptr || IsBadReadPtr(vt, sizeof(void*) * 64)) return -3;
    g_vt = vt;

    bool ok = true;
    ok &= patch(vt, VT_VSSETCONSTANTBUFFERS, reinterpret_cast<void*>(&hk_VSSetConstantBuffers), reinterpret_cast<void**>(&g_orig_vssetcb), "VSSetConstantBuffers");
    ok &= patch(vt, VT_MAP,                  reinterpret_cast<void*>(&hk_Map),                  reinterpret_cast<void**>(&g_orig_map), "Map");
    ok &= patch(vt, VT_UNMAP,                reinterpret_cast<void*>(&hk_Unmap),                reinterpret_cast<void**>(&g_orig_unmap), "Unmap");

    // Patch CreateBuffer on the device: ID3D11Device vtable index 3.
    if (game_device != nullptr) {
        ID3D11Device* dev = reinterpret_cast<ID3D11Device*>(game_device);
        void** dvt = *reinterpret_cast<void***>(dev);
        if (dvt != nullptr && !IsBadReadPtr(dvt, sizeof(void*) * 8)) {
            patch(dvt, 3, reinterpret_cast<void*>(&hk_CreateBuffer),
                  reinterpret_cast<void**>(&g_orig_createbuffer), "CreateBuffer (device)");
        }
    }

    if (!ok) return -2;

    g_installed       = true;
    g_patched_context = game_context;
    g_main_tid        = GetCurrentThreadId();

    {   // start this run's trace fresh
        wchar_t tpath[MAX_PATH];
        _snwprintf_s(tpath, _TRUNCATE, L"%s\\ffxvr_trace.log", g_dir);
        FILE* t = nullptr;
        if (_wfopen_s(&t, tpath, L"w") == 0 && t != nullptr) {
            fprintf(t, "camera trace: one line per second, from UpdateSubresource writes of per-object buffers\n");
            fclose(t);
        }
    }
    heartbeat(true);
    return 0;
}

// Called once per frame from the managed module's render_imgui, which Fahrenheit
// invokes inside its Present hook. That gives us a true frame boundary, so a
// capture can span exactly one frame instead of whatever happened to come next.
static void xr_tick();
extern "C" __declspec(dllexport) void ffxvr_begin_frame() {
    bool dump_now = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_frames++;

        if (g_capturing) {
            if (--g_frames_left <= 0) {      // enough frames collected
                g_capturing    = false;
                dump_now       = true;
                g_frame_writes = static_cast<uint32_t>(g_capture.size());
            }
        }
        else if (g_arm_next) {
            g_arm_next    = false;
            g_capture.clear();
            g_bound.clear();
            g_seq         = 0;
            g_frames_left = kCaptureFrames;
            g_capturing   = true;
        }
    }
    read_camera();
    xr_tick();                      // submit the frame just finished, locate the head for the next one
    g_overlay_phase = true;
    g_first_draw_pending = true;
    g_backbuffer_seen_this_frame = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_frame_live)  { g_frame_live = false; frame_event("-- Present --"); frame_dump(); }
        if (g_frame_arm)   { g_frame_arm = false; g_frame_live = true; frame_event("-- frame start (after Present) --"); }
        if (g_stacks_live) { g_stacks_live = false; census_dump(); }
        if (g_stacks_arm)  { g_stacks_arm = false; g_stacks_live = true; }
    }
    if (g_ovr_enabled && (g_cam_mem_mode & 2)) { std::lock_guard<std::mutex> lock(g_mutex); write_game_camera(); }
    g_latched      = false;
    g_draw_in_frame = 0;
    g_have_PV       = false;
    g_eye2_draws_report = g_eye2_draws; g_eye2_draws = 0;
    g_scene_tex = g_scene_live_tex; g_scene_draws = g_scene_live_draws; g_scene_live_tex = nullptr; g_scene_live_draws = 0; g_draws_per_target.clear();
    g_slot0_report = g_slot0_seen;   // snapshot before the reset, so the UI sees it
    g_slot0_seen   = 0;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_world_live_n > 0) {
            memcpy(g_world_report, g_world_live, sizeof(g_world_report));
            g_world_report_n = g_world_live_n;
        }
        g_world_live_n = 0;
        if (g_scan_live_n > 0) {     // keep the last frame that had any hits
            memcpy(g_scan_report, g_scan_live, sizeof(g_scan_report));
            g_scan_report_n = g_scan_live_n;
        }
        g_scan_live_n = 0;
    }

    if (dump_now) dump_capture();
}

// Fills 48 floats: view, projection, inverse view. Returns 1 if valid.
extern "C" __declspec(dllexport) int ffxvr_get_camera(float* out) {
    if (!g_cam_valid || out == nullptr) return 0;
    memcpy(out, g_cam, sizeof(g_cam));
    return 1;
}

extern "C" __declspec(dllexport) int ffxvr_camera_reads() { return static_cast<int>(g_cam_reads); }
extern "C" __declspec(dllexport) int ffxvr_camera_changes() { return static_cast<int>(g_cam_changes); }

// How many distinct camera-sized buffers were bound to slot 0 last frame.
// If this is large the choice of buffer is ambiguous and worth reporting.
extern "C" __declspec(dllexport) int ffxvr_slot0_count() { return g_slot0_report; }

/// Called from the managed inline detours on every draw. `which` is the
/// counter index (1 DrawIndexed, 2 Draw, 3 DrawIndexedInstanced, 4 DrawInstanced).
/// On the first draw of a frame with a camera-sized buffer in VS slot 0, that
/// buffer is latched for the end-of-frame readback.
extern "C" __declspec(dllexport) void ffxvr_draw_ctx(void* ctx) {
    if (g_in_eye2) return;
    if (ctx == static_cast<void*>(g_ctx)) g_draw_ctx_same++; else { g_draw_ctx_other++; g_draw_ctx_last_other = ctx; }
}

extern "C" __declspec(dllexport) int ffxvr_on_draw(int which) {
    if (g_in_eye2) return 0;
    if (which >= 1 && which <= 4) bump(static_cast<CounterId>(which));
    const int idx = g_draw_in_frame++;

    // Scene-pass draw with the buffer we hold an eye-2 image for?
    g_eye2_cur = nullptr;
    if (g_stereo && g_slot0_cur != nullptr) {
        // Most recent first: the game reuses a buffer for several objects in
        // a frame, and the draw belongs to the latest upload.
        for (int k = 0; k < 32; ++k) {
            EyeImage& e = g_eye_ring[(g_eye_ring_next - 1 - k + 64) % 32];
            if (e.cb == g_slot0_cur && e.frame == g_frames) { g_eye2_cur = &e; break; }
        }
    }
    bool eye2 = g_stereo && (g_cur_rtv_n > 0 || g_cur_dsv != nullptr);
    if (g_stereo && g_backbuffer && g_cur_rtv_n > 0 && texture_of(g_cur_rtv[0]) == g_backbuffer) {
        g_bb_draws_seen++;
        if (g_overlay_phase) g_bb_draws_overlay++;
    }
    if (g_first_draw_pending) { g_imgui_cb = g_slot0_cur; g_first_draw_pending = false; }   // (kept for the census)
    if (g_overlay_phase) eye2 = false;      // between our hook and the game's first clear only the overlay draws
    if (g_stereo_scope == 1 && (g_eye2_cur == nullptr || g_eye2_cur->kind != 1)) eye2 = false;
    if (g_stereo_scope == 2 &&  g_eye2_cur == nullptr) eye2 = false;
    if (!g_stereo) g_e2_no_stereo++;
    else if (g_eye2_cur == nullptr) g_e2_no_image++;   // UI and the like: drawn identically for eye 2
    else g_e2_ok++;

    if (g_frame_live) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_frame_draw_run++;
        if (!g_slot0_cur) g_frame_run_none++;
        else {
            D3D11_BUFFER_DESC bd = {}; g_slot0_cur->GetDesc(&bd);
            if (bd.ByteWidth <= 512) g_frame_run_small++;
            else if (bd.ByteWidth < 1872) g_frame_run_mid++;
            else g_frame_run_big++;
        }
    }
    if (g_latched || g_ctx == nullptr) return eye2 ? 1 : 0;

    ID3D11Buffer* b = nullptr;
    g_ctx->VSGetConstantBuffers(0, 1, &b);          // AddRefs
    if (b == nullptr) return eye2 ? 1 : 0;

    D3D11_BUFFER_DESC bd = {};
    b->GetDesc(&bd);
    // The camera lives in the ~1 KB per-object buffers (view at +64, inverse at
    // +320). The ~7 KB ones are skinned characters with a different layout.
    if (bd.ByteWidth < CAM_MIN_SIZE || bd.ByteWidth > 2048) { b->Release(); return eye2 ? 1 : 0; }

    if (g_slot0 != nullptr) g_slot0->Release();
    g_slot0             = b;                        // keep the reference
    g_latched           = true;
    g_draws_latched_at  = idx;
    return eye2 ? 1 : 0;
}

/// out[0] = floats changed since last frame, out[1] = buffer size, out[2] = draw
/// index that latched, out[3..10] = first changed byte offsets.
/// Called from the managed inline detour on UpdateSubresource, which the
/// runtime would not let us vtable-patch. Records (and scans) the write when
/// the target is a constant buffer being replaced whole.
extern "C" __declspec(dllexport) const void* ffxvr_on_updatesub(void* resource, const void* box, const void* src) {
    bump(C_UPDATESUB);
    if (g_in_eye2) { g_e2_reentered_updates++; return src; }
    if (resource == nullptr || src == nullptr || box != nullptr) return src;

    ID3D11Buffer* buf = nullptr; UINT size = 0;
    if (!as_constant_buffer(reinterpret_cast<ID3D11Resource*>(resource), &buf, &size)) return src;

    std::lock_guard<std::mutex> lock(g_mutex);
    record_buffer(buf, size, src, "UpdateSubresource");   // records the game's own camera
    return maybe_override(src, size, buf);                  // uploads ours
}

extern "C" __declspec(dllexport) void ffxvr_override_set(int enabled, int freeze, float yaw, float pitch,
                                                          float right, float up, float forward) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_ovr_enabled = enabled != 0;
    g_ovr_freeze  = freeze  != 0;
    g_ovr_yaw     = yaw;
    g_ovr_pitch   = pitch;
    g_ovr_move[0] = right; g_ovr_move[1] = up; g_ovr_move[2] = forward;
    if (!g_ovr_freeze) g_ovr_have_base = false;
}

// ---------------------------------------------------------------------------
// Memory scan: where does the game keep the camera?
//
// The view matrix we receive through UpdateSubresource is a copy of something
// the game owns. Finding the original lets us drive the camera at the source,
// so the CPU-side projection used by the battle cursor, damage numbers and the
// like follows -- and so does culling. We search every committed, writable,
// private region for the exact bits of the current view, in the row-major
// 3x4/4x4 layout we see uploaded and in a column-major 4x4 layout, plus the
// bare camera position. Results go to ffxvr_memscan.log.
// ---------------------------------------------------------------------------


/// Searches one region for float-aligned needles, reporting hits through the
/// callback. Runs under SEH so a page that faults despite VirtualQuery's word
/// (decommitted mid-scan, an odd section) skips the region instead of taking
/// the process down. No C++ objects live in here: __try forbids them.
typedef void (*HitFn)(void* ctx, uintptr_t addr, int pat);
static int search_region(const uint8_t* base, size_t len, const uint8_t* const* needles,
                         const size_t* nbytes, int npat, uintptr_t skip_lo, uintptr_t skip_hi,
                         HitFn on_hit, void* ctx) {
    __try {
        for (int pi = 0; pi < npat; ++pi) {
            const size_t nb = nbytes[pi];
            if (nb > len) continue;
            for (size_t off = 0; off + nb <= len; off += 4) {
                if (memcmp(base + off, needles[pi], nb) != 0) continue;
                const uintptr_t at = reinterpret_cast<uintptr_t>(base + off);
                if (at >= skip_lo && at <= skip_hi) continue;
                on_hit(ctx, at, pi);
            }
        }
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static bool region_ok(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD rw = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & rw) != 0;
}

static void describe_region(uintptr_t a, char* out, size_t n) {
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(a), &mod) && mod != nullptr) {
        wchar_t path[MAX_PATH] = L""; GetModuleFileNameW(mod, path, MAX_PATH);
        const wchar_t* base = wcsrchr(path, L'\\'); base = base ? base + 1 : path;
        snprintf(out, n, "%ls+0x%X", base, static_cast<unsigned>(a - reinterpret_cast<uintptr_t>(mod)));
    }
    else snprintf(out, n, "heap");
}

extern "C" __declspec(dllexport) int ffxvr_memscan() {
    float v[12];
    float pos[3];
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_cam_valid) return -1;
        memcpy(v, g_cam, sizeof(v));                        // game's view, rows 0..2 (3x4)
        pos[0] = g_cam[32+3]; pos[1] = g_cam[32+7]; pos[2] = g_cam[32+11];
    }

    // Patterns. Row-major 3x4 as uploaded; column-major 4x4 (translation in
    // floats 12..14); and the bare position.
    float colmajor[16] = {};
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) colmajor[c*4 + r] = v[r*4 + c];
    colmajor[12] = v[3]; colmajor[13] = v[7]; colmajor[14] = v[11]; colmajor[15] = 1.f;

    struct Pat { const char* kind; const float* data; size_t bytes; } pats[] = {
        { "view row-major 3x4",      v,        sizeof(v) },
        { "view column-major (t at 12..14)", colmajor, 15 * sizeof(float) },
        { "camera position float3", pos,      sizeof(pos) },
    };

    std::vector<MemHit> hits;
    int faulted = 0;
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t a = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    const uintptr_t self = reinterpret_cast<uintptr_t>(&g_cam[0]);

    const uint8_t* needles[3]; size_t nbytes[3];
    for (int i = 0; i < 3; ++i) { needles[i] = reinterpret_cast<const uint8_t*>(pats[i].data); nbytes[i] = pats[i].bytes; }

    struct Ctx { std::vector<MemHit>* hits; const Pat* pats; };
    Ctx ctx = { &hits, pats };
    HitFn on_hit = [](void* c, uintptr_t addr, int pi) {
        Ctx* cx = static_cast<Ctx*>(c);
        if (cx->hits->size() < 2000) cx->hits->push_back({ addr, cx->pats[pi].kind });
    };

    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi)) == 0) break;
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= a) break;
        if (region_ok(mbi) && mbi.Type != MEM_MAPPED && mbi.RegionSize < (256u << 20)) {
            if (!search_region(static_cast<const uint8_t*>(mbi.BaseAddress), mbi.RegionSize,
                               needles, nbytes, 3, self - 4096, self + 4096, on_hit, &ctx)) faulted++;
        }
        a = next;
    }

    { std::lock_guard<std::mutex> lock(g_mutex); g_scan_hits = hits; }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_scan_struct = 0;
        g_scan_cams.clear();
        for (const MemHit& h : hits) {
            if (strcmp(h.kind, "camera position float3") != 0) continue;
            bool row12 = false, col16 = false, col60 = false;
            for (const MemHit& k : hits) {
                if (k.addr == h.addr + 12   && strncmp(k.kind, "view row-major", 14) == 0)    row12 = true;
                if (k.addr == h.addr + 0x10 && strncmp(k.kind, "view column-major", 17) == 0) col16 = true;
                if (k.addr == h.addr + 0x60 && strncmp(k.kind, "view column-major", 17) == 0) col60 = true;
            }
            if (col16) g_scan_cams.push_back(h.addr);                       // array entry: pos, then col-major at +0x10
            if (row12 && col60 && g_scan_struct == 0) g_scan_struct = h.addr; // the three-way object, if any
        }
    }

    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\ffxvr_memscan.log", g_dir);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a") != 0 || f == nullptr) return -2;
    fprintf(f, "memscan at tick %lu: view t=(%.3f %.3f %.3f) pos=(%.3f %.3f %.3f)  %zu hits, %d regions faulted; camera object %08X, %zu array entries\n",
            GetTickCount(), v[3], v[7], v[11], pos[0], pos[1], pos[2], hits.size(), faulted, static_cast<unsigned>(g_scan_struct), g_scan_cams.size());
    size_t logged = 0;
    for (const MemHit& h : hits) {
        if (logged++ >= 250) break;
        char where[128]; describe_region(h.addr, where, sizeof(where));
        fprintf(f, "  %08X  %-36s %s\n", static_cast<unsigned>(h.addr), h.kind, where);
    }
    fprintf(f, "\n");
    fclose(f);
    return static_cast<int>(hits.size());
}

// ---------------------------------------------------------------------------
// Write watch: who writes the game's camera?
//
// A hardware data breakpoint (DR0/DR1, write, 4 bytes) on an address, set on
// the game's main thread from a helper thread, with a vectored exception
// handler that records the writing instruction's address and continues. The
// resulting table names the function that computes the camera each frame --
// the place to hook so that UI projection, rendering and culling all follow.
// ---------------------------------------------------------------------------


static LONG CALLBACK watch_veh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    if ((c->Dr6 & 0x3) == 0) return EXCEPTION_CONTINUE_SEARCH;     // not one of ours

    const uintptr_t eip = c->Eip;   // instruction after the write
    g_watch_hits++;
    bool found = false;
    for (int i = 0; i < g_writers_n; ++i) if (g_writers[i].eip == eip) { g_writers[i].count++; found = true; break; }
    if (!found && g_writers_n < 32) g_writers[g_writers_n++] = { eip, 1 };

    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

struct WatchJob { DWORD tid; uintptr_t a0; uintptr_t a1; int result; };

static DWORD WINAPI watch_thread(LPVOID arg) {
    WatchJob* job = static_cast<WatchJob*>(arg);
    HANDLE t = OpenThread(THREAD_ALL_ACCESS, FALSE, job->tid);
    if (t == nullptr) { job->result = -1; return 0; }

    if (SuspendThread(t) == (DWORD)-1) { CloseHandle(t); job->result = -2; return 0; }
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(t, &c)) { ResumeThread(t); CloseHandle(t); job->result = -3; return 0; }

    c.Dr0 = job->a0;
    c.Dr1 = job->a1;
    c.Dr7 = 0;
    if (job->a0) c.Dr7 |= (1u << 0) | (0b01u << 16) | (0b11u << 18);   // L0, write, 4 bytes
    if (job->a1) c.Dr7 |= (1u << 2) | (0b01u << 20) | (0b11u << 22);   // L1, write, 4 bytes
    c.Dr6 = 0;
    const BOOL ok = SetThreadContext(t, &c);
    ResumeThread(t);
    CloseHandle(t);
    job->result = ok ? 0 : -4;
    return 0;
}

// ---------------------------------------------------------------------------
// Static cross-references: which instructions touch the game's camera?
//
// Fahrenheit's stage-0 debugger swallows first-chance exceptions, so a
// hardware breakpoint never reaches us. But 32-bit code addresses a global
// with a literal imm32, so scanning the executable's code for the matrix's
// addresses finds every instruction that reads or writes it. The bytes just
// before the immediate say which.
// ---------------------------------------------------------------------------

struct XrefKind { const uint8_t* pat; int len; int imm_at; const char* name; };

static const char* classify_xref(const uint8_t* at) {
    // `at` points at the imm32. Look at up to 4 preceding bytes.
    const uint8_t b1 = at[-1], b2 = at[-2], b3 = at[-3], b4 = at[-4];
    // modrm with mod=00 rm=101 (disp32, no base) has low 3 bits 101 and top 2 bits 00
    const bool modrm_disp32 = (b1 & 0xC7) == 0x05;
    if (!modrm_disp32) {
        if (b1 >= 0xB8 && b1 <= 0xBF) return "mov r32, imm (address taken)";
        if (b1 == 0x68)                return "push imm (address taken)";
        return "other";
    }
    // SSE stores / loads
    if (b3 == 0x0F && b2 == 0x11) return "STORE movups/movss [g], xmm";
    if (b3 == 0x0F && b2 == 0x29) return "STORE movaps [g], xmm";
    if (b4 == 0xF3 && b3 == 0x0F && b2 == 0x11) return "STORE movss [g], xmm";
    if (b3 == 0x0F && b2 == 0x10) return "load movups [g]";
    if (b3 == 0x0F && b2 == 0x28) return "load movaps [g]";
    if (b4 == 0xF3 && b3 == 0x0F && b2 == 0x10) return "load movss [g]";
    if (b3 == 0x0F && b2 == 0x58) return "load addps [g]";
    if (b3 == 0x0F && b2 == 0x59) return "load mulps [g]";
    if (b4 == 0xF3 && b3 == 0x0F && b2 == 0x59) return "load mulss [g]";
    // integer / x87
    if (b2 == 0x89) return "STORE mov [g], r32";
    if (b2 == 0xC7) return "STORE mov [g], imm32";
    if (b2 == 0x8B) return "load mov r32, [g]";
    if (b2 == 0x8D) return "lea r32, [g] (address taken)";
    if (b2 == 0xD9 && (b1 & 0x38) == 0x18) return "STORE fstp [g]";
    if (b2 == 0xD9 && (b1 & 0x38) == 0x00) return "load fld [g]";
    if (b2 == 0xFF && (b1 & 0x38) == 0x30) return "push [g]";
    return "modrm disp32 (unclassified)";
}

/// Scans the executable's code sections for references to [addr, addr+span)
/// and logs each with its classification. Returns the number found.
extern "C" __declspec(dllexport) int ffxvr_xrefs(uintptr_t addr, unsigned span) {
    HMODULE exe = GetModuleHandleW(nullptr);
    if (exe == nullptr) return -1;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(exe);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const IMAGE_NT_HEADERS* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);

    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\ffxvr_xrefs.log", g_dir);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a") != 0 || f == nullptr) return -2;
    fprintf(f, "xrefs to %08X..%08X (FFX.exe+%X)\n", static_cast<unsigned>(addr), static_cast<unsigned>(addr + span),
            static_cast<unsigned>(addr - reinterpret_cast<uintptr_t>(base)));

    int n = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uint8_t* cs = base + sec[i].VirtualAddress;
        const size_t   cl = sec[i].Misc.VirtualSize;
        for (size_t off = 4; off + 4 <= cl; ++off) {
            uint32_t imm; memcpy(&imm, cs + off, 4);
            if (imm < addr || imm >= addr + span) continue;
            const uint8_t* at = cs + off;
            fprintf(f, "  code FFX.exe+%08X  -> data FFX.exe+%06X (%+d from matrix)  %s   bytes:",
                    static_cast<unsigned>(at - base), static_cast<unsigned>(imm - reinterpret_cast<uintptr_t>(base)),
                    static_cast<int>(static_cast<intptr_t>(imm) - static_cast<intptr_t>(reinterpret_cast<uintptr_t>(base) + 0x88FD3C)),
                    classify_xref(at));
            for (int k = -6; k < 4; ++k) fprintf(f, " %02X", at[k]);
            fprintf(f, "\n");
            n++;
            if (n >= 400) break;
        }
    }
    fprintf(f, "  %d references\n\n", n);
    fclose(f);
    return n;
}

// ---------------------------------------------------------------------------
// Pointer scan: which variables point at the camera?
//
// Code that never names the matrix reaches it through a pointer. Search the
// executable's data sections (and, separately counted, the heap) for 4-byte
// values in [target - below, target], then cross-reference each pointer
// variable found in the executable, since those are the camera accessors.
// ---------------------------------------------------------------------------

/// Heap half of the pointer scan, in a plain function so it can use SEH.
static int ptrscan_heap(FILE* f, uintptr_t target, unsigned below) {
    int heap_n = 0;
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t a = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi)) == 0) break;
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= a) break;
        if (region_ok(mbi) && mbi.Type == MEM_PRIVATE && mbi.RegionSize < (256u << 20)) {
            const uint8_t* rb = static_cast<const uint8_t*>(mbi.BaseAddress);
            __try {
                for (size_t off = 0; off + 4 <= mbi.RegionSize; off += 4) {
                    uint32_t v; memcpy(&v, rb + off, 4);
                    if (v + below < target || v > target) continue;
                    const uintptr_t at = reinterpret_cast<uintptr_t>(rb + off);
                    if (at >= target - 0x1000 && at <= target + 0x1000) continue;   // inside the object itself
                    if (heap_n < 12) fprintf(f, "  heap %08X holds %08X (target%+d)\n", static_cast<unsigned>(at), v,
                                             static_cast<int>(static_cast<intptr_t>(v) - static_cast<intptr_t>(target)));
                    heap_n++;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        a = next;
    }
    return heap_n;
}

extern "C" __declspec(dllexport) int ffxvr_ptrscan(uintptr_t target, unsigned below) {
    HMODULE exe = GetModuleHandleW(nullptr);
    if (exe == nullptr || target == 0) return -1;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(exe);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const IMAGE_NT_HEADERS* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);

    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\ffxvr_xrefs.log", g_dir);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a") != 0 || f == nullptr) return -2;
    fprintf(f, "pointers into [%08X-%u, %08X]:\n", static_cast<unsigned>(target), below, static_cast<unsigned>(target));

    std::vector<uintptr_t> ptr_vars;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        const uint8_t* ds = base + sec[i].VirtualAddress;
        const size_t   dl = sec[i].Misc.VirtualSize;
        for (size_t off = 0; off + 4 <= dl; off += 4) {
            uint32_t v; memcpy(&v, ds + off, 4);
            if (v + below < target || v > target) continue;
            const uintptr_t at = reinterpret_cast<uintptr_t>(ds + off);
            fprintf(f, "  exe data FFX.exe+%06X holds %08X (target%+d)\n", static_cast<unsigned>(at - reinterpret_cast<uintptr_t>(base)),
                    v, static_cast<int>(static_cast<intptr_t>(v) - static_cast<intptr_t>(target)));
            if (ptr_vars.size() < 32) ptr_vars.push_back(at);
        }
    }

    // Heap: count only, with the first few, so a pointer field inside another object shows up.
    const int heap_n = ptrscan_heap(f, target, below);
    fprintf(f, "  %zu pointer variables in the exe, %d on the heap\n\n", ptr_vars.size(), heap_n);
    fclose(f);

    // Chain: who reads each pointer variable?
    for (uintptr_t pv : ptr_vars) ffxvr_xrefs(pv, 4);
    return static_cast<int>(ptr_vars.size());
}

extern "C" __declspec(dllexport) uintptr_t ffxvr_scan_struct() { return g_scan_struct; }
extern "C" __declspec(dllexport) int ffxvr_scan_cams() { std::lock_guard<std::mutex> lock(g_mutex); return static_cast<int>(g_scan_cams.size()); }

/// Arms write watches on up to two addresses (0 = none) on the game's main
/// thread. Returns 0 on success.
extern "C" __declspec(dllexport) int ffxvr_watch(uintptr_t a0, uintptr_t a1) {
    if (g_main_tid == 0) return -10;
    if (g_veh == nullptr) g_veh = AddVectoredExceptionHandler(1, watch_veh);

    g_writers_n  = 0;
    g_watch_hits = 0;
    g_watch_addr[0] = a0; g_watch_addr[1] = a1;

    WatchJob job = { g_main_tid, a0, a1, -99 };
    HANDLE h = CreateThread(nullptr, 0, watch_thread, &job, 0, nullptr);
    if (h == nullptr) return -11;
    WaitForSingleObject(h, 5000);
    CloseHandle(h);
    return job.result;
}

extern "C" __declspec(dllexport) int ffxvr_watch_hits() { return static_cast<int>(g_watch_hits); }

/// The address of the game's static camera matrix (column-major view),
/// found by the memory scan: FFX.exe + 0x88FD3C.
extern "C" __declspec(dllexport) uintptr_t ffxvr_exe_camera() {
    HMODULE exe = GetModuleHandleW(nullptr);
    return exe ? reinterpret_cast<uintptr_t>(exe) + 0x88FD3C : 0;
}

extern "C" __declspec(dllexport) int ffxvr_override_applied() { return static_cast<int>(g_ovr_applied); }
extern "C" __declspec(dllexport) int ffxvr_override_mask()    { return static_cast<int>(g_ovr_mask); }


/// Appends a line to ffxvr_diag.log (the heartbeat rewrites ffxvr_native.log
/// every second, so anything appended there is lost).
static void diag(const char* fmt, ...) {
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\ffxvr_diag.log", g_dir);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a") != 0 || f == nullptr) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static ID3D11Texture2D* texture_of(ID3D11View* view) {
    if (view == nullptr) return nullptr;
    ID3D11Resource* res = nullptr; view->GetResource(&res);
    if (res == nullptr) return nullptr;
    ID3D11Texture2D* tex = nullptr;
    res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
    res->Release();
    if (tex) tex->Release();          // we only use it as a key; the game keeps it alive
    return tex;
}

static Twin* twin_of(ID3D11Texture2D* game) {
    if (game == nullptr || g_device == nullptr) return nullptr;
    auto it = g_twins.find(game);
    if (it != g_twins.end()) return &it->second;

    D3D11_TEXTURE2D_DESC d; game->GetDesc(&d);
    Twin t = {}; t.game = game; t.w = d.Width; t.h = d.Height; t.fmt = d.Format;

    D3D11_TEXTURE2D_DESC cd = d;
    cd.Usage = D3D11_USAGE_DEFAULT; cd.CPUAccessFlags = 0; cd.MiscFlags = 0;
    if (game == g_backbuffer) { cd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE; }

    auto fail = [&](const char* what, HRESULT hr) -> Twin* {
        diag("twin FAILED (%s hr=0x%08X): %ux%u fmt %u mips %u array %u samples %u bind 0x%X misc 0x%X usage %u",
             what, static_cast<unsigned>(hr), d.Width, d.Height, static_cast<unsigned>(d.Format), d.MipLevels, d.ArraySize,
             d.SampleDesc.Count, d.BindFlags, d.MiscFlags, static_cast<unsigned>(d.Usage));
        if (t.tex) t.tex->Release();
        Twin none = {}; none.game = game;          // remembered as failed: no views, never retried
        g_twins[game] = none;
        return nullptr;
    };

    HRESULT hr = g_device->CreateTexture2D(&cd, nullptr, &t.tex);
    if (FAILED(hr)) return fail("CreateTexture2D", hr);
    if (cd.BindFlags & D3D11_BIND_RENDER_TARGET) {
        hr = g_device->CreateRenderTargetView(t.tex, nullptr, &t.rtv);
        if (FAILED(hr)) return fail("CreateRenderTargetView", hr);
    }
    if (cd.BindFlags & D3D11_BIND_DEPTH_STENCIL) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
        dd.Format = (d.Format == DXGI_FORMAT_R24G8_TYPELESS) ? DXGI_FORMAT_D24_UNORM_S8_UINT
                  : (d.Format == DXGI_FORMAT_R32_TYPELESS)  ? DXGI_FORMAT_D32_FLOAT
                  : (d.Format == DXGI_FORMAT_R16_TYPELESS)  ? DXGI_FORMAT_D16_UNORM : d.Format;
        dd.ViewDimension = d.SampleDesc.Count > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
        hr = g_device->CreateDepthStencilView(t.tex, &dd, &t.dsv);
        if (FAILED(hr)) return fail("CreateDepthStencilView", hr);
    }
    g_twin_count++;
    return &(g_twins[game] = t);
}

static ID3D11RenderTargetView* twin_rtv(ID3D11RenderTargetView* v) {
    Twin* t = twin_of(texture_of(v));
    return t ? t->rtv : nullptr;
}
static ID3D11DepthStencilView* twin_dsv(ID3D11DepthStencilView* v) {
    Twin* t = twin_of(texture_of(v));
    return t ? t->dsv : nullptr;
}
/// The twin's SRV matching a game SRV, or null if the sampled texture is not
/// something the game renders into (ordinary textures pass through).
static ID3D11ShaderResourceView* twin_srv(ID3D11ShaderResourceView* v) {
    if (v == nullptr) return nullptr;
    ID3D11Texture2D* game = texture_of(v);
    if (game == nullptr) return nullptr;
    auto it = g_twins.find(game);
    if (it == g_twins.end() || it->second.tex == nullptr) return nullptr;   // only textures we have twins of
    Twin& t = it->second;
    auto sit = t.srvs.find(v);
    if (sit != t.srvs.end()) return sit->second;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd; v->GetDesc(&sd);
    ID3D11ShaderResourceView* nv = nullptr;
    HRESULT hr = g_device->CreateShaderResourceView(t.tex, &sd, &nv);
    if (FAILED(hr)) {
        nv = nullptr;
        D3D11_TEXTURE2D_DESC td; t.tex->GetDesc(&td);
        diag("twin SRV FAILED hr=0x%08X: view fmt %u dim %d on twin %ux%u fmt %u bind 0x%X", static_cast<unsigned>(hr),
             static_cast<unsigned>(sd.Format), static_cast<int>(sd.ViewDimension), td.Width, td.Height, static_cast<unsigned>(td.Format), td.BindFlags);
    }
    t.srvs[v] = nv;
    return nv;
}


/// Copies the scene target (eye 1) and its twin (eye 2) at the same instant.
/// Called the moment the game first binds the scene texture as a shader
/// input -- i.e. when post-processing starts reading it -- so every pass
/// that draws into it, the late transparent ones included, is in the copy.
static void snapshot_scene(ID3D11Texture2D* game) {
    auto it = g_twins.find(game);
    if (it == g_twins.end() || it->second.tex == nullptr) return;
    Twin& src = it->second;
    if (g_snap_tex == nullptr || g_snap_from != game) {
        if (g_snap_srv)      { g_snap_srv->Release();      g_snap_srv = nullptr; }
        if (g_snap_tex)      { g_snap_tex->Release();      g_snap_tex = nullptr; }
        if (g_snap_game_srv) { g_snap_game_srv->Release(); g_snap_game_srv = nullptr; }
        if (g_snap_game)     { g_snap_game->Release();     g_snap_game = nullptr; }
        D3D11_TEXTURE2D_DESC d; src.tex->GetDesc(&d);
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = 0;
        if (SUCCEEDED(g_device->CreateTexture2D(&d, nullptr, &g_snap_tex)))  g_device->CreateShaderResourceView(g_snap_tex,  nullptr, &g_snap_srv);
        if (SUCCEEDED(g_device->CreateTexture2D(&d, nullptr, &g_snap_game))) g_device->CreateShaderResourceView(g_snap_game, nullptr, &g_snap_game_srv);
        g_snap_from = game;
    }
    g_in_eye2 = true;                                  // our copies must not be mirrored
    if (g_snap_tex)  g_ctx->CopyResource(g_snap_tex,  src.tex);
    if (g_snap_game) g_ctx->CopyResource(g_snap_game, game);
    g_in_eye2 = false;
    g_snap_frame = g_frames;
}

/// Snapshot both eyes if `tex` is the texture that took the scene pass this
/// frame and no snapshot was taken yet. `how` records the trigger.
static bool snapshot_if_scene(ID3D11Texture2D* tex, uint32_t how) {
    if (!g_stereo || tex == nullptr || g_snap_frame == g_frames) return false;
    if (tex != g_scene_live_tex || g_scene_live_draws < 60) return false;
    snapshot_scene(tex);
    g_snap_by = how;
    return true;
}

/// Switches every current target to its twin, every sampled render texture
/// to its twin, and uploads the eye-2 image of the object buffer if there is
/// one. The managed detour then re-issues the draw.
ID3D11ShaderResourceView* g_saved_ps_srv[16] = {};
extern "C" __declspec(dllexport) int ffxvr_eye2_begin() {
    if (!g_stereo || g_ctx == nullptr || g_device == nullptr) { g_e2_begin_fail++; return 0; }
    if (g_cur_rtv_n == 0 && g_cur_dsv == nullptr) { g_e2_begin_fail++; return 0; }

    ID3D11RenderTargetView* rtvs[4] = {};
    for (UINT i = 0; i < g_cur_rtv_n; ++i) rtvs[i] = twin_rtv(g_cur_rtv[i]);
    ID3D11DepthStencilView* dsv = twin_dsv(g_cur_dsv);
    const bool to_bb = g_backbuffer && g_cur_rtv_n > 0 && texture_of(g_cur_rtv[0]) == g_backbuffer;
    if (to_bb) { g_bb_begin++; if (rtvs[0] == nullptr) g_bb_twin_null++; }
    if (g_cur_rtv_n > 0 && rtvs[0] == nullptr) { g_e2_begin_fail++; return 0; }

    g_in_eye2 = true;
    g_ctx->OMSetRenderTargets(g_cur_rtv_n, rtvs, dsv);
    {
        ID3D11Texture2D* gt = g_cur_rtv_n > 0 ? texture_of(g_cur_rtv[0]) : nullptr;
        if (gt != g_prev_e2_target) { g_prev_e2_target = gt; g_prev_e2_run = 0; }
        g_prev_e2_run++;
        if (g_debug_red && gt && g_cur_dsv != nullptr && rtvs[0] != nullptr && g_red_done_frame[gt] != g_frames) {
            g_red_done_frame[gt] = g_frames;
            const float red[4] = { 1.f, 0.f, 0.f, 1.f };
            g_ctx->ClearRenderTargetView(rtvs[0], red); g_red_clears++;
        }
        if (gt && g_cur_dsv != nullptr) {
            const uint32_t c = ++g_draws_per_target[gt];
            if (c > g_scene_live_draws) { g_scene_live_draws = c; g_scene_live_tex = gt; }
        }
    }

    ID3D11ShaderResourceView* srvs[16];
    bool any = false;
    char inputs[160] = ""; size_t il = 0;
    for (int i = 0; i < 16; ++i) {
        g_saved_ps_srv[i] = g_cur_ps_srv[i];
        ID3D11ShaderResourceView* tw = twin_srv(g_cur_ps_srv[i]);
        srvs[i] = tw ? tw : g_cur_ps_srv[i];
        if (tw) any = true;
        if (g_frame_live && g_cur_ps_srv[i] != nullptr) {
            // name the input: a rendered texture (twinned or not) by size, else "tex"
            ID3D11Texture2D* t = texture_of(g_cur_ps_srv[i]);
            D3D11_TEXTURE2D_DESC d = {}; if (t) t->GetDesc(&d);
            const bool rendered = (d.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) != 0;
            const bool copied   = !rendered && g_twins.find(t) != g_twins.end();
            if (rendered || copied) {
                il += snprintf(inputs + il, sizeof(inputs) - il, " s%d:%p %ux%u f%u%s", i, static_cast<void*>(t), d.Width, d.Height,
                               static_cast<unsigned>(d.Format), tw ? "" : (rendered ? "!NO-TWIN" : "!NO-TWIN(copy)"));
                if (il >= sizeof(inputs) - 1) break;
            }
        }
    }
    if (g_frame_live && inputs[0]) {
        char t[160]; snprintf(t, sizeof(t), "      eye2 draw inputs:%s", inputs);
        std::lock_guard<std::mutex> lock(g_mutex);
        FrameEv e; strncpy_s(e.text, t, _TRUNCATE);
        if (g_frame_ev.size() < 2000) g_frame_ev.push_back(e);
    }
    if (any) g_ctx->PSSetShaderResources(0, 16, srvs);

    // Upload the eye-2 image of every bound VS constant buffer that has one,
    // whichever slot it sits in: the shader may take the camera from a slot
    // other than 0.
    for (int sl = 0; sl < 14; ++sl) {
        g_eye2_slots[sl] = nullptr;
        ID3D11Buffer* cb = g_vs_cb[sl];
        if (cb == nullptr) continue;
        for (int k = 0; k < 32; ++k) {
            EyeImage& e = g_eye_ring[(g_eye_ring_next - 1 - k + 64) % 32];
            if (e.cb == cb && e.frame == g_frames) { g_eye2_slots[sl] = &e; break; }
        }
        if (g_eye2_slots[sl] == nullptr) continue;
        bool dup = false;
        for (int q = 0; q < sl; ++q) if (g_eye2_slots[q] == g_eye2_slots[sl]) dup = true;   // same buffer in two slots
        if (dup) { g_eye2_slots[sl] = nullptr; continue; }
        g_e2_uploads++;
        if (g_eye2_slots[sl]->eye1 == g_eye2_slots[sl]->eye2) g_e2_identical++;
        g_ctx->UpdateSubresource(cb, 0, nullptr, g_eye2_slots[sl]->eye2.data(), 0, 0);
    }
    return 1;
}

/// Restores the game's targets, sampled textures and the eye-1 image.
extern "C" __declspec(dllexport) void ffxvr_eye2_end() {
    if (!g_in_eye2 || g_ctx == nullptr) return;
    g_ctx->OMSetRenderTargets(g_cur_rtv_n, g_cur_rtv, g_cur_dsv);
    g_ctx->PSSetShaderResources(0, 16, g_saved_ps_srv);
    if (!g_debug_leave) {
        for (int sl = 0; sl < 14; ++sl) {
            if (g_eye2_slots[sl] != nullptr) g_ctx->UpdateSubresource(g_eye2_slots[sl]->cb, 0, nullptr, g_eye2_slots[sl]->eye1.data(), 0, 0);
        }
    }
    g_eye2_draws++;
    g_in_eye2 = false;
}

/// Tracks the pixel shader's resources, and in eye-2 mode is skipped.
extern "C" __declspec(dllexport) void ffxvr_on_ps_srvs(unsigned start, unsigned n, void* const* views) {
    if (g_in_eye2) return;
    for (unsigned i = 0; i < n && start + i < 16; ++i) g_cur_ps_srv[start + i] = views ? static_cast<ID3D11ShaderResourceView*>(views[i]) : nullptr;
    // The first time this frame the game reads the texture that took the
    // scene pass, the scene is complete: snapshot both eyes now.
    if (views == nullptr) return;
    for (unsigned i = 0; i < n; ++i) {
        if (views[i] == nullptr) continue;
        if (snapshot_if_scene(texture_of(static_cast<ID3D11View*>(views[i])), 1)) break;
    }
}

/// MSAA resolve mirrored onto the twins (the one way, besides copies, that a
/// rendered texture's content reaches another texture). Hooked after the
/// game's own call.
extern "C" __declspec(dllexport) void ffxvr_on_resolve(void* dst_res, unsigned dst_sub, void* src_res, unsigned src_sub, unsigned fmt) {
    if (g_in_eye2 || g_ctx == nullptr || dst_res == nullptr || src_res == nullptr) return;
    g_resolves++;
    ID3D11Texture2D* dst = nullptr; static_cast<ID3D11Resource*>(dst_res)->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&dst));
    ID3D11Texture2D* src = nullptr; static_cast<ID3D11Resource*>(src_res)->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&src));
    if (dst && src) {
        if (g_frame_live) {
            char a[96], b[96]; char t[200];
            D3D11_TEXTURE2D_DESC dd; dst->GetDesc(&dd); D3D11_TEXTURE2D_DESC sd; src->GetDesc(&sd);
            snprintf(a, sizeof(a), "%p %ux%u fmt %u", static_cast<void*>(dst), dd.Width, dd.Height, static_cast<unsigned>(dd.Format));
            snprintf(b, sizeof(b), "%p %ux%u fmt %u samples %u", static_cast<void*>(src), sd.Width, sd.Height, static_cast<unsigned>(sd.Format), sd.SampleDesc.Count);
            snprintf(t, sizeof(t), "RSV %s  <-  %s", a, b);
            std::lock_guard<std::mutex> lock(g_mutex); frame_event(t);
        }
        auto sit = g_twins.find(src);
        if (g_stereo && sit != g_twins.end() && sit->second.tex != nullptr) {
            Twin* dt = twin_of(dst);
            if (dt && dt->tex) {
                g_in_eye2 = true;
                g_ctx->ResolveSubresource(dt->tex, dst_sub, sit->second.tex, src_sub, static_cast<DXGI_FORMAT>(fmt));
                g_in_eye2 = false;
                g_resolves_mirrored++;
                // The first resolve of the scene pass is the finished scene, in a
                // plain (non-multisampled) texture: that is the eye image.
                if (src == g_scene_live_tex && g_scene_live_draws >= 60 && g_snap_frame != g_frames) { snapshot_scene(dst); g_snap_by = 2; }
            }
        }
    }
    if (dst) dst->Release();
    if (src) src->Release();
}

/// The UAV variant of target binding: same bookkeeping as the plain one.
extern "C" __declspec(dllexport) void ffxvr_on_rendertargets_uav(unsigned n, void* const* rtvs, void* dsv) {
    if (g_in_eye2) return;
    g_omrt_uav_calls++;
    if (n == 0xFFFFFFFFu) return;                      // D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL
    ffxvr_on_rendertargets(n, rtvs, dsv);
}

/// Clears mirror to the twin, so eye 2's targets start each pass the same way.
extern "C" __declspec(dllexport) void ffxvr_on_clear_twin(void* view, int depth, const float* color, unsigned flags, float d, unsigned char st) {
    if (g_in_eye2 || !g_stereo || g_ctx == nullptr) return;
    if (!depth) snapshot_if_scene(texture_of(static_cast<ID3D11View*>(view)), 3);   // about to be wiped: last chance
    g_in_eye2 = true;
    if (depth) { ID3D11DepthStencilView* t = twin_dsv(static_cast<ID3D11DepthStencilView*>(view)); if (t) g_ctx->ClearDepthStencilView(t, flags, d, st); }
    else       { ID3D11RenderTargetView* t = twin_rtv(static_cast<ID3D11RenderTargetView*>(view)); if (t) g_ctx->ClearRenderTargetView(t, color); }
    g_in_eye2 = false;
}

/// Mirrors a resource copy onto the twins: when the game copies into a
/// texture we twin (the backbuffer included), the source's twin -- or the
/// source itself if it has none -- is copied into the destination's twin.
/// Returns 1 if mirrored. The region form passes a box; full copies pass null.
extern "C" __declspec(dllexport) int ffxvr_on_copy(void* dst_res, unsigned dst_sub, unsigned x, unsigned y, unsigned z,
                                                   void* src_res, unsigned src_sub, const void* box, int whole) {
    if (g_in_eye2 || g_ctx == nullptr || dst_res == nullptr || src_res == nullptr) return 0;

    ID3D11Texture2D* dst = nullptr; static_cast<ID3D11Resource*>(dst_res)->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&dst));
    ID3D11Texture2D* src = nullptr; static_cast<ID3D11Resource*>(src_res)->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&src));
    if (src) snapshot_if_scene(src, 2);
    if (dst) dst->Release();
    if (src) src->Release();
    if (dst == nullptr || src == nullptr) return 0;

    if (g_frame_live) {
        D3D11_TEXTURE2D_DESC dd = {}, sd = {}; dst->GetDesc(&dd); src->GetDesc(&sd);
        const bool st = g_twins.find(src) != g_twins.end() && g_twins[src].tex != nullptr;
        const bool dt = g_twins.find(dst) != g_twins.end() && g_twins[dst].tex != nullptr;
        char t[160];
        snprintf(t, sizeof(t), "CPY %s %ux%u fmt %u%s -> %ux%u fmt %u%s%s%s", whole ? "whole" : "region",
                 sd.Width, sd.Height, static_cast<unsigned>(sd.Format), st ? " (twinned)" : " (NO twin)",
                 dd.Width, dd.Height, static_cast<unsigned>(dd.Format), dt ? " (twinned)" : "",
                 dst == g_backbuffer ? "  [BACKBUFFER]" : "", g_overlay_phase ? "  [overlay phase]" : "");
        std::lock_guard<std::mutex> lock(g_mutex);
        frame_event(t);
    }
    if (!g_stereo) return 0;

    // Mirror when the destination is something we twin (a render texture or
    // the backbuffer), or when the source is: a depth or colour buffer copied
    // into a plain texture for post-processing to sample must carry eye 2's
    // contents too, so the destination gets a twin as well.
    if (dst == g_backbuffer) g_bb_copies++;
    auto its = g_twins.find(src);
    const bool src_twinned = its != g_twins.end() && its->second.tex != nullptr;
    if (g_twins.find(dst) == g_twins.end() && dst != g_backbuffer && !src_twinned) return 0;
    Twin* td = twin_of(dst);
    if (td == nullptr || td->tex == nullptr) return 0;

    ID3D11Resource* from = src_twinned ? static_cast<ID3D11Resource*>(its->second.tex) : static_cast<ID3D11Resource*>(src);

    g_in_eye2 = true;
    if (whole) g_ctx->CopyResource(td->tex, from);
    else       g_ctx->CopySubresourceRegion(td->tex, dst_sub, x, y, z, from, src_sub, static_cast<const D3D11_BOX*>(box));
    g_in_eye2 = false;
    return 1;
}

// ---------------------------------------------------------------------------
// Measuring eye 2 against eye 1
//
// Reads two textures back and reports the mean absolute difference per
// channel and the fraction of pixels that differ at all. Identical images
// give 0; a horizontally shifted view gives a clearly non-zero value.
// ---------------------------------------------------------------------------

static bool readback_diff(ID3D11Texture2D* a, ID3D11Texture2D* b, double* mean_abs, double* frac_diff, double* bright_a = nullptr, double* bright_b = nullptr) {
    if (a == nullptr || b == nullptr || g_device == nullptr || g_ctx == nullptr) return false;
    D3D11_TEXTURE2D_DESC da, db; a->GetDesc(&da); b->GetDesc(&db);
    if (da.Width != db.Width || da.Height != db.Height) return false;

    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width = da.Width; sd.Height = da.Height; sd.MipLevels = 1; sd.ArraySize = 1;
    sd.Format = da.Format; sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *sa = nullptr, *sb = nullptr;
    HRESULT h1 = g_device->CreateTexture2D(&sd, nullptr, &sa);
    HRESULT h2 = SUCCEEDED(h1) ? g_device->CreateTexture2D(&sd, nullptr, &sb) : E_FAIL;
    if (FAILED(h1) || FAILED(h2)) {
        diag("readback: staging create failed hr=0x%08X/0x%08X for %ux%u fmt %u (a: bind 0x%X misc 0x%X samples %u mips %u; b: bind 0x%X misc 0x%X samples %u mips %u)",
             static_cast<unsigned>(h1), static_cast<unsigned>(h2), da.Width, da.Height, static_cast<unsigned>(da.Format),
             da.BindFlags, da.MiscFlags, da.SampleDesc.Count, da.MipLevels, db.BindFlags, db.MiscFlags, db.SampleDesc.Count, db.MipLevels);
        if (sa) sa->Release(); if (sb) sb->Release(); return false;
    }
    g_in_eye2 = true;                                  // our copies must not be mirrored
    g_ctx->CopyResource(sa, a); g_ctx->CopyResource(sb, b);
    g_in_eye2 = false;

    D3D11_MAPPED_SUBRESOURCE ma = {}, mb = {};
    bool ok = SUCCEEDED(g_orig_map(g_ctx, sa, 0, D3D11_MAP_READ, 0, &ma)) && SUCCEEDED(g_orig_map(g_ctx, sb, 0, D3D11_MAP_READ, 0, &mb));
    if (ok) {
        double sum = 0, la = 0, lb = 0; uint64_t n = 0, diff = 0;
        for (UINT y = 0; y < da.Height; y += 4) {       // every 4th row and pixel: plenty for a verdict
            const uint8_t* ra = static_cast<const uint8_t*>(ma.pData) + y * ma.RowPitch;
            const uint8_t* rb = static_cast<const uint8_t*>(mb.pData) + y * mb.RowPitch;
            for (UINT x = 0; x < da.Width; x += 4) {
                int d = 0;
                for (int c = 0; c < 3; ++c) { d += abs(static_cast<int>(ra[x*4+c]) - static_cast<int>(rb[x*4+c])); la += ra[x*4+c]; lb += rb[x*4+c]; }
                sum += d / 3.0; n++; if (d > 6) diff++;
            }
        }
        *mean_abs = n ? sum / n : 0; *frac_diff = n ? static_cast<double>(diff) / n : 0;
        if (bright_a) *bright_a = n ? la / (3.0 * n) : 0;
        if (bright_b) *bright_b = n ? lb / (3.0 * n) : 0;
    }
    if (ma.pData) g_orig_unmap(g_ctx, sa, 0);
    if (mb.pData) g_orig_unmap(g_ctx, sb, 0);
    sa->Release(); sb->Release();
    return ok;
}

/// Writes a texture, downsampled 4x (point sampled), as a 24-bit BMP next to
/// the logs. BGRA/RGBA 8-bit formats only. Returns true on success.
static bool dump_bmp(ID3D11Texture2D* tex, const wchar_t* name, bool alpha_only = false) {
    if (tex == nullptr || g_device == nullptr || g_ctx == nullptr) return false;
    D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
    if (d.SampleDesc.Count != 1) { diag("dump %ls: multisampled, skipped", name); return false; }
    const bool bgra = (d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS);
    const bool rgba = (d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS);
    if (!bgra && !rgba) { diag("dump %ls: format %u not 8-bit colour, skipped", name, static_cast<unsigned>(d.Format)); return false; }
    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width = d.Width; sd.Height = d.Height; sd.MipLevels = 1; sd.ArraySize = 1;
    sd.Format = d.Format; sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    HRESULT hr = g_device->CreateTexture2D(&sd, nullptr, &st);
    if (FAILED(hr)) { diag("dump %ls: staging failed hr=0x%08X", name, static_cast<unsigned>(hr)); return false; }
    g_in_eye2 = true; g_ctx->CopyResource(st, tex); g_in_eye2 = false;
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(g_orig_map(g_ctx, st, 0, D3D11_MAP_READ, 0, &m))) { st->Release(); return false; }
    const UINT w = d.Width / 4, h = d.Height / 4, stride = (w * 3 + 3) & ~3u;
    std::vector<uint8_t> out(static_cast<size_t>(stride) * h);
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y * 4) * m.RowPitch;
        uint8_t* o = out.data() + static_cast<size_t>(h - 1 - y) * stride;      // BMP is bottom-up
        for (UINT x = 0; x < w; ++x) {
            const uint8_t* px = row + static_cast<size_t>(x * 4) * 4;
            if (alpha_only) { o[x*3+0] = o[x*3+1] = o[x*3+2] = px[3]; }
            else if (bgra)  { o[x*3+0] = px[0]; o[x*3+1] = px[1]; o[x*3+2] = px[2]; }
            else            { o[x*3+0] = px[2]; o[x*3+1] = px[1]; o[x*3+2] = px[0]; }
        }
    }
    g_orig_unmap(g_ctx, st, 0); st->Release();
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\%ls", g_dir, name);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || f == nullptr) { diag("dump %ls: cannot open", name); return false; }
    const uint32_t data_size = stride * h, file_size = 54 + data_size;
    uint8_t hdr[54] = {};
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &file_size, 4); const uint32_t off = 54; memcpy(hdr + 10, &off, 4);
    const uint32_t ih = 40; memcpy(hdr + 14, &ih, 4);
    const int32_t iw = static_cast<int32_t>(w), ihh = static_cast<int32_t>(h); memcpy(hdr + 18, &iw, 4); memcpy(hdr + 22, &ihh, 4);
    const uint16_t planes = 1, bpp = 24; memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &data_size, 4);
    fwrite(hdr, 1, 54, f); fwrite(out.data(), 1, out.size(), f); fclose(f);
    diag("dump %ls: %ux%u written", name, w, h);
    return true;
}

// A snapshot of the GAME's scene texture at the same moment as eye 2's, so
// the two can be compared.
extern "C" __declspec(dllexport) int ffxvr_measure() {
    diag("measure (eye separation %.2f): eye-2 uploads %u (identical to eye 1: %u), re-entered UpdateSubresource calls %u, red clears %u, twins %u",
         g_ipd, g_e2_uploads, g_e2_identical, g_e2_reentered_updates, g_red_clears, g_twin_count);
    diag("    draw contexts: on the hooked immediate context %u, on another context %u (last %p); hooked context %p", g_draw_ctx_same, g_draw_ctx_other, g_draw_ctx_last_other, static_cast<void*>(g_ctx));
    {
        char line[256]; size_t l = 0;
        for (int sl = 0; sl < 14; ++sl) l += snprintf(line + l, sizeof(line) - l, " s%d:%u", sl, g_slot_binds[sl]);
        diag("    VS constant buffer slot binds:%s", line);
    }
    diag("    resolves %u (mirrored %u), UAV-variant target binds %u, snapshot trigger %s", g_resolves, g_resolves_mirrored, g_omrt_uav_calls,
         g_snap_by == 1 ? "SRV bind" : g_snap_by == 2 ? "copy/resolve" : g_snap_by == 3 ? "clear" : "none");
    for (int k = 0; k < 32; ++k) {
        const EyeImage& e = g_eye_ring[(g_eye_ring_next - 1 - k + 64) % 32];
        if (e.cb == nullptr || e.kind != 1 || e.eye1.size() < 128) continue;
        const float* a = reinterpret_cast<const float*>(e.eye1.data() + CAM_VIEW_OFFSET);
        const float* b = reinterpret_cast<const float*>(e.eye2.data() + CAM_VIEW_OFFSET);
        diag("    latest camera image %u B: eye1 view t=(%.2f %.2f %.2f)  eye2 view t=(%.2f %.2f %.2f)",
             e.size, a[3], a[7], a[11], b[3], b[7], b[11]);
        break;
    }
    double m = 0, fr = 0, ba = 0, bb = 0;
    auto it = g_backbuffer ? g_twins.find(g_backbuffer) : g_twins.end();
    if (it != g_twins.end() && it->second.tex && readback_diff(g_backbuffer, it->second.tex, &m, &fr, &ba, &bb))
        diag("    final: game vs eye-2 twin   mean |diff| %.2f/255, %.1f%% of pixels differ   (brightness %.1f vs %.1f)", m, fr * 100, ba, bb);
    else diag("    final: could not compare");
    {
        D3D11_TEXTURE2D_DESC d = {}; if (g_snap_from) g_snap_from->GetDesc(&d);
        diag("    scene snapshot: %ux%u fmt %u, taken in frame %u (now %u) when the game first read the scene target", d.Width, d.Height, static_cast<unsigned>(d.Format), g_snap_frame, g_frames);
    }
    if (g_snap_tex && g_snap_game && readback_diff(g_snap_game, g_snap_tex, &m, &fr, &ba, &bb))
        diag("    scene: game vs eye-2 twin   mean |diff| %.2f/255, %.1f%% of pixels differ   (brightness %.1f vs %.1f)", m, fr * 100, ba, bb);
    else diag("    scene: could not compare (snapshots %s/%s)", g_snap_game ? "yes" : "no", g_snap_tex ? "yes" : "no");
    {
        auto bt = g_backbuffer ? g_twins.find(g_backbuffer) : g_twins.end();
        dump_bmp(g_snap_game, L"img_scene_eye1.bmp");
        dump_bmp(g_snap_tex,  L"img_scene_eye2.bmp");
        dump_bmp(g_backbuffer, L"img_final_game.bmp");
        if (bt != g_twins.end()) dump_bmp(bt->second.tex, L"img_final_eye2.bmp");
        dump_bmp(g_backbuffer, L"img_final_game_alpha.bmp", true);
        if (bt != g_twins.end()) dump_bmp(bt->second.tex, L"img_final_eye2_alpha.bmp", true);
        dump_bmp(g_snap_tex, L"img_scene_eye2_alpha.bmp", true);
    }
    return 0;
}

extern "C" __declspec(dllexport) void ffxvr_set_backbuffer(void* tex) { g_backbuffer = static_cast<ID3D11Texture2D*>(tex); }

/// Eye 2's final image: the twin of the backbuffer, as a shader resource.
extern "C" __declspec(dllexport) void* ffxvr_eye2_final_srv() {
    if (g_backbuffer == nullptr) return nullptr;
    auto it = g_twins.find(g_backbuffer);
    if (it == g_twins.end() || it->second.rtv == nullptr) return nullptr;
    Twin& t = it->second;
    auto sit = t.srvs.find(nullptr);
    if (sit != t.srvs.end()) return sit->second;
    ID3D11ShaderResourceView* nv = nullptr;
    g_device->CreateShaderResourceView(t.tex, nullptr, &nv);
    t.srvs[nullptr] = nv;
    return nv;
}

extern "C" __declspec(dllexport) void  ffxvr_stereo_set(int on, float ipd, int scope, int red, int leave) { g_stereo = on != 0; g_ipd = ipd; g_stereo_scope = scope; g_debug_red = red != 0; g_debug_leave = leave != 0; }
/// A twin's plain SRV (whole texture), created on demand; null if none.
static void* twin_plain_srv(ID3D11Texture2D* game) {
    if (game == nullptr) return nullptr;
    auto it = g_twins.find(game);
    if (it == g_twins.end() || it->second.tex == nullptr) return nullptr;
    Twin& t = it->second;
    auto sit = t.srvs.find(nullptr);
    if (sit != t.srvs.end()) return sit->second;
    ID3D11ShaderResourceView* nv = nullptr;
    g_device->CreateShaderResourceView(t.tex, nullptr, &nv);
    t.srvs[nullptr] = nv;
    return nv;
}

// ---------------------------------------------------------------------------
// Opaque copy pass: colour of a texture with alpha forced to 1. The overlay
// draws its preview with alpha blending, and the game leaves alpha at 0 over
// the backdrop and inside the save sphere's refraction box, so the raw
// texture shows the window background through those regions.

typedef HRESULT (WINAPI* PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
static const char k_copy_hlsl[] =
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID) { V o; float2 uv = float2((id << 1) & 2, id & 2); o.uv = uv; o.p = float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1); return o; }\n"
    "float4 ps(V i) : SV_Target { return float4(t.Sample(s, i.uv).rgb, 1); }\n";
ID3D11VertexShader* g_copy_vs = nullptr; ID3D11PixelShader* g_copy_ps = nullptr; ID3D11SamplerState* g_copy_smp = nullptr;
bool g_copy_tried = false;
struct OpaqueCopy { ID3D11Texture2D* tex = nullptr; ID3D11RenderTargetView* rtv = nullptr; ID3D11ShaderResourceView* srv = nullptr; UINT w = 0, h = 0; };
OpaqueCopy g_opaque[3];

static bool copy_pass_init() {
    if (g_copy_tried) return g_copy_vs != nullptr;
    g_copy_tried = true;
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!m) { diag("copy pass: d3dcompiler_47.dll not found"); return false; }
    PFN_D3DCompile compile = reinterpret_cast<PFN_D3DCompile>(GetProcAddress(m, "D3DCompile"));
    if (!compile) { diag("copy pass: D3DCompile missing"); return false; }
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    HRESULT hr = compile(k_copy_hlsl, sizeof(k_copy_hlsl) - 1, "copy", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err);
    if (FAILED(hr)) { diag("copy pass: vs compile failed 0x%08X: %s", static_cast<unsigned>(hr), err ? static_cast<const char*>(err->GetBufferPointer()) : ""); if (err) err->Release(); return false; }
    if (err) { err->Release(); err = nullptr; }
    hr = compile(k_copy_hlsl, sizeof(k_copy_hlsl) - 1, "copy", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err);
    if (FAILED(hr)) { diag("copy pass: ps compile failed 0x%08X: %s", static_cast<unsigned>(hr), err ? static_cast<const char*>(err->GetBufferPointer()) : ""); if (err) err->Release(); vsb->Release(); return false; }
    if (err) err->Release();
    hr = g_device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_copy_vs);
    if (SUCCEEDED(hr)) hr = g_device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_copy_ps);
    vsb->Release(); psb->Release();
    if (FAILED(hr)) { diag("copy pass: shader create failed 0x%08X", static_cast<unsigned>(hr)); return false; }
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_device->CreateSamplerState(&sd, &g_copy_smp);
    diag("copy pass: ready");
    return true;
}

static std::map<ID3D11Texture2D*, ID3D11ShaderResourceView*> g_plain_srvs;   // per source texture
bool ffxvr_copy_pass(ID3D11Texture2D* src, ID3D11RenderTargetView* dst, UINT w, UINT h);

/// Renders `src` into the opaque copy for `slot` (0..2) and returns its SRV.
static ID3D11ShaderResourceView* opaque_copy(int slot, ID3D11Texture2D* src) {
    if (src == nullptr || g_device == nullptr || g_ctx == nullptr || slot < 0 || slot > 2) return nullptr;
    D3D11_TEXTURE2D_DESC d; src->GetDesc(&d);
    if (d.SampleDesc.Count != 1) return nullptr;
    OpaqueCopy& oc = g_opaque[slot];
    if (oc.tex == nullptr || oc.w != d.Width || oc.h != d.Height) {
        if (oc.srv) oc.srv->Release(); if (oc.rtv) oc.rtv->Release(); if (oc.tex) oc.tex->Release();
        oc = OpaqueCopy{};
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = d.Width; td.Height = d.Height; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = g_device->CreateTexture2D(&td, nullptr, &oc.tex);
        if (FAILED(hr)) {
            static bool reported = false;
            if (!reported) {
                reported = true;
                diag("preview: texture %ux%u failed hr=0x%08X", d.Width, d.Height, static_cast<unsigned>(hr));
                if (hr == DXGI_ERROR_DEVICE_REMOVED) diag("DEVICE REMOVED, reason 0x%08X (hung 0x887A0006, reset 0x887A0007, internal 0x887A0020, invalid call 0x887A0001)", static_cast<unsigned>(g_device->GetDeviceRemovedReason()));
            }
            oc.tex = nullptr; return nullptr;
        }
        hr = g_device->CreateRenderTargetView(oc.tex, nullptr, &oc.rtv);
        if (FAILED(hr)) diag("preview: rtv failed hr=0x%08X", static_cast<unsigned>(hr));
        hr = g_device->CreateShaderResourceView(oc.tex, nullptr, &oc.srv);
        if (FAILED(hr)) diag("preview: srv failed hr=0x%08X", static_cast<unsigned>(hr));
        oc.w = d.Width; oc.h = d.Height;
    }
    return ffxvr_copy_pass(src, oc.rtv, oc.w, oc.h) ? oc.srv : nullptr;
}

/// Stretch-copies `src` into `dst` (w x h) with alpha forced to 1, saving
/// and restoring the pipeline state it touches. Also used for the headset.
bool ffxvr_copy_pass(ID3D11Texture2D* src, ID3D11RenderTargetView* dst, UINT w, UINT h) {
    if (src == nullptr || dst == nullptr || g_device == nullptr || g_ctx == nullptr) return false;
    if (!copy_pass_init()) return false;
    ID3D11ShaderResourceView* in = nullptr;
    auto it = g_plain_srvs.find(src);
    if (it != g_plain_srvs.end()) in = it->second;
    else {
        D3D11_TEXTURE2D_DESC sd; src->GetDesc(&sd);
        if (!(sd.BindFlags & D3D11_BIND_SHADER_RESOURCE) || sd.SampleDesc.Count != 1) {
            g_plain_srvs[src] = nullptr;                // remembered: never asked again
            diag("copy pass: source %p (%ux%u bind 0x%X samples %u) cannot be sampled", static_cast<void*>(src), sd.Width, sd.Height, sd.BindFlags, sd.SampleDesc.Count);
            return false;
        }
        if (FAILED(g_device->CreateShaderResourceView(src, nullptr, &in))) { g_plain_srvs[src] = nullptr; return false; }
        g_plain_srvs[src] = in;
    }
    if (in == nullptr) return false;

    // Save the pipeline state we touch, run the pass, restore.
    ID3D11RenderTargetView* o_rtv[4] = {}; ID3D11DepthStencilView* o_dsv = nullptr;
    g_ctx->OMGetRenderTargets(4, o_rtv, &o_dsv);
    UINT nvp = 1; D3D11_VIEWPORT o_vp = {}; g_ctx->RSGetViewports(&nvp, &o_vp);
    ID3D11VertexShader* o_vs = nullptr; ID3D11PixelShader* o_ps = nullptr; ID3D11GeometryShader* o_gs = nullptr;
    g_ctx->VSGetShader(&o_vs, nullptr, nullptr); g_ctx->PSGetShader(&o_ps, nullptr, nullptr); g_ctx->GSGetShader(&o_gs, nullptr, nullptr);
    ID3D11ShaderResourceView* o_srv = nullptr; g_ctx->PSGetShaderResources(0, 1, &o_srv);
    ID3D11SamplerState* o_smp = nullptr; g_ctx->PSGetSamplers(0, 1, &o_smp);
    ID3D11BlendState* o_bs = nullptr; float o_bf[4]; UINT o_mask; g_ctx->OMGetBlendState(&o_bs, o_bf, &o_mask);
    ID3D11DepthStencilState* o_ds = nullptr; UINT o_ref; g_ctx->OMGetDepthStencilState(&o_ds, &o_ref);
    ID3D11RasterizerState* o_rs = nullptr; g_ctx->RSGetState(&o_rs);
    ID3D11InputLayout* o_il = nullptr; g_ctx->IAGetInputLayout(&o_il);
    D3D11_PRIMITIVE_TOPOLOGY o_topo; g_ctx->IAGetPrimitiveTopology(&o_topo);

    g_in_eye2 = true;                                  // our hooks ignore this pass, restore included
    ID3D11RenderTargetView* rt[1] = { dst };
    g_ctx->OMSetRenderTargets(1, rt, nullptr);
    D3D11_VIEWPORT vp = {}; vp.Width = static_cast<float>(w); vp.Height = static_cast<float>(h); vp.MaxDepth = 1.f;
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->VSSetShader(g_copy_vs, nullptr, 0); g_ctx->PSSetShader(g_copy_ps, nullptr, 0); g_ctx->GSSetShader(nullptr, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &in); g_ctx->PSSetSamplers(0, 1, &g_copy_smp);
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF); g_ctx->OMSetDepthStencilState(nullptr, 0); g_ctx->RSSetState(nullptr);
    g_ctx->IASetInputLayout(nullptr); g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->Draw(3, 0);

    g_ctx->OMSetRenderTargets(4, o_rtv, o_dsv);
    if (nvp) g_ctx->RSSetViewports(1, &o_vp);
    g_ctx->VSSetShader(o_vs, nullptr, 0); g_ctx->PSSetShader(o_ps, nullptr, 0); g_ctx->GSSetShader(o_gs, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &o_srv); g_ctx->PSSetSamplers(0, 1, &o_smp);
    g_ctx->OMSetBlendState(o_bs, o_bf, o_mask); g_ctx->OMSetDepthStencilState(o_ds, o_ref); g_ctx->RSSetState(o_rs);
    g_ctx->IASetInputLayout(o_il); g_ctx->IASetPrimitiveTopology(o_topo);
    g_in_eye2 = false;
    for (auto* v : o_rtv) if (v) v->Release();
    if (o_dsv) o_dsv->Release(); if (o_vs) o_vs->Release(); if (o_ps) o_ps->Release(); if (o_gs) o_gs->Release();
    if (o_srv) o_srv->Release(); if (o_smp) o_smp->Release(); if (o_bs) o_bs->Release(); if (o_ds) o_ds->Release();
    if (o_rs) o_rs->Release(); if (o_il) o_il->Release();
    return true;
}

/// source 0: eye 2's final image (backbuffer twin); 1: the scene target's twin.
extern "C" __declspec(dllexport) void* ffxvr_eye2_srv(int source) {
    ID3D11Texture2D* src = nullptr;
    if (source == 1) src = g_snap_tex;
    else if (source == 2) src = g_snap_game;
    else if (g_backbuffer) { auto it = g_twins.find(g_backbuffer); if (it != g_twins.end()) src = it->second.tex; }
    ID3D11ShaderResourceView* v = opaque_copy(source, src);   // alpha forced to 1 for the overlay
    if (v) return v;
    if (source == 1) return g_snap_srv;
    if (source == 2) return g_snap_game_srv;
    return ffxvr_eye2_final_srv();
}
extern "C" __declspec(dllexport) int   ffxvr_eye2_draws() { return static_cast<int>(g_eye2_draws_report); }
extern "C" __declspec(dllexport) int   ffxvr_eye2_size(int* w, int* h, int source) {
    if (source == 1 || source == 2) {
        if (g_snap_tex == nullptr) return 0;
        D3D11_TEXTURE2D_DESC d; g_snap_tex->GetDesc(&d);
        if (w) *w = static_cast<int>(d.Width); if (h) *h = static_cast<int>(d.Height);
        return 1;
    }
    ID3D11Texture2D* key = g_backbuffer;
    if (key == nullptr) return 0;
    auto it = g_twins.find(key);
    if (it == g_twins.end() || it->second.tex == nullptr) return 0;
    if (w) *w = static_cast<int>(it->second.w); if (h) *h = static_cast<int>(it->second.h);
    return 1;
}

// ---------------------------------------------------------------- headset
int g_seq_left = 0, g_seq_index = 0;          // consecutive-frame image dump (F3)
uint32_t g_e2_begin_fail_prev = 0;
static void xr_diag(const char* fmt, ...) { va_list a; va_start(a, fmt); char b[512]; vsnprintf(b, sizeof(b), fmt, a); va_end(a); diag("%s", b); }

extern "C" __declspec(dllexport) int ffxvr_xr_set(int on, float scale, int rotate, int translate, int use_fov, int seat) {
    g_xr_scale = scale; g_xr_rotate = rotate != 0; g_xr_translate = translate != 0; g_xr_use_fov = use_fov != 0; g_xr_seat = seat != 0;
    if (on && !g_xr_want) {
        XrHost h = { g_device, g_ctx, g_dir, xr_diag, ffxvr_copy_pass };
        g_xr_want = xr_start(h);
        g_xr_recenter_pending = true;
        return g_xr_want ? 1 : 0;
    }
    if (!on && g_xr_want) { xr_stop(); g_xr_want = false; g_xr_active = false; }
    return g_xr_want ? 1 : 0;
}
extern "C" __declspec(dllexport) void ffxvr_xr_recenter() { g_xr_recenter_pending = true; }
extern "C" __declspec(dllexport) void ffxvr_dump_seq(int n) { g_seq_left = n; g_seq_index = 0; g_e2_begin_fail_prev = g_e2_begin_fail; }
extern "C" __declspec(dllexport) void ffxvr_shutdown() { if (g_xr_want) { xr_stop(); g_xr_want = false; g_xr_active = false; } }
extern "C" __declspec(dllexport) void ffxvr_xr_status(char* buf, int n) {
    if (buf == nullptr || n <= 0) return;
    const float fwd[3] = { 0, 0, -1 }; float d[3]; quat_rotate(g_xr_q, fwd, d);
    snprintf(buf, static_cast<size_t>(n), "%s%s  yaw %.0f pitch %.0f  pos (%.2f %.2f %.2f) m  eye sep %.2f m = %.1f units  fov L %.0f..%.0f U %.0f..%.0f deg",
             xr_status(), g_xr_active ? " [tracking]" : "",
             atan2f(-d[0], -d[2]) * 57.2958f, asinf(fmaxf(-1.f, fminf(1.f, d[1]))) * 57.2958f,
             g_xr_p[0], g_xr_p[1], g_xr_p[2],
             sqrtf(g_xr_eye_off[0]*g_xr_eye_off[0] + g_xr_eye_off[1]*g_xr_eye_off[1] + g_xr_eye_off[2]*g_xr_eye_off[2]),
             sqrtf(g_xr_eye_off[0]*g_xr_eye_off[0] + g_xr_eye_off[1]*g_xr_eye_off[1] + g_xr_eye_off[2]*g_xr_eye_off[2]) * g_xr_scale,
             atanf(g_xr_fov[0].l) * 57.2958f, atanf(g_xr_fov[0].r) * 57.2958f, atanf(g_xr_fov[0].d) * 57.2958f, atanf(g_xr_fov[0].u) * 57.2958f);
}

/// Called once per frame from the Present hook, with the finished frame in
/// the backbuffer (eye 1) and its twin (eye 2).
ID3D11Texture2D* g_bb_copy = nullptr;        // the backbuffer, copied into a texture that can be sampled
static void xr_tick() {
    if (!g_xr_want) return;
    ID3D11Texture2D* e1 = nullptr;
    ID3D11Texture2D* e2 = nullptr;
    if (g_backbuffer) {
        auto it = g_twins.find(g_backbuffer); if (it != g_twins.end()) e2 = it->second.tex;
        D3D11_TEXTURE2D_DESC bd; g_backbuffer->GetDesc(&bd);
        if (g_bb_copy) {
            D3D11_TEXTURE2D_DESC cd; g_bb_copy->GetDesc(&cd);
            if (cd.Width != bd.Width || cd.Height != bd.Height || cd.Format != bd.Format) { g_bb_copy->Release(); g_bb_copy = nullptr; }
        }
        if (g_bb_copy == nullptr) {
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = bd.Width; td.Height = bd.Height; td.MipLevels = 1; td.ArraySize = 1; td.Format = bd.Format;
            td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(g_device->CreateTexture2D(&td, nullptr, &g_bb_copy))) g_bb_copy = nullptr;
        }
        if (g_bb_copy) { g_in_eye2 = true; g_ctx->CopyResource(g_bb_copy, g_backbuffer); g_in_eye2 = false; e1 = g_bb_copy; }
    }
    const float* P = &g_cam[16];
    const float tx = (P[0] != 0.f) ? 1.f / fabsf(P[0]) : 1.f;
    const float ty = (P[5] != 0.f) ? 1.f / fabsf(P[5]) : 0.5625f;
    // The field of view the frame just finished was rendered with.
    XrFovTan used[2];
    if (g_xr_fov_applied) { used[0] = g_xr_fov_used[0]; used[1] = g_xr_fov_used[1]; }
    else { used[0] = used[1] = XrFovTan{ -tx, tx, ty, -ty }; }
    XrFrameOut out = {};
    if (e1 == nullptr) return;
    g_xr_fov_applied = false;
    if (g_seq_left > 0) {
        wchar_t n1[64], n2[64];
        _snwprintf_s(n1, _TRUNCATE, L"seq_%02d_eye1.bmp", g_seq_index); _snwprintf_s(n2, _TRUNCATE, L"seq_%02d_eye2.bmp", g_seq_index);
        dump_bmp(e1, n1); if (e2) dump_bmp(e2, n2);
        diag("seq %02d: frame %u, eye-2 draws %u, eye-2 begin failures %u, uploads so far %u", g_seq_index, g_frames, g_eye2_draws, g_e2_begin_fail - g_e2_begin_fail_prev, g_e2_uploads);
        g_e2_begin_fail_prev = g_e2_begin_fail;
        g_seq_index++; g_seq_left--;
    }
    xr_frame(e1, g_stereo ? e2 : nullptr, used, &out);
    xr_apply_poses(out);
    // What the next frame renders with.
    if (g_xr_active && g_xr_use_fov) { g_xr_fov_used[0] = g_xr_fov[0]; g_xr_fov_used[1] = g_xr_fov[1]; }
    else { g_xr_fov_used[0] = g_xr_fov_used[1] = XrFovTan{ -tx, tx, ty, -ty }; }
    if (!xr_active()) { g_xr_want = false; g_xr_active = false; }
}

extern "C" __declspec(dllexport) int ffxvr_diff(int* out) {
    if (out == nullptr) return 0;
    out[0] = g_diff_count;
    out[1] = static_cast<int>(g_full.size());
    out[2] = g_draws_latched_at;
    for (int i = 0; i < 8; ++i) out[3 + i] = static_cast<int>(g_diff_offs[i]);
    return g_diff_count;
}

/// Reads one hook counter by index, so the overlay can show whether a specific
/// draw path fires at all rather than only the aggregate.
extern "C" __declspec(dllexport) int ffxvr_counter(int i) {
    if (i < 0 || i >= C_COUNT) return -1;
    return static_cast<int>(g_counters[i]);
}

/// Fills out[] with kScanMax * 6 floats -- offset, buffer size, write path
/// (0 CreateBuffer, 1 Map/Unmap, 2 UpdateSubresource), then x, y, z of the
/// camera position each candidate implies -- and returns the hit count.
extern "C" __declspec(dllexport) int ffxvr_scan(float* out) {
    if (out == nullptr) return 0;

    std::lock_guard<std::mutex> lock(g_mutex);
    for (int i = 0; i < kScanMax; ++i) {
        out[i*6 + 0] = static_cast<float>(g_scan_report[i].offset);
        out[i*6 + 1] = static_cast<float>(g_scan_report[i].size);
        out[i*6 + 2] = static_cast<float>(g_scan_report[i].via);
        out[i*6 + 3] = g_scan_report[i].pos[0];
        out[i*6 + 4] = g_scan_report[i].pos[1];
        out[i*6 + 5] = g_scan_report[i].pos[2];
    }
    return g_scan_report_n;
}

extern "C" __declspec(dllexport) int ffxvr_frame_writes() {
    return static_cast<int>(g_frame_writes);
}

extern "C" __declspec(dllexport) int ffxvr_buffer_count() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return static_cast<int>(g_buffers.size());
}

extern "C" __declspec(dllexport) int ffxvr_dump_count() { return g_dump_index; }

// Total hooked calls observed, so the overlay can show whether anything fires.
extern "C" __declspec(dllexport) int ffxvr_hook_calls() {
    long total = 0;
    for (int i = 0; i < C_CB_CAPTURED; ++i) total += g_counters[i];
    return static_cast<int>(total);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        GetModuleFileNameW(module, g_dir, MAX_PATH);
        wchar_t* slash = wcsrchr(g_dir, L'\\');
        if (slash) *slash = L'\0';
    }
    return TRUE;
}
