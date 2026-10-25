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
// Ctrl+F9 writes a full dump of every constant buffer seen.

#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>   // ID3D11Multithread
#include <dxgi1_4.h>    // IDXGIAdapter3: video memory readout
#include <psapi.h>
#include <d3dcompiler.h>
#include <Xinput.h>
#include "ffxvr_xr.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <atomic>
#include <mutex>
#include <vector>

// Defined with the eye-2 twin code near the end; used from several places.
static ID3D11Texture2D* texture_of(ID3D11View* view);   // twins, below

#pragma comment(lib, "d3d11.lib")

static void diag(const char* fmt, ...);   // ffxvr_diag.log, defined below
static void g_plain_srvs_forget(ID3D11Texture2D* t);   // SRV cache, defined below
static uint32_t g_plain_srvs_count();

static void gpu_timing_start_span();   // GPU-time span starts at the game's first clear (defined with perf_tick)
static void gpu_timing_end_span();     // ... and closes at Present, before the runtime wait
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
using PFN_Map                  = HRESULT (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using PFN_Unmap                = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using PFN_CreateBuffer         = HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);

PFN_VSSetConstantBuffers g_orig_vssetcb        = nullptr;
PFN_Map                  g_orig_map            = nullptr;
PFN_Unmap                g_orig_unmap          = nullptr;
PFN_CreateBuffer         g_orig_createbuffer   = nullptr;
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateTexture2D)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
PFN_CreateTexture2D      g_orig_createtex2d    = nullptr;
// MSAA override (VR): the game's scene colour/depth targets are created at
// 8 samples (GameSetting.ini's MSAA key is ignored on the Fahrenheit launch
// path). When the game (re)creates a large multisampled texture, the sample
// count is replaced by g_msaa_override (2 or 4; 0 = leave alone). Never 1: the
// game's ResolveSubresource calls need a multisampled source.
int      g_msaa_override = 0;
uint32_t g_msaa_seen = 0, g_msaa_changed = 0;   // large MS textures created since init / of those, overridden
uint32_t g_msaa_last_w = 0, g_msaa_last_h = 0, g_msaa_last_n = 0;

// ---------------------------------------------------------------- state

struct CbRecord {
    UINT                 size    = 0;
    uint64_t             updates = 0;
    uint32_t             frame   = 0;      // last update (g_frames), for pruning
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
// Diagnostics that cost frame time (the slot-0 readback, the 1 s heartbeat) run only while armed: the first 1800
// frames, and while the overlay's Debug header is expanded (managed calls ffxvr_diag_keepalive every such frame).
// The captures (pick, frame census, cb dump, measure, sequence) do not need it: they read what is always kept.
uint32_t           g_diag_until    = 1800;
static inline bool diag_armed() { return g_frames < g_diag_until; }
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
DWORD                g_render_tid   = 0;          // the thread that runs the per-frame hook
uint32_t             g_other_thread_calls = 0;   // hook entries from other threads
DWORD                g_other_thread_last = 0;
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
// The slot-0 readback (diagnostic: the heartbeat's "changed since previous frame"): a ring of staging copies tagged
// with their frame, mapped without waiting, so the CPU never stalls on the GPU.
struct CamRb { ID3D11Buffer* st = nullptr; UINT size = 0; uint32_t frame = 0; bool pending = false; };
CamRb    g_cam_rb[5];   // deeper than DXGI's default 3-frame latency, so a GPU-bound frame never leaves no free entry
uint32_t g_cam_last_collected = 0;

float    g_cam[48] = {};     // view, projection, inverse view
float    g_cam_last_view[12] = {};
uint32_t g_cam_changes = 0;  // frames in which the view matrix differed from the previous one

// Whole-buffer readback of the latched slot-0 buffer, and a diff against the
// previous frame, so we can see which bytes move while walking even if none of
// them is a view matrix we recognise.
std::vector<uint8_t> g_full, g_full_prev;
int      g_diff_count   = 0;         // floats that changed since last frame; -1: n/a (the two copies are not consecutive frames)
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

// Vertex or index buffer (identity key only, like as_constant_buffer).
bool as_geometry_buffer(ID3D11Resource* res, ID3D11Buffer** out_buf, UINT* out_size, UINT* out_bind) {
    if (res == nullptr) return false;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN; res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
    ID3D11Buffer* buf = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buf))) || buf == nullptr) return false;
    D3D11_BUFFER_DESC desc = {}; buf->GetDesc(&desc); buf->Release();
    if ((desc.BindFlags & (D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER)) == 0) return false;
    *out_buf = buf; *out_size = desc.ByteWidth; *out_bind = desc.BindFlags;
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

thread_local std::vector<uint8_t> g_scratch;   // per thread: the pointer we hand back is used after the lock is released, and the loader thread uploads too

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
bool  g_in_eye2      = false;         // re-entrancy guard: our own GPU work, and everything inside the OpenXR runtime
struct InEye2 {                        // scoped: sets the guard, restores the previous value (nesting-safe)
    bool prev; InEye2() : prev(g_in_eye2) { g_in_eye2 = true; } ~InEye2() { g_in_eye2 = prev; }
};
// Recent camera-carrying uploads, with both eyes' images, keyed by buffer.
// The game uploads the next object's buffer before drawing the current one,
// so the image for a draw is usually not the most recent upload.
struct EyeImage { ID3D11Buffer* cb; UINT size; std::vector<uint8_t> eye1, eye2, eye1b, eye2b; uint32_t frame; int kind; bool has_world; float world_pos[3]; bool pal_hands, split; };   // pal_hands: this part carries hand bones; split: drawn twice per eye (eye1/eye2 = left hand, eye1b/eye2b = right hand)
EyeImage g_eye_ring[32];
int      g_eye_ring_next = 0;
/// The next ring slot, with every field from its previous use reset (R14): a slot that last held a split hand
/// record must not ask for 3 passes or upload its old eye1b/eye2b for a different buffer.
static EyeImage& ring_take(ID3D11Buffer* buf, UINT size, int kind) {
    EyeImage& e = g_eye_ring[g_eye_ring_next]; g_eye_ring_next = (g_eye_ring_next + 1) % 32;
    e.cb = buf; e.size = size; e.frame = g_frames; e.kind = kind;
    e.has_world = false; e.pal_hands = false; e.split = false;
    e.eye1b.clear(); e.eye2b.clear();               // clear() keeps the capacity: no reallocation
    return e;
}
/// A split (two-hand) record whose variants really are this buffer's size: only then 3 passes / the eye1b, eye2b uploads.
static bool split_ok(const EyeImage* e) { return e != nullptr && e->split && e->eye1b.size() == e->size && e->eye2b.size() == e->size; }
EyeImage* g_eye2_cur = nullptr;       // the entry in use for the current eye-2 draw
bool      g_split_pass = false;       // the current extra pass is the right-hand variant into eye 1 (targets unchanged)
EyeImage* g_eye2_slots[14] = {};      // entries uploaded for each VS slot in the current eye-2 draw
uint32_t g_eye2_draws = 0, g_eye2_draws_report = 0;
// R23 step 1 (a measurement only): eye-2 re-issues that swap no input texture and upload no eye-2 constant buffer.
// An UPPER BOUND of the draws whose eye-2 output could equal eye 1's (their targets may still hold different content).
uint32_t g_e2_same_inputs = 0, g_e2_same_inputs_report = 0;
// why a draw did not get an eye-2 pass (cumulative)
uint32_t g_e2_no_stereo = 0, g_e2_no_image = 0, g_e2_cb_mismatch = 0, g_e2_no_dsv = 0, g_e2_small = 0, g_e2_ok = 0, g_e2_begin_fail = 0;
uint32_t g_e2_no_depth_twin = 0;   // R12: eye-2 draws whose depth target has no twin (drawn without depth, as before; counted only)
bool     g_twin_srv_trim = false;  // R12: a twin's SRV cache reached 256: trimmed at the frame boundary (twin_srv_trim), never mid-draw
// Twins: for every texture the game renders into, an identical texture of
// ours. In eye-2 mode a draw's targets are swapped for their twins, and any
// sampled texture that has a twin is swapped too, so post-processing,
// reflections and the final composite all happen for eye 2 as well.
struct Twin {
    ID3D11Texture2D* game; ID3D11Texture2D* tex;
    ID3D11RenderTargetView* rtv; ID3D11DepthStencilView* dsv;
    std::map<ID3D11ShaderResourceView*, ID3D11ShaderResourceView*> srvs;   // game SRV -> twin SRV
    UINT w, h; DXGI_FORMAT fmt; UINT samples, mips, bind;                 // the game texture's description the twin was made for
    uint32_t last_used;                                                   // g_frames at the last lookup
    uint32_t fail_frame, retry_after;                                     // a failed twin (tex null): when it failed, and when to try again (frames)
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
// Scene snapshots cost two full-resolution copies per stereo frame, so they are taken only on demand: the eye-2
// preview (sources 1, 2) and Measure ask for them; snapshots unused for 600 frames are freed in sweep_caches.
uint32_t g_snap_until = 0;                            // snapshots wanted while g_frames < this (only ever raised: snap_request)
bool     g_measure_pending = false; uint32_t g_measure_deadline = 0;   // Measure waits for a fresh snapshot (end of begin_frame)
static inline bool snap_wanted() { return g_frames < g_snap_until || g_measure_pending; }
static inline void snap_request(uint32_t frames) { const uint32_t u = g_frames + frames; if (u > g_snap_until) g_snap_until = u; }
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
constexpr int k_world_max = 128;
float g_world_live[k_world_max][3];   int g_world_live_n = 0;
float g_world_report[k_world_max][3]; int g_world_report_n = 0; uint32_t g_world_report_frame = 0;   // frame the report was last refreshed
static void census_write(UINT size);   // defined with the census code below


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
static float same4_tol_of(const float* b) {
    float scale = 1.f;
    for (int i = 0; i < 16; ++i) scale = (std::max)(scale, fabsf(b[i]));
    return 0.01f * scale;
}
static bool same4_tol(const float* a, const float* b, float tol) {   // same4 with b's tolerance computed once by the caller
    for (int i = 0; i < 16; ++i) if (fabsf(a[i] - b[i]) > tol) return false;
    return true;
}
static bool same4(const float* a, const float* b) { return same4_tol(a, b, same4_tol_of(b)); }

/// Builds the override view V' (4x4) and its inverse from a base 3x4 view.
// Head tracking (OpenXR). The located left-eye pose, relative to the
// recentre reference, is applied on top of the manual override: rotation
// in view space (the game's view axes match OpenXR's: x right, y up, -z
// forward), translation along the base camera's axes scaled by the world
// scale (game units per metre). Eye 2 adds the right-eye offset.
bool  g_xr_want     = false;       // the user asked for the headset
ID3D11Texture2D* g_bb_copy = nullptr;        // eye 1: the backbuffer copied at the hook, then the overlay drawn on top
ID3D11RenderTargetView* g_bb_copy_rtv = nullptr;

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
float g_xr_pitch_follow = 0.85f;        // seat: how much of the authored pitch stays in the reference frame (0 level .. 1 the original view)
// Seat heading latch: the reference frame is taken from the authored camera
// once and kept, so scripted pans do not turn the world; a cut re-latches
// (and recentres on the head), optionally with a blink. "Follow pans" slews
// the latched heading toward the live one slowly.
bool  g_seat_latched = false;
float g_seat_yaw = 0.f, g_seat_pitch = 0.f;   // latched authored heading (radians; yaw about +y, pitch + looks up)
bool  g_cut_recentre = true, g_cut_blink = false, g_follow_pans = false;
float g_follow_rate = 20.f;                    // deg/s
uint32_t g_cut_count = 0; int g_blink_frames = 0;
float g_cam_prev_pos[3] = {}, g_cam_prev_fwd[3] = {}; bool g_cam_prev_valid = false;
float g_dt = 1.f / 72.f;                      // the last Present-to-Present interval, measured at the top of ffxvr_begin_frame
constexpr float k_dt_min = 0.001f, k_dt_max = 0.05f;   // clamp: slow motion and load stalls must not make the filters jump
bool  g_dt_measured = false;                   // once true, the managed dt passed to ffxvr_cuts_set is ignored

// Turning: the right stick (XInput, read directly) and the turn keys rotate
// the latched heading, in first person and in the seat alike. Snap turns are
// a fixed angle per flick (with a blink); smooth turns are degrees per second.
bool  g_turn_stick = true; int g_turn_mode = 0; float g_turn_snap = 30.f, g_turn_rate = 90.f; bool g_turn_blink = true;
uint32_t g_turn_count = 0;

// Movement remap: in first person the game still maps the left stick against
// its own camera, so the stick is rotated by (view heading - game camera
// heading) inside the game's XInputGetState import. Pure IAT hook.
bool  g_move_remap = true; float g_move_sign = 1.f; float g_move_delta = 0.f;   // radians
float g_move_delta_hold = 0.f; bool g_move_held = false;   // third person: the remap latched while the stick is pushed (see the hook)
float g_yaw_view = 0.f, g_yaw_cam = 0.f;
uint32_t g_xi_hook_calls = 0; bool g_xi_hooked = false; uint32_t g_xi_hook_calls_all = 0;   // _all: every call of the game's, pad or not (heartbeat)
typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD, XINPUT_STATE*);
PFN_XInputGetState g_xi_get = nullptr, g_xi_orig = nullptr; bool g_xi_tried = false; int g_xi_pad = -1; ULONGLONG g_xi_probe = 0; DWORD g_xi_probe_slot = 0;
// What the game's own XInputGetState calls returned, per pad slot (recorded by the import hook, before the VR pad is
// merged in): while the game polls, stick turning uses this and makes no XInput calls of its own. Written on the
// game's input thread, read on the render thread: aligned 32-bit fields, a torn read is harmless.
struct XiSeen { DWORD res; LONG rx; DWORD tick; };
XiSeen g_xi_seen[4] = {};
// Build 7 diagnostics, log only (design-notes "Build 7: battle-cursor and engine diagnostics (log only)"), on with
// ffxvr_diag7_set (ini diag_build7, default 1): the game's XInputSetState calls (6.11), the loading-stall watchdog (6.5),
// the backbuffer desc (6.7), extra Motion-log columns (6.4). Off: none of it runs and XInputSetState is not hooked.
bool g_diag7 = false;
typedef DWORD (WINAPI *PFN_XInputSetState)(DWORD, XINPUT_VIBRATION*);
PFN_XInputSetState g_xis_orig = nullptr; bool g_xis_hooked = false; uint32_t g_xis_calls = 0, g_xis_nonzero = 0;
// Motor changes, written by the game's thread (single writer), logged at begin_frame: 20 per arm (battle start re-arms).
struct XisRec { uint32_t frame, pad; uint16_t l, r; };
XisRec g_xis_log[32] = {}; volatile LONG g_xis_w = 0; LONG g_xis_r = 0, g_xis_until = 20; uint16_t g_xis_last[4][2] = {};
uint32_t g_bb_desc_logged = 0;                             // backbuffer desc lines (6.7), at most 10
float g_motion_cols[12] = {}; bool g_motion_cols_fresh = false;   // managed's post-join snapshot columns for the Motion log (6.4)
volatile LONG g_present_ms = 0;                            // GetTickCount at the last begin_frame: the watchdog's input
volatile LONG g_stall_open = 0; LONG g_stall_from_ms = 0; uint32_t g_stalls = 0;   // a stall logged and not yet ended; stalls logged
DWORD g_main_tid = 0; uint32_t g_main_other = 0;           // the game's main thread (first caller of ffxvr_main_note); joins seen elsewhere
HANDLE g_wd_thread = nullptr, g_wd_stop = nullptr;
// First person / follow: the view is built from the player's position, not
// the authored camera. Heading latched at toggle (and at recentre); the
// head owns orientation; the player's own skinned draws are hidden.
int   g_fp_mode = 0;                           // 0 off, 1 first person, 2 diorama (follow), 3 third person
float g_fp_pos[3] = {};                        // player position, render space
// Motion-smoothness log: for a couple of seconds, one line per drawn frame with the player's position as
// read from the actor table, the nearest rendered skinned position (what the fps mod's interpolation puts
// on screen), the view yaw and the head yaw - to see which of them step at the 30 Hz logic rate.
int g_motion_log_left = 0;
bool  g_fp_have_pos = false;
float g_fp_eye_h = 18.f;                       // eye height above the actor origin (units), when not anchored to the head
// Eye anchored to the head bone (the highest of the actor's sub-positions):
// the eye leans and bobs with the body, so the neck stays below the view
// when the run animation tips the torso forward. Smoothed to take the bob out.
int   g_fp_head_anchor = 1; float g_fp_head_up = 1.5f;   // 0 fixed eye height, 1 the head bone's HEIGHT only (swimming, crouching; no lean), 2 the full head bone
float g_fp_head_smooth = 0.35f;   // per-frame blend toward the raw head position (1 = raw)
// The head bone's origin is the neck joint at the back of the skull; the eyes
// are a head-length forward of it. The offset goes along the movement
// direction while moving (that is where the body leans), else the heading.
float g_fp_head_fwd = 1.5f; float g_fp_move_dir[3] = {}; bool g_fp_move_dir_valid = false;
float g_fp_head[3] = {}; bool g_fp_have_head = false; float g_fp_head_s[3] = {}; bool g_fp_head_s_valid = false;
float g_fp_follow_dist = 60.f, g_fp_follow_h = 25.f;   // follow: behind and above (units)
// Third person: life-size, the camera just behind and above the player; the
// heading slews round behind the direction he runs (rate-limited, 0 = fixed),
// the grips pan / zoom (distance, not world scale) / rotate / tilt, and the
// reset chord puts it back behind him.
float g_tp_dist = 24.f, g_tp_h = 22.f, g_tp_pitch = -0.15f, g_tp_follow_rate = 0.f;    // units, units, radians, deg/s cap (0 = the heading is fixed, as in first person)
float g_fp_yaw = 0.f; bool g_fp_yaw_latched = false;
bool  g_fp_hide_body = true; float g_fp_hide_radius = 0.75f;  // the player's own parts sit on the actor position; an NPC you talk to is 1+ units off
uint32_t g_fp_hidden_draws = 0;
// Which of the player's parts to hide in first person. The player's skinned
// draws (world translation on the actor position) come in the game's part
// order, stable from frame to frame, so a part is addressed by its index among
// those draws into the scene target. Mode 0 hides every player draw (also in
// shadow / reflection passes); mode 1 hides only the parts in g_fp_head_mask
// (the head, so the body stays visible below the eyes); mode 2 hides nothing.
// g_fp_solo >= 0 shows only that part (to find the head's indices by eye).
int      g_fp_body_mode = 0; uint32_t g_fp_head_mask = 0; int g_fp_solo = -1;
// While the actor moves, the FFX-60fps mod draws the model at a position
// interpolated between logic ticks, up to one step away from the actor's
// stored position: the match radius grows with the last step and shrinks
// back a few frames after the actor stops (when NPCs are close, you stand).
float    g_fp_step = 0.f; int g_fp_still_frames = 0;
uint32_t g_fp_part_idx = 0, g_fp_parts_last = 0;
// Experiment: scene-pass draws with no eye-2 camera image (fog, water, effects
// with a constant-buffer layout we do not patch). 0 draw identically in both
// eyes, 1 eye 1 only, 2 hide in both.
unsigned g_ui_hide_w = 0, g_ui_hide_h = 0; int g_ui_hide_notex = 0; uint32_t g_ui_hidden_draws = 0;   // UI draw hiding experiment (ffxvr_ui_hide)
bool g_ui_hide_cursor = false; uint32_t g_rect_unknown = 0, g_rect_checked = 0;
// The hide list: 2D UI draws to skip while the cursor hiding is on. An entry is a sprite signature -
// slot-0 texture size (0x0 = textureless), index count, and a range of quad width / height in UI
// units (pulsing cursors change width). Filled from the managed side (ffxvr_ui_hide_list), which
// keeps it in the ini; entries come from picks.
struct HideSig { unsigned tw, th; float w0, w1, h0, h1; };   // per quad: the game batches sprites, so the index count is not part of it
HideSig g_hide_sigs[16]; int g_hide_sig_n = 0; uint32_t g_hide_sig_hits[16] = {};
// Partial hide of the draw being reported (R39): index ranges to leave out, (first, count) relative to the draw's start, sorted
// and apart (ffxvr_draw_skip_ranges). Runs past 8 stay visible (g_skip_overflow).
struct SkipRange { unsigned first, count; }; SkipRange g_skip[8]; int g_skip_n = 0;
// R39/R48 counters for the perf: line, since the last one: vertices read, batches of a hidden kind over k_ui_max_q quads,
// draws with a non-zero start (and those not read: start not on a quad, or a negative base), with a non-zero base, runs past 8.
uint32_t g_ui_verts_read = 0, g_ui_trunc = 0, g_ui_start_nonzero = 0, g_ui_start_bail = 0, g_ui_base_nonzero = 0, g_skip_overflow = 0;
const unsigned k_ui_max_q = 32;   // quads examined per batch (R39; was 12). Going to 64 means a uint64_t qmask (1ull << q) in ffxvr_on_draw too
float g_ui_xy[2 * 4 * k_ui_max_q]; unsigned g_ui_xy_n = 0; int g_ui_xy_draw = -1; uint32_t g_ui_xy_frame = 0;   // the per-draw shared read (ui_quads_cached)
// The pointer rule: the game places its battle pointers (over the target, over the acting character) by
// projecting the actor through ITS camera, and draws them as a small quad - textureless (the turn pointer,
// a spinning billboard whose width and height vary with the camera distance) or from the 1024 atlas (the
// target pointer). So: a small quad of those kinds that sits in the column above where an actor projects
// through the game camera is a pointer. Size-independent, batching-independent, no list to maintain.
bool g_hide_pointers = false; float g_actor_ui[k_world_max][2]; int g_actor_ui_src[k_world_max]; int g_actor_ui_n = 0; uint32_t g_pointer_hits = 0; int g_actor_ui_frame = -1;
// Window backgrounds seen so far this frame (large quads: the CTB list panel, the command and status windows).
// A small quad lying inside one is an icon on a window, never a pointer - the CTB portraits sit right where
// a character at the right of the screen can project.
struct BgRect { float x0, y0, x1, y1; }; BgRect g_bg_rects[48]; int g_bg_n = 0; int g_bg_frame = -1;
// Draw arguments of the call being reported (set by ffxvr_draw_args just before ffxvr_on_draw).
int g_draw_kind = 0; unsigned g_draw_n = 0, g_draw_start = 0; int g_draw_base = 0;
// R48: the draw's first vertex in the slot-0 stream: the base vertex exactly, plus a non-zero start index only when it begins a
// quad (DrawIndexed, start % 6 == 0: the UI's 0,1,2,2,1,3 pattern per quad, quad k = vertices 4k..4k+3). False: not read.
static bool ui_first_v(unsigned* first_v) {
    if (g_draw_base < 0 || g_draw_base > 0x7FFFFFF || g_draw_start > 0x7FFFFFFu) return false;   // no 32-bit wrap below (B6-8)
    *first_v = (unsigned)g_draw_base;
    if (g_draw_start != 0) { if (g_draw_kind != 1 || g_draw_start % 6 != 0) return false; *first_v += (g_draw_start / 6) * 4; }
    return true;
}
// The game's own pointer art, re-used: when a pointer quad is cut out of the UI pass, its whole draw state
// is kept (texture, sampler, shaders, layout, blend, cb0, the UV / colour streams at that quad's offset, and
// the quad's size) together with the actor it floated over. At the end of the frame it is drawn again into
// each eye image at OUR projection of the marker for that actor, with the game's pipeline, so the headset
// shows the real cursor (animation included) in 3D instead of a stand-in triangle.
struct PtrSprite {
    ID3D11ShaderResourceView* srv; ID3D11SamplerState* smp; ID3D11VertexShader* vs; ID3D11PixelShader* ps; ID3D11InputLayout* il;
    ID3D11BlendState* bs; float bf[4]; UINT bm; ID3D11Buffer* cb0; ID3D11Buffer* vb[8]; UINT stride[8], off[8];   // every bound stream (UVs turned out not to be in slots 1-2)
    float w, h; float world[3]; bool has_world; unsigned q;
};
PtrSprite g_ptr_sprites[6]; int g_ptr_sprite_n = 0; bool g_ptr_reuse = false; uint32_t g_ptr_drawn = 0, g_ptr_unpaired = 0, g_ptr_captured = 0; double g_ptr_log_t = 0;
ID3D11Buffer* g_ptr_vb = nullptr; ID3D11Buffer* g_ptr_ib = nullptr;
static void ptr_sprites_release() {
    for (int i = 0; i < g_ptr_sprite_n; ++i) {
        PtrSprite& p = g_ptr_sprites[i];
        if (p.srv) p.srv->Release(); if (p.smp) p.smp->Release(); if (p.vs) p.vs->Release(); if (p.ps) p.ps->Release(); if (p.il) p.il->Release();
        if (p.bs) p.bs->Release(); if (p.cb0) p.cb0->Release(); for (int k = 0; k < 8; ++k) if (p.vb[k]) p.vb[k]->Release();
    }
    g_ptr_sprite_n = 0;
}
// Called from the UI draw path for a quad about to be hidden: quad q of the draw, its UI rect, the actor it
// sits over (index into g_world_live, or -1).
static void ptr_sprite_capture(unsigned q, float x0, float y0, float x1, float y1, int actor) {
    unsigned first_v = 0;
    if (!g_ptr_reuse || g_ptr_sprite_n >= 6 || !g_ctx || !ui_first_v(&first_v)) return;
    PtrSprite p = {};
    g_ctx->PSGetShaderResources(0, 1, &p.srv); g_ctx->PSGetSamplers(0, 1, &p.smp);
    g_ctx->VSGetShader(&p.vs, nullptr, nullptr); g_ctx->PSGetShader(&p.ps, nullptr, nullptr); g_ctx->IAGetInputLayout(&p.il);
    g_ctx->OMGetBlendState(&p.bs, p.bf, &p.bm); g_ctx->VSGetConstantBuffers(0, 1, &p.cb0);
    g_ctx->IAGetVertexBuffers(0, 8, p.vb, p.stride, p.off);
    for (int k = 1; k < 8; ++k) if (p.vb[k]) p.off[k] += (first_v + q * 4) * p.stride[k];   // this quad's four vertices in every other stream
    p.w = x1 - x0; p.h = y1 - y0; p.q = q;
    p.has_world = actor >= 0 && actor < g_world_live_n;
    if (p.has_world) memcpy(p.world, g_world_live[actor], sizeof(p.world));
    g_ptr_sprites[g_ptr_sprite_n++] = p;
}
// Mouse pick of 2D UI draws: the next frame's UI draws whose quads cover the picked point are logged
// (ffxvr_diag.log) with texture, index count, rectangle and the whole cb0, and summarised for the overlay.
// The UI's coordinate space is a centred 1920 x 1080 layout (y down, x -960..960), scaled to the backbuffer
// (calibrated with two picks: the command cursor triangle and the HP window).
bool g_pick_armed = false; float g_pick_nx = 0, g_pick_ny = 0;
unsigned g_pick_scene_text = 0;   // scene-pass draws sampling a 2048x4096 sheet during the pick frame
char g_pick_summary[1024] = ""; int g_pick_hits = 0; float g_pick_best_d = 1e9f; char g_pick_best[200] = "";
// Hover highlight: every frame, the quads under the mouse (mode 1) or every UI quad (mode 2) are collected
// as rectangles in UI units for the overlay to outline. Written during the UI pass, read at Present.
int g_hover_mode = 0; float g_hover_nx = 0, g_hover_ny = 0;
// UI atlas dump: the 1024x1024 (and 1632x1088) textures the UI pass samples, written as .dds next to the
// mod (they are BC3-compressed, so no BMP), to cut the cursor art out of by hand.
bool g_atlas_dump_armed = false; ID3D11Texture2D* g_atlas_dumped[16]; int g_atlas_dumped_n = 0;
struct HoverRect { float x0, y0, x1, y1; unsigned tw, th, hit; };
HoverRect g_hover_rects[512]; int g_hover_n = 0;
int g_slowmo_ms = 0;   // sleep per frame (slow motion for picking things that flash by)
// Flat mode (no headset): the UI pass is not captured into a layer, but its draws are still recognised
// (same depth clear on the scene target that opens the pass in VR) so picks, the census and the hide
// experiments work on the monitor. Set at that depth clear, cleared at the next resolve / frame start.
bool g_ui_flat_pass = false; bool g_ui_flat_draw_now = false; bool g_ui_flat_resolved = false;
ID3D11Texture2D* g_flat_scene_tex = nullptr; uint32_t g_flat_scene_draws = 0; std::map<ID3D11Texture2D*, uint32_t> g_flat_draws_per_target;   // sprite-rect lookups that had no usable Map pointer / total   // the battle target cursor: a single 1024x1024-atlas quad of ~68x45 units drawn outside the CTB column (design-notes "cursor vertices")
int g_noimg_mode = 0; uint32_t g_noimg_draws = 0; int g_noimg_census_lines = 0;
// Bisect tool: scene-pass draws by index in the frame. mode 1 hides [a,b],
// mode 2 shows only [a,b]. The census lists every scene-pass draw.
// History (temporal accumulation) draws: a full-screen draw, blending, into a
// full-resolution target that was neither cleared nor rendered into earlier
// this frame -- the previous frame mixed into this one. Water areas do this
// (a persistent buffer that the scene is drawn into and read back from),
// which trails on every head movement. 0 leave, 1 draw without blending
// (history = this frame only), 2 skip.
int g_hist_mode = 1; uint32_t g_hist_draws = 0; uint32_t g_draw_in_frame_last = 0;
ID3D11Texture2D* g_targets_frame[64]; int g_targets_frame_n = 0;   // rtv0 textures some draw has targeted so far this frame
ID3D11Texture2D* g_cleared_frame[64]; int g_cleared_frame_n = 0;   // colour-cleared so far this frame
ID3D11BlendState* g_opaque_blend = nullptr; ID3D11BlendState* g_nb_saved = nullptr; float g_nb_saved_bf[4]; UINT g_nb_saved_mask = 0; bool g_nb_active = false;
int g_bisect_mode = 0, g_bisect_a = 0, g_bisect_b = 0; int g_scene_census_lines = 0; uint32_t g_scene_draw_idx = 0, g_scene_draws_last = 0;
uint32_t g_twins_recycled = 0, g_twins_swept = 0, g_twin_srvs_dropped = 0, g_buffers_pruned = 0;
XrFovTan g_xr_fov[2] = { { -1.f, 1.f, 1.f, -1.f }, { -1.f, 1.f, 1.f, -1.f } };   // from the runtime
XrFovTan g_xr_fov_used[2] = {};         // what this frame is rendered with (submitted with the layers)
bool  g_xr_fov_applied = false;
// Virtual screen for frames that are not 3D.
int   g_xr_screen_mode = 0;             // 0 auto, 1 always, 2 never
// Layered-2D backdrop rooms (Besaid hut interiors): the walls are painted
// layers drawn through a dummy camera (identity view at (0,0,-1), fixed
// projection, 512x448 PS2-space transforms further down the buffer). Never
// overridden. g_backdrop_mode: 1 = the whole frame on the virtual screen
// while such draws are present (default), 0 = 3D props over the flat backdrop.
int   g_backdrop_mode = 1; uint32_t g_bg_uploads_frame = 0, g_bg_uploads_total = 0;
bool  g_ovr_suspend = false;            // the frame goes to the virtual screen: render it exactly as the game does
// Stereo screen: 3D frames go to the virtual screen in stereo, seen through the game's own camera and lens.
// The two eyes are the game's camera moved -+sep/2 along its right axis, with a horizontal image shift so the
// plane at distance `conv` (game units) has no disparity and sits on the screen. No head pose, no headset FOV,
// no first-person or diorama view. 2D frames (menus, FMV) still go to the mono screen. UI draws and other
// camera-less overlays come out identical in both eyes, i.e. on the screen plane - which is right for a screen.
bool  g_screen3d = false, g_screen3d_now = false; float g_screen3d_sep = 6.f, g_screen3d_conv = 300.f;   // eye separation and screen-plane distance, game units
// Auto depth: per frame, the screen plane follows the subject (the skinned actor nearest the image centre)
// and the separation is capped so that the nearest visible actor never pops out of the screen by more than
// g_s3_max_pop (a fraction of the screen width). Targets are eased (time constant ~0.3 s) and snapped on cuts.
bool g_s3_auto = true; float g_s3_depth = 1.f, g_s3_max_pop = 0.025f, g_s3_plane_set = 300.f;
float g_s3_sep_cur = 10.f, g_s3_conv_cur = 300.f; uint32_t g_s3_frame = 0, g_s3_cut_seen = 0; int g_s3_vis = 0; float g_s3_near = 0.f, g_s3_subject = 0.f;
float g_s3_subj_pos[3] = {}; bool g_s3_subj_valid = false; float g_s3_cand_t = 0.f;   // the subject is sticky: a challenger must be clearly nearer the centre for half a second
bool  g_backdrop_room = false;          // last frame had painted-backdrop draws: first person makes no sense here
float g_xr_screen_width = 2.5f, g_xr_screen_dist = 2.5f, g_xr_screen_dist_placed = -1.f;
uint32_t g_cam_uploads_frame = 0;       // perspective camera buffers seen this frame
uint32_t g_cam3d_uploads_frame = 0;     // ... of which real cameras (not the title/intro near-2D projection)
UINT g_dump_cb_size = 0; int g_dump_cb_left = 0;   // one-shot constant-buffer layout dump (ffxvr_dump_cb)
// One-frame capture of the player's skinned buffers (bone palettes), raw, to
// ffxvr_bones.bin, with the actor's sub-positions in ffxvr_bones.log (phase 3).
int g_bones_left = 0; uint32_t g_bones_frame = 0; float g_bones_optpos[26][3] = {}; int g_bones_take = 0;   // take N -> ffxvr_bones_N.bin
// Bone palettes: from +1344 of the player's skinned buffers, 64-byte row-major
// affine matrices in render (world) space, one per bone slot, per part; the
// same bone is byte-identical in every part that uses it. Per frame the
// player's uploads are numbered (record 0, 1, ...) and their palettes kept, so
// a bone picked as (record, slot) can be found in the other parts by value
// (the map is built at frame end and used on the next frame).
constexpr int k_pal_max_slots = 64, k_pal_max_recs = 32;
// Where the palette starts: phyrechrlitshader's variants declare BoneTransforms (float4x4[80]) at
// +1344, +1360 or +1456 of $Globals depending on the variant (from the DXBC reflection in the VBF's
// shader files), and the buffer size alone does not say which. The first of those offsets that is
// followed by three affine matrices wins.
UINT g_pal_off_cur = 1344;
struct RecPal { int n; UINT off; uint8_t m[k_pal_max_slots][64]; };
RecPal g_pal[k_pal_max_recs]; int g_pal_n = 0;           // this frame's records
int g_bone_rec = 0;                                      // next record index this frame
int g_pal_n_last = 0; int g_pal_slots_last[k_pal_max_recs] = {};
// Wiggle diagnostic: (record, slot) lifted by `amount` units in world Y, and
// every other (record, slot) that carried the same matrix last frame.
int g_wig_rec = -1, g_wig_slot = -1; float g_wig_amt = 0.f;
uint8_t g_wig_map[k_pal_max_recs][k_pal_max_slots / 8] = {}; int g_wig_matches = 0;
// Sequence capture: one record's palette every frame for N frames, to
// ffxvr_bones_seq.bin ("SEQ1", u32 frame, u32 slots, matrices), for solving
// the hierarchy and the bind-pose joint positions from the motion.
int g_bseq_rec = 10, g_bseq_left = 0;
// VR hands (phase 3): in first person the arms follow the controllers. The
// skeleton from the 240-frame solve (docs/design-notes.md): bind-space pivots
// (model units, x = Tidus's left, y down) and the slots of the 53-slot
// reference part. Each arm segment is moved as a rigid world-space delta
// about its joint, applied to every bone of the group (fingers keep their
// animated curl), so no bind rotation is needed anywhere.
bool  g_hands_on = false; float g_hands_cal[3] = { 0.f, 0.f, 0.f };   // calibration: yaw, pitch, roll of the hand about the grip (radians)
float g_hands_off[3] = { 0.f, 0.f, 0.f };                              // grip -> hand offset (reference metres: right, up, back; mirrored for the left)
bool  g_hands_collapse_head = true; bool g_hands_swap = false, g_hands_mirror = false; int g_hands_only = 0;
// Pose calibration: with both arms straight out in front, palms down, the
// grip -> hand rotation per controller is captured (g_hands_Rc), so the
// fingers are then wherever the controller points. Sliders trim on top.
float g_hands_Rc[2][9] = { { 1,0,0, 0,1,0, 0,0,1 }, { 1,0,0, 0,1,0, 0,0,1 } }; bool g_hands_cal_pending = false; uint32_t g_hands_cal_count = 0;
// Which bind side is Tidus's left: measured while he runs (left = up x travel
// direction, compared with the vector from the -x shoulder to the +x one).
float g_side_acc = 0.f; bool g_px_is_left = false, g_side_known = false; float g_fwd_sign = 0.f;   // measured: +x is Tidus's RIGHT; kept in the ini
// The arms stretch along their bones (up to this factor) when the controller is further than Tidus's 5.2-unit reach.
float g_hands_stretch = 1.6f;
int   g_hands_show = 2;   // what of the body is drawn with the hands: 0 all, 1 arms only, 2 forearms + hands, 3 hands only (the rest collapses with the head)   // diagnostics: swap the arms, mirror the targets left<->right, drive one hand only (1 left, 2 right)
constexpr int   k_ref_slots = 53;
constexpr float k_bL_sh[3] = { 1.90f, -14.70f, -0.01f }, k_bL_el[3] = { 3.82f, -12.79f, -0.04f }, k_bL_wr[3] = { 5.57f, -11.03f, -0.07f };
constexpr float k_bL_kn[3] = { 6.31f, -10.65f, 0.09f },  k_bL_th[3] = { 5.76f, -10.53f, -0.46f };   // knuckles (middle), thumb base
constexpr float k_bN_hd[3] = { 0.f, -15.81f, 0.05f };
const int k_slL_up[] = { 20 }, k_slL_fo[] = { 25, 26, 44 }, k_slL_hd[] = { 45, 46, 47, 48, 49, 50, 51, 52 };          // 21, 23, 24 share the pivot but deform the chest (the torso part uses them)
const int k_slR_up[] = { 41 }, k_slR_fo[] = { 38, 39, 40 }, k_slR_hd[] = { 31, 32, 33, 34, 35, 36, 37 };              // 42, 43 likewise
const int k_sl_head[] = { 0, 1, 27, 30 };
enum { G_NONE = -1, G_L_UP, G_L_FO, G_L_HD, G_R_UP, G_R_FO, G_R_HD, G_HEAD, G_COUNT };
// Slot -> group: the reference record by table; any other record by matching
// each slot against last frame's reference bones transported to this frame
// (bones move little in a frame, different bones differ by a lot). Record
// order and byte equality are not relied on.
int8_t  g_refg[k_ref_slots]; bool g_refg_built = false;
float   g_ref_T[k_ref_slots][16]; bool g_ref_T_valid = false;
float   g_hand_W0[2][3], g_hand_P[2][3], g_hand_E[2][3], g_hand_Hn[2][9]; bool g_hand_live[2] = {}; bool g_hands_dump = false;
bool    g_hands_split = false;   // the per-hand split draw (superseded by the NaN cut; kept for experiments)
bool    g_hands_nan_cut = true;  // cut the adjoining segment with NaN matrices (see hands_apply)   // per bind side (0 = +x): this frame's wrist as animated, and where the hand goes
int     g_ap_arm = 0, g_ap_col = 0, g_ap_none = 0, g_ap_arm_l = 0, g_ap_col_l = 0, g_ap_none_l = 0;   // slots this / last frame: driven as arm, collapsed, untouched
int     g_ref_rec = -1, g_ref_claim = -1;                        // the 53-slot record (last frame; claimed this frame)
float   g_ref_W[16]; bool g_ref_valid = false;                   // that record's actor world matrix and the arm joints, last frame
float   g_ref_pal[k_ref_slots][16];
float   g_hb_P[3], g_hb_Rb[9], g_hb_sc = 1.f; bool g_hb_valid = false;   // first-person base: eye anchor, heading frame rows (right, up, back), units per metre
float   g_hb_eye[3], g_hb_right[3], g_hb_fwd[3];                          // the eye in the world and the head's horizontal right / forward: the virtual shoulders hang off these
float   g_hands_sh_w = 0.17f, g_hands_sh_down = 0.13f, g_hands_sh_back = 0.04f;   // shoulder half-width, drop below the eyes, behind them (metres)
float   g_delta[G_COUNT][16]; bool g_delta_on[G_COUNT]; uint32_t g_delta_frame = 0;
uint32_t g_hands_frames = 0; char g_hands_why[96] = "off";
uint32_t g_frames_without_cam = 0;
bool  g_xr_screen_now = false;          // decided per frame
float g_xr_screen_q[4] = { 0, 0, 0, 1 }, g_xr_screen_p[3] = {};

// UI panel. The game's UI pass is the run of draws into the MSAA scene
// target after a resolve and a depth-only clear (colour kept: the scene is
// the base). The resolve before it is the clean per-eye image; the UI draws
// are re-issued into a transparent texture shown on a head-locked quad.
enum UiState { UI_IDLE = 0, UI_RESOLVED = 1, UI_PASS = 2, UI_DONE = 3 };
// Blitzball: the scoreboard and the radar disc are drawn into the scene target after the scene resolve and
// before the UI pass opens, with no camera we recognise (identical in both eyes: double vision). With
// g_blitz_overlay on, the first such draw opens the UI pass early (g_ui_early) so the run lands in the UI
// layer; the resolve that follows the run pauses the pass instead of ending it (g_ui_early_keep), and the
// next opener - another such draw, or the usual depth clear - resumes it without re-copying the eye images
// (the game's own resolve now has the overlay baked in) and without clearing the UI layer.
bool g_blitz_overlay = false, g_ui_early = false, g_ui_early_keep = false; uint32_t g_ui_early_draws = 0, g_ui_early_report = 0;
int   g_ui_state = UI_IDLE;
bool  g_ui_enabled = true;
float g_ui_dist = 1.5f, g_ui_width = 1.6f;
bool  g_markers_3d = true;             // our battle markers as 3D geometry in the eye images
ID3D11Texture2D* g_scene_resolved = nullptr;    // the latest resolve destination of the scene target (game's)
ID3D11Texture2D* g_ui_tex = nullptr; ID3D11RenderTargetView* g_ui_rtv = nullptr;
// The ImGui layer: everything drawn into the backbuffer between our frame
// hook and the real Present (Fahrenheit's own menus such as the save screen,
// and our overlay) is re-issued into this texture and shown on a head-locked
// quad. Cleared after each submit; one frame of latency.
ID3D11Texture2D* g_imgui_tex = nullptr; ID3D11RenderTargetView* g_imgui_rtv = nullptr;
bool g_imgui_draw_now = false; bool g_imgui_layer = true; uint32_t g_imgui_draws_frame = 0, g_imgui_draws_last = 0;
bool g_imgui_force = false; uint32_t g_imgui_min_draws = 12;   // shown when the managed side says so (overlay open, save screen) or when there is a real menu's worth of draws
ID3D11Texture2D* g_eye1_final = nullptr; ID3D11RenderTargetView* g_eye1_final_rtv = nullptr;
ID3D11Texture2D* g_eye2_final = nullptr; ID3D11RenderTargetView* g_eye2_final_rtv = nullptr;
uint32_t g_ui_pass_frame = 0;                   // frame in which a UI pass completed
uint32_t g_ui_draws_frame = 0, g_ui_draws_report = 0;
uint32_t g_ui_frames_with_pass = 0, g_ui_frames_3d = 0, g_ui_resolves_frame = 0, g_ui_depth_clears_frame = 0; bool g_ui_last_frame_had_pass = false;   // 10 s tallies for the ui: diag line
// Cursor stage C2: the game's two battle cursors are skipped while our 3D markers are what the headset shows. g_c2_on comes
// from managed (cursor_source=game, hooks live, markers on); g_c2_skip is decided once per frame at begin_frame on the render
// thread and published with an interlocked store; the detours read it on the game's main thread (ffxvr_cursor_skip).
bool g_c2_on = false; volatile LONG g_c2_skip = 0; uint32_t g_c2_skip_frames = 0; uint32_t g_c2_nopass = 0; const uint32_t k_c2_nopass = 2;   // C2-8: 3D frames in a row without the UI pass before the skip lets go
ID3D11BlendState* g_ui_blend = nullptr;         // straight alpha with coverage alpha
bool  g_ui_draw_now = false;                    // the draw being re-issued is a UI draw
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

static void seat_frame(float yaw, float pitch, float Rh[9]) {   // rows: right, up, back - the seat's heading frame
    const float fy[3] = { -sinf(yaw), 0.f, -cosf(yaw) };
    const float right[3] = { -fy[2], 0.f, fy[0] };
    float R[9] = { right[0], right[1], right[2],  0.f, 1.f, 0.f,  -fy[0], -fy[1], -fy[2] };
    if (fabsf(pitch) > 1e-4f) {
        const float cp = cosf(pitch), sp = sinf(pitch); float nu[3], nb[3];
        for (int k = 0; k < 3; ++k) { nu[k] = R[3+k] * cp + R[6+k] * sp; nb[k] = -R[3+k] * sp + R[6+k] * cp; }
        for (int k = 0; k < 3; ++k) { R[3+k] = nu[k]; R[6+k] = nb[k]; }
    }
    memcpy(Rh, R, sizeof(R));
}
static void gesture_tick();
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
        g_xr_screen_dist_placed = -1.f;   // re-place the screen below
    }
    if (g_xr_screen_dist_placed != g_xr_screen_dist) {
        // The virtual screen sits g_xr_screen_dist in front of the reference, facing the user; re-placed from the
        // reference whenever the distance setting changes, so the slider works live (it used to wait for a recentre).
        g_xr_screen_dist_placed = g_xr_screen_dist;
        const float ahead[3] = { 0.f, 0.f, -g_xr_screen_dist }; float off[3]; quat_rotate(g_xr_ref_q, ahead, off);
        g_xr_screen_p[0] = g_xr_ref_p[0] + off[0]; g_xr_screen_p[1] = g_xr_ref_p[1] + off[1]; g_xr_screen_p[2] = g_xr_ref_p[2] + off[2];
        memcpy(g_xr_screen_q, g_xr_ref_q, sizeof(g_xr_screen_q));
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

// Quest controllers as the gamepad: the OpenXR action state, synced once a
// frame, becomes the XInput state the game asks for (merged with a physical
// pad if one is connected: buttons OR, the stick with more deflection wins).
bool     g_vrpad_enabled = true;
XrInput  g_vrpad = {};                       // copied from the XR layer each frame
uint32_t g_vrpad_frames = 0, g_vrpad_packet = 1;
bool     g_vrpad_suppress = false;           // a mod gesture is using the controllers: the game sees nothing this frame
// Diorama gestures (seat view, not first person): a grip grabs the world -
// one hand pans, two hands pan with the midpoint, rotate with the angle
// between them and zoom with their distance, all about the midpoint. The
// results are an extra seat offset (heading frame, units), a yaw added to the
// seat, and a multiplier on the world scale. Chord: both stick clicks =
// toggle first person (counted; the managed side performs the toggle).
bool     g_gest_enabled = true;
float    g_gest_move[3] = {};                // right, up, forward (units), added to the manual seat offset
float    g_gest_scale = 1.f;                 // multiplies g_xr_scale
float    g_gest_pitch = 0.f;                 // diorama (follow) view: the seat's pitch, set by the tilt gesture (radians, + looks up)
// Diorama / third-person anchor: the player's position, optionally smoothed (the actor table moves at the logic
// rate, 30 Hz, under a 60 Hz render). A time constant, applied once per frame (R4); 0 = snap, the closest to the
// behaviour before Oct 2026, when the 0.5-per-update factor ran on every camera upload (effectively a snap).
// A/B target: 24 ms (design-notes' "0.5 per frame" at 60 fps).
float    g_fp_anchor[3] = {}; bool g_fp_anchor_valid = false; float g_fp_anchor_tau = 0.f;   // s
// Battle: the diorama anchors to the centre of the combatants (set by the managed side from the battle lists).
// Its own time constant; 0 = snap. A/B target: 325 ms ("0.05 per frame").
float    g_anchor_ovr[3] = {}; bool g_anchor_ovr_valid = false, g_anchor_ovr_was = false; float g_anchor_ovr_tau = 0.f;   // s
// The once-per-frame view tick (R4): the third-person slew, the anchor and the seat follow-pans advance at the first
// eye-1 upload of a real 3D camera in each frame, on the render thread. g_anchor_frame: the last frame the anchor
// advanced (a gap of more than 2 frames snaps it).
uint32_t g_view_tick_frame = 0xFFFFFFFFu, g_anchor_frame = 0;
uint32_t g_view_ticks = 0, g_view_uploads = 0, g_view_othercam = 0, g_view_early = 0; float g_tick_v[12] = {};   // diag: the 10 s "view:" line
// Diorama floor: the eye never goes below this many units above the anchor
// (the player's feet / the fight's centre), except while a grip is held so a
// deliberate drag below is possible; 0 = off.
float    g_diorama_floor = 8.f;
bool     g_gest_grab[2] = { false, false };  // grip held last frame
float    g_gest_last_p[2][3] = {};           // hand positions (reference space, m) last frame
float    g_gest_last_tilt = 0.f;             // mean grip pitch last frame (two-hand tilt)
float    g_gest_tilt_gain = 1.f;             // seat pitch change per radian of wrist roll (negative inverts)
uint32_t g_gest_toggle_fp = 0;               // chord events, read and cleared by the managed side
bool     g_gest_chord_down = false, g_gest_reset_down = false, g_gest_nchord_down = false; uint32_t g_gest_next_chr = 0;
bool     g_gest_active = false;              // a grab is in progress this frame (overlay)
/// Each mode's default view offset / pitch / scale.
static void view_defaults(int mode) {
    g_gest_scale = 1.f; g_gest_pitch = (mode == 2) ? -0.35f : (mode == 3) ? g_tp_pitch : 0.f;
    if (mode == 2)      { g_gest_move[0] = 0.f; g_gest_move[1] = g_fp_follow_h; g_gest_move[2] = -g_fp_follow_dist; }
    else if (mode == 3) { g_gest_move[0] = 0.f; g_gest_move[1] = g_tp_h;        g_gest_move[2] = -g_tp_dist; }
    else g_gest_move[0] = g_gest_move[1] = g_gest_move[2] = 0.f;
}
/// Reset the grab: the authored seat goes back to its camera; the diorama and
/// third person go back behind and above the player (third person: behind the
/// way he last went).
static void view_reset() {
    view_defaults(g_fp_mode);
    if (g_fp_mode == 3 && g_fp_yaw_latched && g_fp_move_dir_valid) g_fp_yaw = atan2f(-g_fp_move_dir[0], -g_fp_move_dir[2]);
}
static SHORT stick_s(float v) { v = v < -1.f ? -1.f : (v > 1.f ? 1.f : v); return (SHORT)(v * 32767.f); }
static void vrpad_apply(XINPUT_STATE* st, bool had_pad) {
    const XrHand& L = g_vrpad.hand[0]; const XrHand& R = g_vrpad.hand[1];
    if (!had_pad) memset(st, 0, sizeof(*st));
    XINPUT_GAMEPAD& g = st->Gamepad;
    if (g_vrpad_suppress) { if (!had_pad) st->dwPacketNumber = g_vrpad_packet; return; }
    WORD b = 0;
    if (R.primary) b |= XINPUT_GAMEPAD_A;  if (R.secondary) b |= XINPUT_GAMEPAD_B;
    if (L.primary) b |= XINPUT_GAMEPAD_X;  if (L.secondary) b |= XINPUT_GAMEPAD_Y;
    if (L.squeeze > 0.5f) b |= XINPUT_GAMEPAD_LEFT_SHOULDER; if (R.squeeze > 0.5f) b |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (L.menu) b |= XINPUT_GAMEPAD_START; if (L.stick_click) b |= XINPUT_GAMEPAD_BACK; if (R.stick_click) b |= XINPUT_GAMEPAD_RIGHT_THUMB;
    g.wButtons |= b;
    const BYTE lt = (BYTE)(L.trigger * 255.f), rt = (BYTE)(R.trigger * 255.f);
    if (lt > g.bLeftTrigger) g.bLeftTrigger = lt; if (rt > g.bRightTrigger) g.bRightTrigger = rt;
    auto merge = [](SHORT& x, SHORT& y, const float* v) {
        const float m_pad = (float)x * (float)x + (float)y * (float)y, m_vr = (v[0]*v[0] + v[1]*v[1]) * 32767.f * 32767.f;
        if (m_vr > m_pad) { x = stick_s(v[0]); y = stick_s(v[1]); }
    };
    merge(g.sThumbLX, g.sThumbLY, L.stick); merge(g.sThumbRX, g.sThumbRY, R.stick);
    st->dwPacketNumber = g_vrpad_packet;
}
static DWORD WINAPI xi_get_state_hook(DWORD idx, XINPUT_STATE* st) {
    DWORD r = g_xi_orig(idx, st);
    g_xi_hook_calls_all++;
    if (idx < 4 && st != nullptr) { g_xi_seen[idx].res = r; g_xi_seen[idx].rx = st->Gamepad.sThumbRX; g_xi_seen[idx].tick = GetTickCount(); }
    const bool vr = g_vrpad_enabled && g_xr_active && g_vrpad.ready && idx == 0 && st != nullptr;
    if (vr) { vrpad_apply(st, r == ERROR_SUCCESS); r = ERROR_SUCCESS; }
    if (r == ERROR_SUCCESS && st) {
        g_xi_hook_calls++;
        // Third person with the heading following the run (off by default):
        // a remap against the live view would chase itself (push sideways: he
        // turns with the camera, the camera turns after him, round and round),
        // so the remap is latched when the stick leaves centre and held until
        // it returns. With a fixed heading the control is first person's.
        float delta = g_move_delta;
        if (g_fp_mode == 3 && g_tp_follow_rate > 0.f) {
            const float sx = (float)st->Gamepad.sThumbLX / 32767.f, sy = (float)st->Gamepad.sThumbLY / 32767.f;
            const bool pushed = sx * sx + sy * sy > 0.25f * 0.25f;
            if (pushed && !g_move_held) g_move_delta_hold = g_move_delta;
            g_move_held = pushed;
            if (pushed) delta = g_move_delta_hold;
        }
        else g_move_held = false;
        if (g_move_remap && g_fp_mode != 0 && g_xr_active && g_ovr_enabled && delta != 0.f) {
            const float d = g_move_sign * delta;
            const float c = cosf(d), sn = sinf(d);
            const float sx = (float)st->Gamepad.sThumbLX, sy = (float)st->Gamepad.sThumbLY;
            const float nx = sx * c - sy * sn, ny = sy * c + sx * sn;
            st->Gamepad.sThumbLX = (SHORT)fmaxf(-32768.f, fminf(32767.f, nx));
            st->Gamepad.sThumbLY = (SHORT)fmaxf(-32768.f, fminf(32767.f, ny));
        }
    }
    return r;
}

/// Hand position relative to the recentred reference, like the head.
static void hand_to_ref(const float* p, float* out) {
    const float ref_inv[4] = { -g_xr_ref_q[0], -g_xr_ref_q[1], -g_xr_ref_q[2], g_xr_ref_q[3] };
    const float dp[3] = { p[0] - g_xr_ref_p[0], p[1] - g_xr_ref_p[1], p[2] - g_xr_ref_p[2] };
    quat_rotate(ref_inv, dp, out);
}
/// Pitch of a grip's forward axis (-Z of the grip pose) above the horizon, radians.
static float grip_pitch(const float* q) {
    const float ref_inv[4] = { -g_xr_ref_q[0], -g_xr_ref_q[1], -g_xr_ref_q[2], g_xr_ref_q[3] };
    float qr[4]; quat_mul(ref_inv, q, qr);
    const float fwd[3] = { 0.f, 0.f, -1.f }; float d[3]; quat_rotate(qr, fwd, d);
    return asinf(fmaxf(-1.f, fminf(1.f, d[1])));
}
static void gesture_tick() {
    g_vrpad_suppress = false; g_gest_active = false;
    if (!g_vrpad.ready || !g_vrpad_enabled || !g_xr_active) { g_gest_grab[0] = g_gest_grab[1] = false; g_gest_chord_down = false; return; }
    const XrHand& L = g_vrpad.hand[0]; const XrHand& R = g_vrpad.hand[1];
    // Chord: right grip + right stick click -> toggle first person (on the press edge).
    // (Both stick clicks is Virtual Desktop's own performance overlay.)
    const bool chord = R.squeeze > 0.5f && R.stick_click;
    if (chord && !g_gest_chord_down) { g_gest_toggle_fp++; xr_haptic(0, 0.6f, 0.08f); xr_haptic(1, 0.6f, 0.08f); }
    g_gest_chord_down = chord;
    if (chord) g_vrpad_suppress = true;
    // Next-character chord: left grip + right stick click (in battle, the managed side moves the eyes to the next party member).
    const bool nchord = L.squeeze > 0.5f && R.stick_click;
    if (nchord && !g_gest_nchord_down) { g_gest_next_chr++; xr_haptic(1, 0.4f, 0.06f); }
    g_gest_nchord_down = nchord;
    if (nchord) g_vrpad_suppress = true;
    // Reset chord: left grip + left stick click -> the view back to its default
    // (the authored seat to its camera, the diorama / third person behind the player).
    const bool rchord = L.squeeze > 0.5f && L.stick_click;
    if (rchord && !g_gest_reset_down) { view_reset(); xr_haptic(0, 0.6f, 0.08f); }
    g_gest_reset_down = rchord;
    if (rchord) g_vrpad_suppress = true;
    // Grabs: seat view only, gestures on, both poses needed for two hands.
    const bool diorama = g_fp_mode >= 2 && g_fp_yaw_latched && !g_ovr_suspend;
    const bool third = g_fp_mode == 3;
    const bool seat = (g_xr_seat && g_seat_latched && g_fp_mode == 0 && !g_ovr_suspend) || diorama;
    float& yaw_ref = diorama ? g_fp_yaw : g_seat_yaw;
    float& pitch_ref = diorama ? g_gest_pitch : g_seat_pitch;
    const float ovr[3] = { diorama ? 0.f : g_ovr_move[0], diorama ? 0.f : g_ovr_move[1], diorama ? 0.f : g_ovr_move[2] };
    bool grab[2] = { seat && g_gest_enabled && L.squeeze > 0.5f && L.grip_valid, seat && g_gest_enabled && R.squeeze > 0.5f && R.grip_valid };
    float hp[2][3]; hand_to_ref(L.grip_p, hp[0]); hand_to_ref(R.grip_p, hp[1]);
    const bool was[2] = { g_gest_grab[0], g_gest_grab[1] };
    const int n = (grab[0] ? 1 : 0) + (grab[1] ? 1 : 0), nwas = (was[0] ? 1 : 0) + (was[1] ? 1 : 0);
    if (n > 0) { g_gest_active = true; g_vrpad_suppress = true; }
    if (seat && g_gest_enabled && (L.squeeze > 0.5f || R.squeeze > 0.5f)) g_vrpad_suppress = true;   // the grips are the grab, not LB/RB, in the seat
    for (int h = 0; h < 2; ++h) if (grab[h] && !was[h]) xr_haptic(h, 0.3f, 0.04f);
    if (n == 2 && nwas < 2) g_gest_last_tilt = (grip_pitch(L.grip_q) + grip_pitch(R.grip_q)) * 0.5f;
    if (n > 0 && n == nwas && grab[0] == was[0] && grab[1] == was[1]) {
        const float sc = g_xr_scale * g_gest_scale;
        float Rb[9]; seat_frame(yaw_ref, pitch_ref, Rb);
        // Reference-space vectors (x right, y up, z back) -> the seat offset convention (right, up, forward).
        auto to_mv = [](const float* v, float* mv) { mv[0] = v[0]; mv[1] = v[1]; mv[2] = -v[2]; };
        if (n == 1) {
            const int h = grab[0] ? 0 : 1;
            const float d[3] = { hp[h][0] - g_gest_last_p[h][0], hp[h][1] - g_gest_last_p[h][1], hp[h][2] - g_gest_last_p[h][2] };
            float dm[3]; to_mv(d, dm);                              // the world follows the hand: the seat moves the other way
            for (int i = 0; i < 3; ++i) g_gest_move[i] -= dm[i] * sc;
        }
        else {
            const float m0[3] = { (g_gest_last_p[0][0] + g_gest_last_p[1][0]) * 0.5f, (g_gest_last_p[0][1] + g_gest_last_p[1][1]) * 0.5f, (g_gest_last_p[0][2] + g_gest_last_p[1][2]) * 0.5f };
            const float m1[3] = { (hp[0][0] + hp[1][0]) * 0.5f, (hp[0][1] + hp[1][1]) * 0.5f, (hp[0][2] + hp[1][2]) * 0.5f };
            const float v0[3] = { g_gest_last_p[1][0] - g_gest_last_p[0][0], 0.f, g_gest_last_p[1][2] - g_gest_last_p[0][2] };
            const float v1[3] = { hp[1][0] - hp[0][0], 0.f, hp[1][2] - hp[0][2] };
            const float d0 = sqrtf((g_gest_last_p[1][0]-g_gest_last_p[0][0])*(g_gest_last_p[1][0]-g_gest_last_p[0][0]) + (g_gest_last_p[1][1]-g_gest_last_p[0][1])*(g_gest_last_p[1][1]-g_gest_last_p[0][1]) + (g_gest_last_p[1][2]-g_gest_last_p[0][2])*(g_gest_last_p[1][2]-g_gest_last_p[0][2]));
            const float d1 = sqrtf((hp[1][0]-hp[0][0])*(hp[1][0]-hp[0][0]) + (hp[1][1]-hp[0][1])*(hp[1][1]-hp[0][1]) + (hp[1][2]-hp[0][2])*(hp[1][2]-hp[0][2]));
            // 1. pan with the midpoint
            const float d[3] = { m1[0] - m0[0], m1[1] - m0[1], m1[2] - m0[2] }; float dm[3]; to_mv(d, dm);
            for (int i = 0; i < 3; ++i) g_gest_move[i] -= dm[i] * sc;
            // 2. zoom: hands apart = world bigger = fewer units per metre; the midpoint stays put
            if (third && d0 > 0.05f && d1 > 0.05f) {
                // third person: hands apart = closer; the offset from the player shrinks, the scale stays life-size
                const float len = sqrtf(g_gest_move[0]*g_gest_move[0] + g_gest_move[1]*g_gest_move[1] + g_gest_move[2]*g_gest_move[2]);
                if (len > 1e-3f) { const float nl = fmaxf(4.f, fminf(400.f, len * d0 / d1)); for (int i = 0; i < 3; ++i) g_gest_move[i] *= nl / len; }
            }
            else if (d0 > 0.05f && d1 > 0.05f) {
                const float ratio = d1 / d0;
                const float sc_new = fmaxf(5.f, fminf(2000.f, sc / ratio));
                const float* anchor = m1;                                            // the midpoint in reference metres
                float head[3] = { g_xr_translate ? g_xr_p[0] : 0.f, g_xr_translate ? g_xr_p[1] : 0.f, g_xr_translate ? g_xr_p[2] : 0.f };
                const float a[3] = { anchor[0] + head[0], anchor[1] + head[1], anchor[2] + head[2] };
                float am[3]; to_mv(a, am);
                for (int i = 0; i < 3; ++i) g_gest_move[i] += am[i] * (sc - sc_new);
                g_gest_scale = sc_new / g_xr_scale;
            }
            // 3. rotate about the midpoint: yaw from the angle between the hands
            //    (clockwise from above = world clockwise = seat yaw grows), pitch
            //    from both wrists rolling together (the far edge of the "tray"
            //    coming up = the seat pitches down to look at it).
            const float a0 = atan2f(v0[0], -v0[2]), a1 = atan2f(v1[0], -v1[2]);
            float dyaw = a1 - a0; while (dyaw > 3.14159265f) dyaw -= 6.2831853f; while (dyaw < -3.14159265f) dyaw += 6.2831853f;
            const float tilt = (grip_pitch(L.grip_q) + grip_pitch(R.grip_q)) * 0.5f;
            float dpitch = -(tilt - g_gest_last_tilt) * g_gest_tilt_gain;
            g_gest_last_tilt = tilt;
            if (fabsf(dyaw) > 1e-5f || fabsf(dpitch) > 1e-5f) {
                const float sc2 = g_xr_scale * g_gest_scale;
                const float head[3] = { g_xr_translate ? g_xr_p[0] : 0.f, g_xr_translate ? g_xr_p[1] : 0.f, g_xr_translate ? g_xr_p[2] : 0.f };
                // world point under the midpoint, before: the offsets are applied along the seat frame's rows, c = (mv + (head+m)*sc) as (right, up, -back)
                float c[3] = { ovr[0] + g_gest_move[0] + (head[0] + m1[0]) * sc2, ovr[1] + g_gest_move[1] + (head[1] + m1[1]) * sc2, ovr[2] + g_gest_move[2] - (head[2] + m1[2]) * sc2 };
                float W[3]; for (int i = 0; i < 3; ++i) W[i] = Rb[0*3+i]*c[0] + Rb[1*3+i]*c[1] - Rb[2*3+i]*c[2];
                const float yaw_new = yaw_ref + dyaw;
                const float pitch_new = fmaxf(-1.5f, fminf(1.5f, pitch_ref + dpitch));
                float Rn[9]; seat_frame(yaw_new, pitch_new, Rn);
                // the same world point after: rows are orthonormal, so c' = (W.row0, W.row1, -W.row2)
                float cn[3] = { W[0]*Rn[0]+W[1]*Rn[1]+W[2]*Rn[2], W[0]*Rn[3]+W[1]*Rn[4]+W[2]*Rn[5], -(W[0]*Rn[6]+W[1]*Rn[7]+W[2]*Rn[8]) };
                g_gest_move[0] = cn[0] - ovr[0] - (head[0] + m1[0]) * sc2;
                g_gest_move[1] = cn[1] - ovr[1] - (head[1] + m1[1]) * sc2;
                g_gest_move[2] = cn[2] - ovr[2] + (head[2] + m1[2]) * sc2;
                yaw_ref = yaw_new; pitch_ref = pitch_new;
            }
        }
    }
    memcpy(g_gest_last_p, hp, sizeof(hp));
    g_gest_grab[0] = grab[0]; g_gest_grab[1] = grab[1];
}

/// Replace the game's XInputGetState import with ours (import address table).
static void install_xinput_hook() {
    if (g_xi_hooked) return;
    HMODULE exe = GetModuleHandleA(nullptr);
    const BYTE* base = (const BYTE*)exe;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) { diag("xinput hook: no import table"); return; }
    const IMAGE_IMPORT_DESCRIPTOR* imp = (const IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
    for (; imp->Name; ++imp) {
        const char* dll = (const char*)(base + imp->Name);
        if (_strnicmp(dll, "xinput", 6) != 0) continue;
        const IMAGE_THUNK_DATA* names = (const IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            const IMAGE_IMPORT_BY_NAME* ibn = (const IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp(ibn->Name, "XInputGetState") != 0) continue;
            DWORD old = 0;
            if (!VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) { diag("xinput hook: VirtualProtect failed"); return; }
            g_xi_orig = (PFN_XInputGetState)iat->u1.Function;
            iat->u1.Function = (DWORD_PTR)&xi_get_state_hook;
            VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
            g_xi_hooked = true;
            diag("xinput hook: %s!XInputGetState hooked (orig %p)", dll, (void*)g_xi_orig);
            return;
        }
    }
    diag("xinput hook: XInputGetState import not found");
}

// -- Build 7 (log only) --------------------------------------------------------------------------------------------
/// The game's rumble (6.11): every call passes straight through; motor changes per pad are kept for begin_frame to log.
static DWORD WINAPI xi_set_state_hook(DWORD pad, XINPUT_VIBRATION* v) {
    g_xis_calls++;
    if (g_diag7 && v != nullptr && pad < 4) {
        const uint16_t l = v->wLeftMotorSpeed, r = v->wRightMotorSpeed;
        if (l | r) g_xis_nonzero++;
        if (l != g_xis_last[pad][0] || r != g_xis_last[pad][1]) {
            g_xis_last[pad][0] = l; g_xis_last[pad][1] = r;
            const LONG w = g_xis_w;
            if (w < g_xis_until && w - g_xis_r < 32) { XisRec& e = g_xis_log[w & 31]; e.frame = g_frames; e.pad = pad; e.l = l; e.r = r; InterlockedIncrement(&g_xis_w); }
        }
    }
    return g_xis_orig ? g_xis_orig(pad, v) : ERROR_DEVICE_NOT_CONNECTED;
}
/// The game's XInputSetState import -> xi_set_state_hook (the XInputGetState pattern above). Build 7 only.
static void install_xinput_set_hook() {
    if (g_xis_hooked || !g_diag7) return;
    const BYTE* base = (const BYTE*)GetModuleHandleA(nullptr);
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) { diag("xinput set hook: no import table"); return; }
    for (const IMAGE_IMPORT_DESCRIPTOR* imp = (const IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        const char* dll = (const char*)(base + imp->Name);
        if (_strnicmp(dll, "xinput", 6) != 0) continue;
        const IMAGE_THUNK_DATA* names = (const IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            if (strcmp(((const IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name, "XInputSetState") != 0) continue;
            DWORD old = 0;
            if (!VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) { diag("xinput set hook: VirtualProtect failed"); return; }
            g_xis_orig = (PFN_XInputSetState)iat->u1.Function;
            iat->u1.Function = (DWORD_PTR)&xi_set_state_hook;
            VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
            g_xis_hooked = true;
            diag("xinput set hook: %s!XInputSetState hooked (orig %p) - build 7, log only", dll, (void*)g_xis_orig);
            return;
        }
    }
    diag("xinput set hook: XInputSetState import not found (the game may not rumble through its import)");
}

/// A read of our own process that fails instead of faulting (no SEH: Fahrenheit's stage 0 is a debugger).
static bool wd_read(uintptr_t a, void* out, SIZE_T n) {
    SIZE_T got = 0;
    return a >= 0x10000 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(a), out, n, &got) && got == n;
}
/// Loading-stall watchdog (6.5): when no Present has come for 300 ms, once per stall, the game's loading state and where
/// its main thread is. The main thread is suspended only for GetThreadContext and one ReadProcessMemory of its stack:
/// no allocation, no lock, no logging and nothing that can raise until ResumeThread. Everything else runs outside it.
static DWORD WINAPI stall_watchdog(LPVOID) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    uintptr_t text_lo = 0, text_hi = 0;
    {
        const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
            if (memcmp(sec[i].Name, ".text", 6) == 0) { text_lo = base + sec[i].VirtualAddress; text_hi = text_lo + sec[i].Misc.VirtualSize; }
    }
    static uint32_t stack[256];   // this thread only
    HANDLE th = nullptr; DWORD th_tid = 0;
    for (;;) {
        if (WaitForSingleObject(g_wd_stop, 50) == WAIT_OBJECT_0) break;
        if (!g_diag7 || g_frames == 0 || g_stalls >= 100 || g_slowmo_ms >= 300) continue;   // slow motion (the pick tool) is not a stall
        if (g_main_tid != 0 && g_main_tid != th_tid) {   // opened ahead of any stall
            if (th) CloseHandle(th);
            th = nullptr; th_tid = g_main_tid;
            // never the render thread or this one (B7-6: impossible today, 0xA44B30 is not on the render thread's path)
            if (g_main_tid != g_render_tid && g_main_tid != GetCurrentThreadId()) th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_main_tid);
        }
        const LONG last = g_present_ms, now = static_cast<LONG>(GetTickCount());
        if (g_stall_open || now - last < 300) continue;
        uintptr_t app = 0, rend = 0; uint8_t app_flag = 0xEE, r96 = 0xEE, r97 = 0xEE; int8_t bstate = -99; char map[16] = {};
        if (wd_read(base + 0x8C9CD8, &app, 4) && app) wd_read(app + 0x3A0, &app_flag, 1);   // PApplication +0x3A0: loads present frames
        if (wd_read(base + 0x8CB9D8, &rend, 4) && rend) { wd_read(rend + 0x1196, &r96, 1); wd_read(rend + 0x1197, &r97, 1); }
        wd_read(base + 0xD2A8E0, &bstate, 1); wd_read(base + 0x1FCBC60, map, 15);
        if (g_present_ms != last) continue;   // a Present came while the state was read: not a stall after all (B7-7)
        DWORD eip = 0, esp = 0; SIZE_T got = 0; bool ctx_ok = false;
        if (th && SuspendThread(th) != static_cast<DWORD>(-1)) {
            CONTEXT c = {}; c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(th, &c)) { ctx_ok = true; eip = c.Eip; esp = c.Esp; ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(esp)), stack, sizeof(stack), &got); }
            ResumeThread(th);
        }
        // -- the main thread runs again from here --
        g_stall_from_ms = last; InterlockedExchange(&g_stall_open, 1); g_stalls++;
        char where[96] = "?";
        if (ctx_ok) {
            if (eip >= text_lo && eip < text_hi) snprintf(where, sizeof(where), "FFX.exe+0x%X", static_cast<unsigned>(eip - base));
            else {
                HMODULE m = nullptr; char path[MAX_PATH] = "";
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(eip)), &m) && m && GetModuleFileNameA(m, path, MAX_PATH)) {
                    const char* nm = strrchr(path, '\\'); snprintf(where, sizeof(where), "%s+0x%X", nm ? nm + 1 : path, static_cast<unsigned>(eip - reinterpret_cast<uintptr_t>(m)));
                }
                else snprintf(where, sizeof(where), "%08X", static_cast<unsigned>(eip));
            }
        }
        char st[16 * 12 + 1] = ""; size_t sl = 0; int hits = 0;
        for (SIZE_T k = 0; k < got / 4 && hits < 16; ++k)
            if (stack[k] >= text_lo && stack[k] < text_hi) { sl += snprintf(st + sl, sizeof(st) - sl, " %X", static_cast<unsigned>(stack[k] - base)); hits++; }
        for (int tries = 0; tries < 3; ++tries) {   // the render thread may hold the log open: a few short retries
            wchar_t path[MAX_PATH]; _snwprintf_s(path, _TRUNCATE, L"%s\\ffxvr_diag.log", g_dir);
            FILE* f = nullptr;
            if (_wfopen_s(&f, path, L"a") == 0 && f) {
                fprintf(f, "stall #%u: no Present for %ld ms (frame %u): app+0x3A0 %d, renderer +0x1196 %d +0x1197 %d, battle_state %d, map '%s' | main thread %lu %s: eip %s, stack (FhUtil, first 16 in .text):%s\n",
                        g_stalls, static_cast<long>(now - last), g_frames, app_flag, r96, r97, bstate, map, th_tid, th ? (ctx_ok ? "" : "(no context)") : "(not known yet)", where, sl ? st : " -");
                fclose(f); break;
            }
            Sleep(5);
        }
    }
    if (th) CloseHandle(th);
    return 0;
}
/// Build 7, once per frame at begin_frame (render thread): the stall end, and the motor changes the hook kept.
static void diag7_frame() {
    g_present_ms = static_cast<LONG>(GetTickCount());
    if (!g_diag7) return;
    if (g_stall_open) { InterlockedExchange(&g_stall_open, 0); diag("stall #%u ended: %ld ms without a Present", g_stalls, static_cast<long>(g_present_ms - g_stall_from_ms)); }
    const LONG w = g_xis_w;
    if (w - g_xis_r > 32) g_xis_r = w - 32;
    for (; g_xis_r < w; ++g_xis_r) { const XisRec& e = g_xis_log[g_xis_r & 31]; diag("xinput set: pad %u motors left %u right %u (frame %u)", e.pad, e.l, e.r, e.frame); }
}

/// Turn the current heading by `deg` (positive = right).
static void turn_by(float deg) {
    const float d = -deg * 0.0174533f;                   // yaw about +y: positive turns left
    if (g_fp_mode != 0) { if (g_fp_yaw_latched) g_fp_yaw += d; }
    else if (g_seat_latched) g_seat_yaw += d;
    g_turn_count++;
}

/// Right stick x, in -1..1 with the dead zone removed; 0 without a pad.
static float right_stick_x() {
    if (g_vrpad_enabled && g_xr_active && g_vrpad.ready && g_vrpad.hand[1].active && !g_vrpad_suppress) {
        float x = g_vrpad.hand[1].stick[0]; const float dz = 0.2f;
        if (fabsf(x) >= dz) return (x - (x > 0 ? dz : -dz)) / (1.f - dz);
        // fall through: a physical pad may still be turning
    }
    auto norm_rx = [](LONG rx) {
        float x = rx / 32767.f;
        const float dz = 0.25f;
        if (fabsf(x) < dz) return 0.f;
        return (x - (x > 0 ? dz : -dz)) / (1.f - dz);
    };
    if (g_xi_hooked) {   // the game polls through its own import: use what it saw (no XInput calls of ours, no probing spikes)
        const DWORD now = GetTickCount(); bool polling = false;
        for (int i = 0; i < 4; ++i) {
            const XiSeen seen = g_xi_seen[i];
            if (seen.tick == 0 || (LONG)(now - seen.tick) > 500) continue;   // signed: a sample newer than `now` (other thread) is fresh
            polling = true;
            if (seen.res == ERROR_SUCCESS) { g_xi_pad = i; return norm_rx(seen.rx); }   // remembered: the direct read starts there if polling stops
        }
        if (polling) return 0.f;   // the game polls but no pad answered: a pad it does not poll could not drive it either
    }
    if (!g_xi_tried) {
        g_xi_tried = true;
        HMODULE m = LoadLibraryA("xinput1_4.dll");
        if (!m) m = LoadLibraryA("xinput1_3.dll");
        if (!m) m = LoadLibraryA("xinput9_1_0.dll");
        if (m) g_xi_get = (PFN_XInputGetState)GetProcAddress(m, "XInputGetState");
        if (g_xi_orig) g_xi_get = g_xi_orig;                   // the game's own import, unhooked
        diag("xinput: %s", g_xi_get ? "loaded" : "not available");
    }
    if (!g_xi_get) return 0.f;
    // No hook, or the game is not polling: read the pad directly.
    XINPUT_STATE st = {};
    if (g_xi_pad >= 0) {
        if (g_xi_get((DWORD)g_xi_pad, &st) == ERROR_SUCCESS) return norm_rx(st.Gamepad.sThumbRX);
        g_xi_pad = -1;
    }
    const ULONGLONG now = GetTickCount64();                // probing an absent pad is slow: one slot every 2 s, round robin
    if (now < g_xi_probe) return 0.f;
    g_xi_probe = now + 2000;
    const DWORD i = g_xi_probe_slot; g_xi_probe_slot = (g_xi_probe_slot + 1) % 4;
    if (g_xi_get(i, &st) == ERROR_SUCCESS) { g_xi_pad = (int)i; diag("xinput: pad %u", i); return norm_rx(st.Gamepad.sThumbRX); }
    return 0.f;
}

/// Per frame: stick turning.
static void turn_tick() {
    if (!g_turn_stick) return;
    const float x = right_stick_x();
    if (x != 0.f) turn_by(x * fabsf(x) * g_turn_rate * g_dt);   // smooth, with a gentle curve near centre
}

/// advance = this call moves the per-frame filters on (once per frame: write_camera's first eye-1 3D upload). Every
/// other caller (eye 2, ffxvr_project, the markers) passes false and only reads them. The latches (fp heading, seat
/// heading) still fire from any caller; they are idempotent within a frame (every reset happens at Present), and
/// g_cam is the post-cut camera, so that is harmless.
static void build_override_view(const float v[12], float Vn[16], float In[16], float Pn[3], bool advance) {
    float R[9] = { v[0], v[1], v[2],  v[4], v[5], v[6],  v[8], v[9], v[10] };
    const float t[3] = { v[3], v[7], v[11] };
    float P[3];
    for (int i = 0; i < 3; ++i) P[i] = -(R[0*3+i]*t[0] + R[1*3+i]*t[1] + R[2*3+i]*t[2]);

    if (g_screen3d_now) {
        // Stereo screen: the game's camera, each eye moved half the separation along the camera's right axis
        // (row 0 of R). The separation comes from the managed side (eye distance at life size x the depth slider).
        const float half = 0.5f * g_screen3d_sep * ((g_eye_shift != 0.f) ? 1.f : -1.f);
        for (int i = 0; i < 3; ++i) Pn[i] = P[i] + R[0*3+i] * half;
        float tn[3]; for (int r = 0; r < 3; ++r) tn[r] = -(R[r*3+0]*Pn[0] + R[r*3+1]*Pn[1] + R[r*3+2]*Pn[2]);
        for (int r = 0; r < 3; ++r) {
            Vn[r*4+0] = R[r*3+0]; Vn[r*4+1] = R[r*3+1]; Vn[r*4+2] = R[r*3+2]; Vn[r*4+3] = tn[r];
            In[r*4+0] = R[0*3+r]; In[r*4+1] = R[1*3+r]; In[r*4+2] = R[2*3+r]; In[r*4+3] = Pn[r];
        }
        Vn[12] = Vn[13] = Vn[14] = 0.f; Vn[15] = 1.f; In[12] = In[13] = In[14] = 0.f; In[15] = 1.f;
        if (g_eye_shift == 0.f) g_move_delta = 0.f;   // the game's camera: its own stick directions
        return;
    }

    // First person / follow: replace the base with a view at the player.
    const bool fp = (g_fp_mode != 0) && g_fp_have_pos && g_xr_active && g_ovr_enabled && !g_backdrop_room;
    const bool primary = (g_eye_shift == 0.f);
    if (primary) g_yaw_cam = atan2f(R[6], R[8]);                          // authored heading: forward = -row 2 -> atan2(-f.x, -f.z)
    if (fp) {
        if (!g_fp_yaw_latched) {                        // heading: where the authored camera looked when toggled
            const float f[3] = { -R[6], 0.f, -R[8] };
            g_fp_yaw = atan2f(-f[0], -f[2]); g_fp_yaw_latched = true;
            g_seat_latched = false;
        }
        if (g_fp_mode == 3 && advance && g_tp_follow_rate > 0.f && g_fp_step > 0.f && g_fp_move_dir_valid && !g_gest_active) {
            // Third person: the heading slews round behind the direction of
            // travel. Proportional with a rate cap, and weighted by how far
            // round it has to go, so running toward the camera does not swing
            // it (w = 0 at 180 degrees, 1/2 side-on, 1 running away).
            const float want = atan2f(-g_fp_move_dir[0], -g_fp_move_dir[2]);
            float d = want - g_fp_yaw;
            while (d >  3.14159265f) d -= 6.2831853f;
            while (d < -3.14159265f) d += 6.2831853f;
            const float w = (1.f + cosf(d)) * 0.5f;
            const float step = g_tp_follow_rate * 0.0174533f * g_dt * w;
            g_fp_yaw += fmaxf(-step, fminf(step, d * fminf(1.f, 1.5f * g_dt)));   // exponential approach (2/3 s), capped
        }
        const float fy[3] = { -sinf(g_fp_yaw), 0.f, -cosf(g_fp_yaw) };
        const float Rf[9] = { -fy[2], 0.f, fy[0],   0.f, 1.f, 0.f,   -fy[0], -fy[1], -fy[2] };   // rows: right (f x up), up, back
        memcpy(R, Rf, sizeof(R));
        if (g_fp_head_anchor == 1 && g_fp_have_head && g_fp_head_s_valid) {
            // Height from the head bone (prone when swimming, low when crouching),
            // horizontal position from the actor origin: no lean, nothing to see.
            P[0] = g_fp_pos[0]; P[1] = g_fp_pos[1] + g_fp_head_s[1] + g_fp_head_up; P[2] = g_fp_pos[2];
        }
        else if (g_fp_head_anchor == 2 && g_fp_have_head && g_fp_head_s_valid) {
            // g_fp_head_s is the smoothed head offset from the actor origin; the
            // eye is the current origin plus that, so it never trails a sprint.
            P[0] = g_fp_pos[0] + g_fp_head_s[0]; P[1] = g_fp_pos[1] + g_fp_head_s[1] + g_fp_head_up; P[2] = g_fp_pos[2] + g_fp_head_s[2];
            // Forward = the way the torso leans (the horizontal part of that
            // offset), which is the facing while moving; the heading when upright.
            const float lx = g_fp_head_s[0], lz = g_fp_head_s[2], ll = sqrtf(lx*lx + lz*lz);
            float d[3] = { fy[0], 0.f, fy[2] };
            if (ll > 0.3f) { d[0] = lx / ll; d[2] = lz / ll; }
            P[0] += d[0] * g_fp_head_fwd; P[2] += d[2] * g_fp_head_fwd;
        }
        else { P[0] = g_fp_pos[0]; P[1] = g_fp_pos[1] + g_fp_eye_h; P[2] = g_fp_pos[2]; }
        if (g_fp_mode >= 2) {
            // Diorama / third person: anchored to the player (smoothed), the offset
            // lives in the heading frame as g_gest_move (right, up, forward; behind =
            // negative forward) and is what the grab gestures and the reset set. The
            // battle centroid stands in for the player in the diorama only.
            const bool ovr_on = g_anchor_ovr_valid && g_fp_mode == 2;
            const float* target = ovr_on ? g_anchor_ovr : g_fp_pos;
            if (advance) {
                if (ovr_on != g_anchor_ovr_was) { g_fp_anchor_valid = false; g_anchor_ovr_was = ovr_on; }   // battle start / end: snap
                const uint32_t gap = g_frames - g_anchor_frame; g_anchor_frame = g_frames;
                if (gap > 2) g_fp_anchor_valid = false;                 // idle for a while (screen, stereo screen, backdrop, seat, first person, loading): snap
                if (g_fp_anchor_valid && !ovr_on) {                     // a warp (ffxvr_player_set's 50-unit rule): snap; the battle centre keeps its glide
                    const float dx = target[0] - g_fp_anchor[0], dy = target[1] - g_fp_anchor[1], dz = target[2] - g_fp_anchor[2];
                    if (dx*dx + dy*dy + dz*dz > 50.f * 50.f) g_fp_anchor_valid = false;
                }
                const float tau = ovr_on ? g_anchor_ovr_tau : g_fp_anchor_tau;
                const float k = tau > 1e-4f ? 1.f - expf(-g_dt * (float)gap / tau) : 1.f;   // g_dt: the native frame time (clamped in begin_frame)
                if (!g_fp_anchor_valid) { memcpy(g_fp_anchor, target, sizeof(g_fp_anchor)); g_fp_anchor_valid = true; }
                else for (int i = 0; i < 3; ++i) g_fp_anchor[i] += (target[i] - g_fp_anchor[i]) * k;
            }
            const float* A = g_fp_anchor_valid ? g_fp_anchor : target;
            P[0] = A[0]; P[1] = A[1]; P[2] = A[2];
        }
    }

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
            // Live authored heading (yaw about +y) and pitch (+ looks up).
            const float live_yaw = atan2f(-f[0], -f[2]);
            const float af[3] = { -Rb[6], -Rb[7], -Rb[8] };
            const float live_pitch = asinf(fmaxf(-1.f, fminf(1.f, af[1]))) * fmaxf(0.f, fminf(1.f, g_xr_pitch_follow));
            if (!g_seat_latched || fp) { g_seat_yaw = live_yaw; g_seat_pitch = (g_fp_mode >= 2) ? g_gest_pitch : live_pitch; g_seat_latched = true; }   // first person / diorama: the fp heading owns the yaw; the diorama's pitch is the tilt gesture's
            else if (g_follow_pans && advance) {
                // slew the latched yaw toward the live one, rate-limited (once per frame: an honest deg/s)
                float d = live_yaw - g_seat_yaw;
                while (d >  3.14159265f) d -= 6.2831853f;
                while (d < -3.14159265f) d += 6.2831853f;
                const float step = g_follow_rate * 0.0174533f * g_dt;
                g_seat_yaw += fmaxf(-step, fminf(step, d));
                g_seat_pitch += (live_pitch - g_seat_pitch) * fminf(1.f, g_dt * 2.f);
            }
            // Frame from the latched yaw: forward (-sin yaw, 0, -cos yaw), right = f x up, then pitch about right.
            const float fy[3] = { -sinf(g_seat_yaw), 0.f, -cosf(g_seat_yaw) };
            const float up[3] = { 0.f, 1.f, 0.f };
            const float right[3] = { fy[1]*up[2] - fy[2]*up[1], fy[2]*up[0] - fy[0]*up[2], fy[0]*up[1] - fy[1]*up[0] };
            float Rh[9] = { right[0], right[1], right[2],  up[0], up[1], up[2],  -fy[0], -fy[1], -fy[2] };
            if (fabsf(g_seat_pitch) > 1e-4f) {
                const float cp = cosf(g_seat_pitch), sp = sinf(g_seat_pitch);
                float nu[3], nb[3];
                for (int k = 0; k < 3; ++k) { nu[k] = Rh[3+k] * cp + Rh[6+k] * sp; nb[k] = -Rh[3+k] * sp + Rh[6+k] * cp; }
                for (int k = 0; k < 3; ++k) { Rh[3+k] = nu[k]; Rh[6+k] = nb[k]; }
            }
            memcpy(Rb, Rh, sizeof(Rh));                          // position offsets follow the heading frame too
        }
        mul3(MqT, Rb, Rn);
    }
    else memcpy(Rn, Rb, sizeof(Rn));
    if (primary) {
        g_yaw_view = atan2f(Rn[6], Rn[8]);
        float d = g_yaw_view - g_yaw_cam;
        while (d >  3.14159265f) d -= 6.2831853f;
        while (d < -3.14159265f) d += 6.2831853f;
        g_move_delta = fp ? d : 0.f;
    }

    // Move along the new camera's axes (rows of R'; forward is -row 2). The
    // eye shift is a further lateral offset, used for the second eye. With
    // the headset, the head position and the right-eye offset are reference-
    // space vectors: along the base camera's axes (x right, y up, z back).
    const float lateral = xr ? 0.f : g_eye_shift;
    float hx = 0.f, hy = 0.f, hz = 0.f;
    if (xr) {
        if (g_xr_translate) { hx = g_xr_p[0]; hy = g_xr_p[1]; hz = g_xr_p[2]; }
        if (eye2) { hx += g_xr_eye_off[0]; hy += g_xr_eye_off[1]; hz += g_xr_eye_off[2]; }
        const float sc = g_xr_scale * (g_fp_mode == 2 ? g_gest_scale : 1.f); hx *= sc; hy *= sc; hz *= sc;   // only the diorama zooms the world
    }
    if (primary && xr) { memcpy(g_hb_P, P, sizeof(g_hb_P)); memcpy(g_hb_Rb, Rb, sizeof(g_hb_Rb)); g_hb_sc = g_xr_scale * (g_fp_mode == 2 ? g_gest_scale : 1.f); g_hb_valid = fp && g_fp_mode == 1; }
    const float* Rm = xr ? Rb : Rn;                      // manual offsets: heading frame with the headset (up stays up), head frame without
    float mv[3] = { 0.f, 0.f, 0.f };
    if (g_fp_mode == 0)      { for (int i = 0; i < 3; ++i) mv[i] = g_ovr_move[i] + g_gest_move[i]; }   // authored seat: numpad + grab
    else if (g_fp_mode >= 2) { for (int i = 0; i < 3; ++i) mv[i] = g_gest_move[i]; }                  // diorama / third person: the grab offset from the player
    for (int i = 0; i < 3; ++i)
        Pn[i] = P[i] + Rm[0*3+i]*mv[0] + Rn[0*3+i]*lateral + Rm[1*3+i]*mv[1] - Rm[2*3+i]*mv[2]
                     + Rb[0*3+i]*hx + Rb[1*3+i]*hy + Rb[2*3+i]*hz;

    if (g_fp_mode >= 2 && g_diorama_floor > 0.f && !g_gest_active && Pn[1] < P[1] + g_diorama_floor) Pn[1] = P[1] + g_diorama_floor;   // diorama / third person floor
    if (primary && xr) {   // for the VR hands: where the eye is, and which way the head faces (horizontal)
        memcpy(g_hb_eye, Pn, sizeof(g_hb_eye));
        float f[3] = { -Rn[6], 0.f, -Rn[8] }; float l = sqrtf(f[0]*f[0] + f[2]*f[2]); if (l < 1e-4f) { f[0] = 0.f; f[2] = -1.f; l = 1.f; } f[0] /= l; f[2] /= l;
        g_hb_fwd[0] = f[0]; g_hb_fwd[1] = 0.f; g_hb_fwd[2] = f[2]; g_hb_right[0] = -f[2]; g_hb_right[1] = 0.f; g_hb_right[2] = f[0];   // right = forward x up
    }

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

/// Rewrites every camera-derived block in a buffer image. `orig` is the
/// untouched original, `p` the copy being patched, `base` the byte offset of
/// the view matrix (64 in the common layout, 48 in the shifted one), `v` the
/// 3x4 view to base the override on. Each derived block is replaced only if
/// the original equals the product it is expected to be. Returns a bitmask.
// Auto depth for the stereo screen (see g_s3_auto). `v` the game's 3x4 view, `Pj` its projection.
static void screen3d_auto(const float v[12], const float* Pj) {
    const float R[9] = { v[0], v[1], v[2],  v[4], v[5], v[6],  v[8], v[9], v[10] };
    const float t[3] = { v[3], v[7], v[11] };
    float P[3]; for (int i = 0; i < 3; ++i) P[i] = -(R[0*3+i]*t[0] + R[1*3+i]*t[1] + R[2*3+i]*t[2]);
    const float P0 = fabsf(Pj[0]), P5 = fabsf(Pj[5]);
    float dnear = 1e9f, dsub = 0.f, best_c = 1e9f; int vis = 0; int best_i = -1;
    float same_c = 1e9f, same_d = 0.f; int same_i = -1;   // the actor nearest where last frame's subject stood
    {   // (no lock: write_camera runs under g_mutex already - ffxvr_on_updatesub takes it; relocking crashed the game)
        for (int i = 0; i < g_world_report_n; ++i) {
            const float w[3] = { g_world_report[i][0] - P[0], g_world_report[i][1] - P[1], g_world_report[i][2] - P[2] };
            const float x = R[0]*w[0] + R[1]*w[1] + R[2]*w[2], y = R[3]*w[0] + R[4]*w[1] + R[5]*w[2], d = -(R[6]*w[0] + R[7]*w[1] + R[8]*w[2]);
            if (d < 5.f) continue;
            const float nx = P0 * x / d, ny = P5 * y / d;
            if (fabsf(nx) > 1.1f || fabsf(ny) > 1.1f) continue;   // outside the picture (a little margin: the stereo shift)
            vis++;
            if (d < dnear) dnear = d;
            const float c = nx * nx + ny * ny; if (c < best_c) { best_c = c; dsub = d; best_i = i; }
            if (g_s3_subj_valid) {
                const float sx = g_world_report[i][0] - g_s3_subj_pos[0], sy = g_world_report[i][1] - g_s3_subj_pos[1], sz = g_world_report[i][2] - g_s3_subj_pos[2];
                if (sx*sx + sy*sy + sz*sz < 40.f * 40.f && c < same_c) { same_c = c; same_d = d; same_i = i; }
            }
        }
    }
    // Sticky subject: keep last frame's subject while it is in view, unless another actor has been clearly
    // nearer the centre (by 0.15 in ndc^2) for half a second; a cut resets it at once.
    const bool cut_now = g_cut_count != g_s3_cut_seen;
    if (same_i >= 0 && !cut_now) {
        if (best_i != same_i && best_c + 0.15f < same_c) g_s3_cand_t += g_dt; else g_s3_cand_t = 0.f;
        if (g_s3_cand_t < 0.5f) { dsub = same_d; best_i = same_i; } else g_s3_cand_t = 0.f;
    }
    else g_s3_cand_t = 0.f;
    if (best_i >= 0) { memcpy(g_s3_subj_pos, g_world_report[best_i], sizeof(g_s3_subj_pos)); g_s3_subj_valid = true; } else g_s3_subj_valid = false;
    g_s3_vis = vis; g_s3_near = vis ? dnear : 0.f; g_s3_subject = dsub;
    // Disparity of a point at depth d, as a fraction of the screen width: P0 * sep * (1/d - 1/conv) / 2
    // (positive = in front of the screen). Two limits:
    //  - background: the far distance (d -> inf) must not push the eyes apart beyond their own spacing on the
    //    physical screen - 0.8 x 64 mm over the screen's width in metres - or they diverge;
    //  - foreground: the nearest actor's pop-out stays under g_s3_max_pop, by moving the screen plane forward
    //    toward it (the depth is kept; the subject slips a little behind the screen) rather than flattening.
    const float bg_max = 0.8f * 0.064f / fmaxf(1.f, g_xr_screen_width);
    float conv = vis ? fmaxf(60.f, fminf(2000.f, dsub)) : g_s3_plane_set;
    float sep = 0.f;
    for (int pass = 0; pass < 2; ++pass) {
        sep = g_s3_depth * conv / 30.f;
        if (P0 > 0.f) sep = fminf(sep, 2.f * bg_max * conv / P0);
        if (vis && dnear < conv && P0 > 0.f && sep > 0.f) {
            const float inv = 1.f / dnear - 2.f * g_s3_max_pop / (P0 * sep);   // 1/conv must be >= this
            if (inv > 1.f / conv) conv = fmaxf(dnear, 1.f / inv);
        }
    }
    // ease (the plane slowly: a jump in the plane is what reads as a wobble; the separation a little faster,
    // since it is what keeps a close actor fusible), snap on a cut
    const bool cut = cut_now; g_s3_cut_seen = g_cut_count;
    const float kc = cut ? 1.f : fminf(1.f, g_dt / 0.8f), ks = cut ? 1.f : fminf(1.f, g_dt / 0.25f);
    g_s3_conv_cur += (conv - g_s3_conv_cur) * kc;
    g_s3_sep_cur  += (sep  - g_s3_sep_cur)  * ks;
    // the eased pair must still respect the pop-out limit this frame (the plane lags the target on purpose)
    if (vis && dnear < g_s3_conv_cur && P0 > 0.f) {
        const float cap = 2.f * g_s3_max_pop / (P0 * (1.f / dnear - 1.f / g_s3_conv_cur));
        if (g_s3_sep_cur > cap) g_s3_sep_cur = fmaxf(0.f, cap);
    }
    g_screen3d_conv = g_s3_conv_cur; g_screen3d_sep = g_s3_sep_cur;
}

static unsigned write_camera(const uint8_t* orig, uint8_t* p, UINT size, UINT base, const float v[12]) {
    if (g_screen3d_now && g_s3_auto && g_eye_shift == 0.f && g_s3_frame != g_frames) { g_s3_frame = g_frames; screen3d_auto(v, reinterpret_cast<const float*>(orig + base + 64)); }
    float Vn[16], In[16], Pn[3];
    bool advance = false;
    if (g_eye_shift == 0.f) {   // eye 1: the first upload of a real 3D camera in this frame, on the render thread, moves the filters on
        g_view_uploads++;
        const float* mp = reinterpret_cast<const float*>(orig + base + 192);   // main projection (|P0|,|P5| <= 0.2: the intro's near-2D camera)
        const bool real3d = fabsf(mp[0]) > 0.2f && fabsf(mp[5]) > 0.2f;
        const bool rt = g_render_tid == 0 || GetCurrentThreadId() == g_render_tid;
        if (g_view_tick_frame != g_frames && real3d && rt) { advance = true; g_view_tick_frame = g_frames; g_view_ticks++; memcpy(g_tick_v, v, sizeof(g_tick_v)); }
        else if (g_view_tick_frame == g_frames && real3d && memcmp(v, g_tick_v, sizeof(g_tick_v)) != 0) g_view_othercam++;
        else if (g_view_tick_frame != g_frames) g_view_early++;   // before the tick (not 3D, or another thread): drawn with last frame's anchor
    }
    build_override_view(v, Vn, In, Pn, advance);

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
    const bool fov = g_xr_active && g_ovr_enabled && g_xr_use_fov && !g_screen3d_now;
    if (g_screen3d_now && fabsf(Pj[0]) > 0.2f && g_screen3d_conv > 1.f) {
        // Stereo screen: shift each eye's frustum so a point on the centre line at distance conv maps to the
        // image centre (ndc_x = P[0]*t - P[2], t the tangent of that point seen from the eye: +-half/conv).
        const float half = 0.5f * g_screen3d_sep;
        const float tt = ((g_eye_shift != 0.f) ? -half : half) / g_screen3d_conv;
        auto shift = [&](float* P) { P[2] += P[0] * tt; };
        shift(Pjn); memcpy(p + o_projn, Pjn, 64);
        if (size >= o_proj + 64) { float Pm[16]; memcpy(Pm, orig + o_proj, 64); shift(Pm); memcpy(p + o_proj, Pm, 64); }
    }
    const XrFovTan& f = g_xr_fov_used[(g_eye_shift != 0.f) ? 1 : 0];
    const bool fov_ok = std::isfinite(f.l) && std::isfinite(f.r) && std::isfinite(f.u) && std::isfinite(f.d)
                     && f.r - f.l > 0.1f && f.u - f.d > 0.1f && f.r - f.l < 20.f && f.u - f.d < 20.f
                     && fabsf(Pj[0]) > 0.2f && fabsf(Pj[5]) > 0.2f;   // a real camera, not the intro's near-2D projections
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
/// Replaces the frame's known proj*view (and proj*view*X for affine blocks X) in a buffer with no recognised camera.
/// Reads `rd`, writes `wr`: the first hit copies rd into wr (unless they are the same buffer), so a buffer with
/// nothing of ours in it (the common case) costs no copy. Returns the number of blocks replaced.
static int replace_known_products(const uint8_t* rd, uint8_t* wr, UINT size, const float* PVn) {
    if (!g_have_PV) return 0;
    int n = 0;
    const UINT nblocks = size / 16;
    bool copied = false;
    auto take = [&]() { if (!copied) { if (wr != rd) memcpy(wr, rd, size); copied = true; } };   // never memcpy onto itself
    auto cur  = [&]() -> const uint8_t* { return copied ? wr : rd; };                            // later passes see earlier replacements
    const float tol_pv = same4_tol_of(g_last_PV);

    // Pass 1: bare proj*view.
    for (UINT off = 0; off + 64 <= size; off += 16) {
        if (same4_tol(reinterpret_cast<const float*>(cur() + off), g_last_PV, tol_pv)) {
            take();
            memcpy(wr + off, PVn, 64);
            n++;
        }
    }

    // Pass 2: proj*view*X for each affine-looking block X in the buffer.
    // Buffers of this kind are small, so the quadratic search is cheap.
    if (nblocks > 64) return n;
    for (UINT xo = 0; xo + 64 <= size; xo += 16) {
        const float* X = reinterpret_cast<const float*>(cur() + xo);
        if (X[12] != 0.f || X[13] != 0.f || X[14] != 0.f || fabsf(X[15] - 1.f) > 1e-4f) continue;   // not affine
        if (same4_tol(X, g_last_PV, tol_pv)) continue;
        float PVX[16], PVnX[16];
        mul4(g_last_PV, X, PVX);
        const float tol_x = same4_tol_of(PVX);
        bool used = false;
        for (UINT off = 0; off + 64 <= size; off += 16) {
            if (off == xo) continue;
            if (same4_tol(reinterpret_cast<const float*>(cur() + off), PVX, tol_x)) {
                if (!used) { mul4(PVn, X, PVnX); used = true; }   // X is unchanged in either buffer (only off != xo is written)
                take();
                memcpy(wr + off, PVnX, 64);
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

/// Every page of [p, p+len) is committed and readable (no guard / no-access). The stage-0 debugger makes an
/// access violation fatal even inside __try, so this check, not SEH, is what keeps a stale pointer harmless.
static bool range_readable(const void* p, size_t len) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    if (p == nullptr || len == 0 || a < 0x10000 || a + len < a) return false;
    uintptr_t q = a;
    while (q < a + len) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<const void*>(q), &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        const DWORD prot = mbi.Protect & 0xFF;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        if (prot != PAGE_READONLY && prot != PAGE_READWRITE && prot != PAGE_EXECUTE_READ && prot != PAGE_EXECUTE_READWRITE && prot != PAGE_WRITECOPY && prot != PAGE_EXECUTE_WRITECOPY) return false;
        q = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

/// Copies `len` bytes from game memory. Returns 1 on success, 0 if the range
/// is not readable (stale battle pointers, an uninitialised actor table at
/// start-up). The pages are checked with VirtualQuery first: Fahrenheit's
/// stage-0 is a debugger and an access violation is fatal under it even
/// inside __try, so faulting on purpose is not an option.
extern "C" __declspec(dllexport) int ffxvr_safe_read(const void* src, void* dst, unsigned len) {
    if (src == nullptr || dst == nullptr || len == 0) return 0;
    if (!range_readable(src, len)) return 0;
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
        build_override_view(v, V, In, Pn, false);   // read-only: the filters advance once per frame in write_camera
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
    if (out == nullptr || max <= 0) return 0;
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

struct FrameEv { char text[400]; };
std::vector<FrameEv> g_frame_ev;
bool  g_frame_arm = false, g_frame_live = false;
// Draw arguments of the call being reported (set by ffxvr_draw_args just before ffxvr_on_draw), and
// vertex / index buffer contents captured through Map/Unmap while a frame census is armed, so the
// census can print the 2D UI quads' screen positions (the cursor is a textureless quad among others).
bool g_vb_capture = false; unsigned g_vb_maps = 0, g_vb_updates = 0;   // geometry copies taken (Map/Unmap, UpdateSubresource) while armed
std::map<ID3D11Buffer*, std::vector<uint8_t>> g_vbufs;
// The 2D UI's own vertex streams (learned from UI draws). Copying them at every Unmap was far too slow
// (the streams are big dynamic ring buffers in write-combined memory: reading them froze battles), so
// only the mapped pointer is remembered; a hide rule reads the few vertices it needs through a guarded
// read, within the frame the game wrote them (the upload ring is not reused until the GPU is done).
// R9: a record is re-checked for readability (VirtualQuery) once per frame before the guarded reads, and dropped
// when a new buffer is created at its address (hk_CreateBuffer). No age window: the game maps the UI ring ahead,
// and static UI can keep an old mapping (design-notes, Picks, 21 Oct).
struct VbMap { void* p = nullptr; UINT size = 0; uint32_t frame = 0; uint32_t last_draw = 0; uint32_t checked_frame = ~0u; bool readable = false; };
std::set<ID3D11Buffer*> g_ui_vbs;
std::map<ID3D11Buffer*, VbMap> g_vb_last_map;
std::atomic<bool> g_ui_vbs_any{ false };     // set at the first g_ui_vbs insert; read without the lock in hk_CreateBuffer
uint32_t g_vb_purged = 0, g_vb_unreadable = 0, g_rect_age_max = 0;   // heartbeat diagnostics
/// The record's mapping is still readable: checked once per frame per buffer (one syscall), under g_mutex.
static bool vb_live(VbMap& m) {
    m.last_draw = g_frames;
    if (m.checked_frame != g_frames) { m.checked_frame = g_frames; m.readable = range_readable(m.p, m.size); if (!m.readable) g_vb_unreadable++; }
    return m.readable;
}
static void ui_vbs_learn(ID3D11Buffer* vb) { if (g_ui_vbs.insert(vb).second) g_ui_vbs_any.store(true, std::memory_order_relaxed); }   // under g_mutex
std::map<void*, std::pair<ID3D11Buffer*, UINT>> g_vb_in_flight;
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
    // The backbuffer is identified by what it is, not by when it is bound:
    // a swapchain buffer is the only render target the game uses that has no
    // shader-resource binding (every texture the game renders to has one).
    // Timing-based identification broke whenever the overlay drew nothing.
    if (n > 0 && rtvs && rtvs[0]) {
        ID3D11Texture2D* t = texture_of(static_cast<ID3D11View*>(rtvs[0]));
        if (t && t != g_backbuffer) {
            D3D11_TEXTURE2D_DESC d; t->GetDesc(&d);
            if ((d.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0 && (d.BindFlags & D3D11_BIND_RENDER_TARGET) && d.Width >= 640 && d.SampleDesc.Count == 1) {
                g_backbuffer = t; g_backbuffer_seen_this_frame = true;
                if (g_diag7 && g_bb_desc_logged < 10) {   // build 7 (6.7): the prediction is format 28 (R8G8B8A8_UNORM; 29 would be _SRGB)
                    g_bb_desc_logged++;
                    const DWORD tid = GetCurrentThreadId();
                    diag("backbuffer: %p %ux%u format %u samples %u mips %u array %u bind 0x%X usage %u misc 0x%X cpu 0x%X (frame %u, %s thread)", static_cast<void*>(t), d.Width, d.Height,
                         static_cast<unsigned>(d.Format), d.SampleDesc.Count, d.MipLevels, d.ArraySize, d.BindFlags, static_cast<unsigned>(d.Usage), d.MiscFlags, d.CPUAccessFlags, g_frames,
                         tid == g_render_tid ? "render" : tid == g_main_tid ? "main" : "another");
                }
            }
        }
        else if (t == g_backbuffer) g_backbuffer_seen_this_frame = true;
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
    if (!g_in_eye2) { if (g_overlay_phase) gpu_timing_start_span(); g_overlay_phase = false; }
    if (!g_frame_live) return;
    char a[96]; describe_view(static_cast<ID3D11View*>(view), a, sizeof(a));
    char t[160];
    snprintf(t, sizeof(t), "CLR %s %s", depth ? "depth" : "color", a);
    std::lock_guard<std::mutex> lock(g_mutex);
    frame_event(t);
}

extern "C" __declspec(dllexport) void ffxvr_dump_cb(unsigned size, int count) { g_dump_cb_size = size; g_dump_cb_left = count; }
/// Hides 2D UI draws whose texture is w x h (w = 0: off); notex also hides the textureless draws.
extern "C" __declspec(dllexport) void ffxvr_ui_hide(unsigned w, unsigned h, int notex) { g_ui_hide_w = w; g_ui_hide_h = h; g_ui_hide_notex = notex; }
extern "C" __declspec(dllexport) void ffxvr_ui_hide_cursor(int on) { g_ui_hide_cursor = on != 0; }
/// Replaces the hide list: `count` entries of 6 floats each (tex_w, tex_h, w0, w1, h0, h1).
extern "C" __declspec(dllexport) void ffxvr_ui_hide_list(const float* v, int count) {
    if (v == nullptr) count = 0;
    g_hide_sig_n = 0;
    for (int i = 0; i < count && i < 16; ++i) { const float* e = v + i * 6; g_hide_sigs[g_hide_sig_n++] = { (unsigned)e[0], (unsigned)e[1], e[2], e[3], e[4], e[5] }; }
}
extern "C" __declspec(dllexport) void ffxvr_ui_hide_pointers(int on) { g_hide_pointers = on != 0; }
/// Blitzball overlay routing (see g_blitz_overlay); returns the draws routed last frame.
extern "C" __declspec(dllexport) unsigned ffxvr_blitz_overlay(int on) { g_blitz_overlay = on != 0; return g_ui_early_report; }
/// (pointer-rule hits since the last call) | (actors projected this frame) << 16
extern "C" __declspec(dllexport) unsigned ffxvr_ui_pointer_stats() { unsigned n = (g_pointer_hits & 0xFFFF) | ((unsigned)g_actor_ui_n << 16); g_pointer_hits = 0; return n; }
/// After ffxvr_on_draw returned with bit 17 set: the index ranges of the draw to leave out, `max` (first, count) pairs at most,
/// relative to the draw's start, sorted and apart (R39). Returns the pair count.
extern "C" __declspec(dllexport) int ffxvr_draw_skip_ranges(unsigned* out, int max) {
    if (!out || max <= 0) return 0;
    const int n = g_skip_n < max ? g_skip_n : max;
    for (int i = 0; i < n; ++i) { out[i * 2] = g_skip[i].first; out[i * 2 + 1] = g_skip[i].count; }
    return n;
}
/// The single range of ABI 4, kept as a shim (R39): the envelope of the ranges (it also hides the quads between them).
extern "C" __declspec(dllexport) void ffxvr_draw_skip(unsigned* first, unsigned* count) {
    if (!first || !count) return;
    if (g_skip_n <= 0) { *first = 0; *count = 0; return; }
    *first = g_skip[0].first; *count = g_skip[g_skip_n - 1].first + g_skip[g_skip_n - 1].count - g_skip[0].first;
}
/// Hits per hide-list entry since the last call (up to 16), for the overlay.
extern "C" __declspec(dllexport) void ffxvr_ui_hide_hits(unsigned* out, int count) { if (!out) return; for (int i = 0; i < count && i < 16; ++i) { out[i] = g_hide_sig_hits[i]; g_hide_sig_hits[i] = 0; } }
static bool guarded_read_xy(const uint8_t* base, UINT stride, float* xy8);   // below
// Exactly n (<= 4) contiguous floats under an SEH guard (the F9 pick's stream dump). No C++ objects (SEH).
static bool guarded_read_floats(const uint8_t* p, float* out, unsigned n) {
    __try { for (unsigned k = 0; k < n && k < 4; ++k) out[k] = reinterpret_cast<const float*>(p)[k]; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
/// Arms a pick at a normalised backbuffer position (0..1, y down) for the next frame's UI pass.
extern "C" __declspec(dllexport) void ffxvr_ui_pick(float nx, float ny) { g_pick_nx = nx; g_pick_ny = ny; g_pick_hits = 0; g_pick_scene_text = 0; g_pick_best_d = 1e9f; g_pick_best[0] = 0; g_pick_summary[0] = 0; g_pick_armed = true; }
extern "C" __declspec(dllexport) void ffxvr_ui_atlas_dump() { g_atlas_dump_armed = true; g_atlas_dumped_n = 0; }
static void dump_dds(ID3D11Texture2D* tex, const wchar_t* name) {
    D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_BC3_UNORM && d.Format != DXGI_FORMAT_BC3_UNORM_SRGB && d.Format != DXGI_FORMAT_BC1_UNORM && d.Format != DXGI_FORMAT_BC1_UNORM_SRGB && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM
        && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) { diag("atlas dump %ls: format %u, skipped", name, (unsigned)d.Format); return; }
    D3D11_TEXTURE2D_DESC sd = d; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0; sd.MipLevels = 1; sd.ArraySize = 1;
    ID3D11Texture2D* st = nullptr; if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &st)) || !st) return;
    { InEye2 guard; g_ctx->CopySubresourceRegion(st, 0, 0, 0, 0, tex, 0, nullptr); }
    D3D11_MAPPED_SUBRESOURCE ms = {};
    if (SUCCEEDED(g_orig_map(g_ctx, st, 0, D3D11_MAP_READ, 0, &ms))) {
        const bool bc = d.Format == DXGI_FORMAT_BC3_UNORM || d.Format == DXGI_FORMAT_BC3_UNORM_SRGB || d.Format == DXGI_FORMAT_BC1_UNORM || d.Format == DXGI_FORMAT_BC1_UNORM_SRGB;
        const bool bc3 = d.Format == DXGI_FORMAT_BC3_UNORM || d.Format == DXGI_FORMAT_BC3_UNORM_SRGB;
        const UINT bw = bc ? (d.Width + 3) / 4 : d.Width, bh = bc ? (d.Height + 3) / 4 : d.Height, bpb = bc ? (bc3 ? 16 : 8) : 4;
        wchar_t path[MAX_PATH]; _snwprintf_s(path, _TRUNCATE, L"%s\\%s", g_dir, name);
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"wb") == 0 && f) {
            uint32_t h[32] = {}; h[0] = 124; h[1] = 0x1 | 0x2 | 0x4 | 0x1000 | (bc ? 0x80000 : 0x8); h[2] = d.Height; h[3] = d.Width; h[4] = bc ? bw * bpb * bh : d.Width * 4; h[6] = 1;
            // DDS_PIXELFORMAT starts at header dword 18 (size, flags, fourCC, bit count, R/G/B/A masks); caps at 26
            h[18] = 32; h[19] = bc ? 0x4 : 0x41; h[20] = bc ? (bc3 ? 0x35545844u : 0x31545844u) : 0; h[21] = bc ? 0 : 32;
            if (!bc) {   // channel masks by format: RGBA has red in the low byte (R42b)
                const bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                h[22] = rgba ? 0x000000FFu : 0x00FF0000u; h[23] = 0x0000FF00u; h[24] = rgba ? 0x00FF0000u : 0x000000FFu; h[25] = 0xFF000000u;
            }
            h[26] = 0x1000;
            fwrite("DDS ", 1, 4, f); fwrite(h, 4, 31, f);
            for (UINT y = 0; y < bh; ++y) fwrite(static_cast<const uint8_t*>(ms.pData) + y * ms.RowPitch, 1, bw * bpb, f);
            fclose(f);
            diag("atlas dump: %ls (%ux%u fmt %u)", name, d.Width, d.Height, (unsigned)d.Format);
        }
        g_orig_unmap(g_ctx, st, 0);
    }
    st->Release();
}
/// Hover highlight for this frame: mode 0 off, 1 the quads under (nx, ny), 2 every UI quad (the hit ones flagged).
extern "C" __declspec(dllexport) void ffxvr_ui_hover(float nx, float ny, int mode) { g_hover_nx = nx; g_hover_ny = ny; g_hover_mode = mode; }
/// The rectangles collected this frame: 7 floats each (x0 y0 x1 y1 tex_w tex_h hit), in UI units; returns the count and clears.
extern "C" __declspec(dllexport) int ffxvr_ui_hover_rects(float* out, int max) {
    if (out == nullptr || max <= 0) { g_hover_n = 0; return 0; }   // still clear, or the rects pile up and collection stops
    const int n = g_hover_n < max ? g_hover_n : max;
    for (int i = 0; i < n; ++i) { const HoverRect& r = g_hover_rects[i]; out[i*7] = r.x0; out[i*7+1] = r.y0; out[i*7+2] = r.x1; out[i*7+3] = r.y1; out[i*7+4] = (float)r.tw; out[i*7+5] = (float)r.th; out[i*7+6] = (float)r.hit; }
    g_hover_n = 0;
    return n;
}

// Collects this draw's quads for the hover highlight.
static unsigned ui_sprite_xy(float* xy, unsigned max_v);   // below
// R39: this draw's quads (up to k_ui_max_q), read once per draw and shared by the hover collector, the hide list and the
// pointer rule (the streams are write-combined memory: reads are slow). Returns the vertex count; *xy is valid until the next draw.
static unsigned ui_quads_cached(const float** xy) {
    if (g_ui_xy_draw != g_draw_in_frame || g_ui_xy_frame != g_frames) {
        g_ui_xy_draw = g_draw_in_frame; g_ui_xy_frame = g_frames;
        if (g_draw_start != 0) g_ui_start_nonzero++;
        if (g_draw_base != 0) g_ui_base_nonzero++;   // R48 reads these at the base now (they were read at vertex 0)
        unsigned fv = 0; if (!ui_first_v(&fv)) g_ui_start_bail++;
        g_ui_xy_n = ui_sprite_xy(g_ui_xy, k_ui_max_q * 4);
        g_ui_verts_read += g_ui_xy_n;
    }
    *xy = g_ui_xy;
    return g_ui_xy_n;
}

// Projects this frame's actor positions (the translations of the skinned world matrices seen in the scene
// pass) through the game's own camera into UI units. Once per frame, at the first UI draw.
static void actors_project_ui() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_actor_ui_n = 0;
    if (!g_cam_valid) return;
    float V[16]; memcpy(V, g_cam, 48); V[12] = V[13] = V[14] = 0.f; V[15] = 1.f;
    const float* P = &g_cam[16];
    for (int i = 0; i < g_world_live_n && g_actor_ui_n < k_world_max; ++i) {
        const float w[4] = { g_world_live[i][0], g_world_live[i][1], g_world_live[i][2], 1.f };
        float e[4], c[4];
        for (int r = 0; r < 4; ++r) e[r] = V[r*4+0]*w[0] + V[r*4+1]*w[1] + V[r*4+2]*w[2] + V[r*4+3]*w[3];
        for (int r = 0; r < 4; ++r) c[r] = P[r*4+0]*e[0] + P[r*4+1]*e[1] + P[r*4+2]*e[2] + P[r*4+3]*e[3];
        if (c[3] <= 1e-4f) continue;
        const float ux = c[0] / c[3] * 960.f, uy = -c[1] / c[3] * 540.f;
        if (!std::isfinite(ux) || !std::isfinite(uy) || fabsf(ux) > 1500.f || fabsf(uy) > 900.f) continue;
        g_actor_ui[g_actor_ui_n][0] = ux; g_actor_ui[g_actor_ui_n][1] = uy; g_actor_ui_src[g_actor_ui_n] = i; g_actor_ui_n++;   // src: off-screen actors are skipped, so keep the g_world_live index
    }
}

// Records this draw's large quads as window backgrounds (called for every UI draw while a rule is on).
static void ui_backgrounds_note(const float* xy, unsigned nv) {
    static int last_draw = -1;
    if (g_bg_frame != (int)g_frames) { g_bg_frame = (int)g_frames; g_bg_n = 0; last_draw = -1; }
    if (last_draw == g_draw_in_frame) return;   // once per draw: the hover collector and the pointer rule both call this (R42a)
    last_draw = g_draw_in_frame;
    for (unsigned q = 0; q < nv / 4 && g_bg_n < 48; ++q) {
        float x0 = xy[q*8], x1 = x0, y0 = xy[q*8+1], y1 = y0;
        for (unsigned k = 1; k < 4; ++k) { const float x = xy[q*8+k*2], y = xy[q*8+k*2+1]; if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
        if (!std::isfinite(x0) || !std::isfinite(x1) || !std::isfinite(y0) || !std::isfinite(y1)) continue;
        if (x1 - x0 >= 100.f && y1 - y0 >= 100.f && x1 - x0 < 1900.f) g_bg_rects[g_bg_n++] = { x0, y0, x1, y1 };
    }
}
static bool quad_on_window(float x0, float y0, float x1, float y1) {
    if (g_bg_frame != (int)g_frames) return false;
    for (int i = 0; i < g_bg_n; ++i) { const BgRect& b = g_bg_rects[i]; if (x0 >= b.x0 - 4.f && x1 <= b.x1 + 4.f && y0 >= b.y0 - 4.f && y1 <= b.y1 + 4.f) return true; }
    return false;
}

// Quad q's rectangle within a draw's vertex list.
static void quad_rect(const float* xy, unsigned q, float* x0, float* y0, float* x1, float* y1) {
    *x0 = *x1 = xy[q*8]; *y0 = *y1 = xy[q*8+1];
    for (unsigned k = 1; k < 4; ++k) { const float x = xy[q*8+k*2], y = xy[q*8+k*2+1]; if (x < *x0) *x0 = x; if (x > *x1) *x1 = x; if (y < *y0) *y0 = y; if (y > *y1) *y1 = y; }
}
// Pointers are solitary; damage numbers are runs of digit quads side by side (same height, touching) in one
// draw, and they float above the target exactly where a pointer would. A quad with such a neighbour is a glyph.
static bool quad_in_run(const float* xy, unsigned nq, unsigned q) {
    float x0, y0, x1, y1; quad_rect(xy, q, &x0, &y0, &x1, &y1); const float h = y1 - y0;
    for (unsigned o = 0; o < nq; ++o) {
        if (o == q) continue;
        float ox0, oy0, ox1, oy1; quad_rect(xy, o, &ox0, &oy0, &ox1, &oy1);
        const float oh = oy1 - oy0; if (fabsf(oh - h) > 0.35f * h) continue;                       // similar height
        const float gap = ox0 > x1 ? ox0 - x1 : x0 > ox1 ? x0 - ox1 : 0.f; if (gap > 12.f) continue;   // touching or nearly
        const float vo = (y1 < oy1 ? y1 : oy1) - (y0 > oy0 ? y0 : oy0); if (vo > 0.5f * h) return true;   // on the same line
    }
    return false;
}

// Is this quad (UI units) a battle pointer by the pointer rule? `actor` gets the index of the actor it is over.
static bool quad_is_pointer(float x0, float y0, float x1, float y1, int* actor = nullptr) {
    const float w = x1 - x0, h = y1 - y0;
    if (w > 90.f || h > 90.f || w < 1.f || h < 8.f) return false;
    if (quad_on_window(x0, y0, x1, y1)) return false;
    const float cx = 0.5f * (x0 + x1);
    int best = -1; float best_d = 1e9f;
    for (int i = 0; i < g_actor_ui_n; ++i) {
        const float dx = cx - g_actor_ui[i][0], above = g_actor_ui[i][1] - y1;   // the actor's root projects below the pointer
        if (fabsf(dx) < 55.f && above > -30.f && above < 420.f && fabsf(dx) + above * 0.1f < best_d) { best_d = fabsf(dx) + above * 0.1f; best = i; }
    }
    if (actor) *actor = best >= 0 ? g_actor_ui_src[best] : -1;   // an index into g_world_live (R44), not into g_actor_ui
    return best >= 0;
}

static void ui_hover_collect() {
    if (g_hover_mode == 0 || g_hover_n >= 512) return;
    if (g_actor_ui_frame != (int)g_frames) { g_actor_ui_frame = (int)g_frames; actors_project_ui(); }
    if (g_ui_draws_frame == 0) {   // once per frame: where the actors project through the game camera (flag 4: a cross)
        for (int i = 0; i < g_actor_ui_n && g_hover_n < 512; ++i) g_hover_rects[g_hover_n++] = { g_actor_ui[i][0] - 6.f, g_actor_ui[i][1] - 6.f, g_actor_ui[i][0] + 6.f, g_actor_ui[i][1] + 6.f, 0, 0, 4u };
    }
    const float* xy = nullptr; const unsigned nv = ui_quads_cached(&xy); if (nv == 0) return;
    const float px = g_hover_nx * 1920.f - 960.f, py = g_hover_ny * 1080.f - 540.f;
    ID3D11Texture2D* t = texture_of(g_cur_ps_srv[0]); D3D11_TEXTURE2D_DESC td = {}; if (t) t->GetDesc(&td);
    if (!t || td.Width == 1024 || td.Width == 8192) ui_backgrounds_note(xy, nv);
    const bool pointer_kind = !t || (td.Width == 1024 && td.Height == 1024);
    for (unsigned q = 0; q < nv / 4 && g_hover_n < 512; ++q) {
        float x0 = xy[q*8], x1 = x0, y0 = xy[q*8+1], y1 = y0; bool ok = true;
        for (unsigned k = 0; k < 4; ++k) { const float x = xy[q*8+k*2], y = xy[q*8+k*2+1]; if (!std::isfinite(x) || !std::isfinite(y) || fabsf(x) > 5000.f || fabsf(y) > 5000.f) { ok = false; break; } if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
        if (!ok) continue;
        const bool hit = px >= x0 - 4.f && px <= x1 + 4.f && py >= y0 - 4.f && py <= y1 + 4.f;
        const bool ptr = pointer_kind && quad_is_pointer(x0, y0, x1, y1) && !quad_in_run(xy, nv / 4, q);   // flag 2: the pointer rule would hide it
        if (g_hover_mode == 1 && !hit && !ptr) continue;
        g_hover_rects[g_hover_n++] = { x0, y0, x1, y1, td.Width, td.Height, (hit ? 1u : 0u) | (ptr ? 2u : 0u) };
    }
}

/// The last pick's summary (one line per hit), for the overlay.
extern "C" __declspec(dllexport) const char* ffxvr_ui_pick_result() { return g_pick_summary; }
extern "C" __declspec(dllexport) void ffxvr_slowmo(int ms) { g_slowmo_ms = ms < 0 ? 0 : ms > 2000 ? 2000 : ms; }

// Up to `max_v` vertex x,y pairs of the sprite about to be drawn (quads are laid out 4 vertices each in
// the slot-0 stream at the bound offset). Returns the count read, 0 if the stream is unknown.
static unsigned ui_sprite_xy(float* xy, unsigned max_v) {
    ID3D11Buffer* vb = nullptr; UINT stride = 0, voff = 0; g_ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &voff);
    if (!vb) return 0;
    const uint8_t* src = nullptr; unsigned nv = 0; uint32_t age = 0;
    unsigned first_v = 0; const bool fv_ok = ui_first_v(&first_v);   // R48: base vertex and a quad-aligned start honoured, else not read
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ui_vbs_learn(vb);
        auto it = g_vb_last_map.find(vb);
        if (fv_ok && it != g_vb_last_map.end() && it->second.p && stride >= 8 && vb_live(it->second)) {
            nv = (g_draw_n / 6) * 4; if (nv > max_v) nv = max_v; if (nv < 4) nv = 4;
            if ((uint64_t)voff + ((uint64_t)first_v + nv) * stride <= it->second.size) { src = static_cast<const uint8_t*>(it->second.p) + voff + (size_t)first_v * stride; age = g_frames - it->second.frame; } else nv = 0;
        }
    }
    vb->Release();
    if (!src) return 0;
    for (unsigned k = 0; k < nv; k += 4) { if (!guarded_read_xy(src + k * stride, stride, xy + k * 2)) return k; }
    if (age > g_rect_age_max) g_rect_age_max = age;   // R9 diagnostic: the reads that hide pointers and digits come through here
    return nv;
}

// The pick: called for every UI draw of the armed frame.
static void ui_pick_check(unsigned draw_idx) {
    float xy[2 * 48]; const unsigned nv = ui_sprite_xy(xy, 48);
    const float px = g_pick_nx * 1920.f - 960.f, py = g_pick_ny * 1080.f - 540.f;
    if (nv == 0) return;
    ID3D11Texture2D* t = texture_of(g_cur_ps_srv[0]); D3D11_TEXTURE2D_DESC td = {}; if (t) t->GetDesc(&td);
    // each quad of the draw on its own (sprites are batched): the hit is the quad under the point
    const unsigned nq = nv / 4; float x0 = 0, x1 = 0, y0 = 0, y1 = 0; int hit_q = -1; float best_d = 1e9f;
    for (unsigned q = 0; q < nq; ++q) {
        float qx0 = xy[q*8], qx1 = qx0, qy0 = xy[q*8+1], qy1 = qy0; bool okq = true;
        for (unsigned k = 0; k < 4; ++k) { const float x = xy[q*8+k*2], y = xy[q*8+k*2+1]; if (!std::isfinite(x) || !std::isfinite(y) || fabsf(x) > 5000.f || fabsf(y) > 5000.f) { okq = false; break; } if (x < qx0) qx0 = x; if (x > qx1) qx1 = x; if (y < qy0) qy0 = y; if (y > qy1) qy1 = y; }
        if (!okq) continue;
        const float dx = px < qx0 ? qx0 - px : px > qx1 ? px - qx1 : 0.f, dy = py < qy0 ? qy0 - py : py > qy1 ? py - qy1 : 0.f; const float d = sqrtf(dx * dx + dy * dy);
        if (d < best_d) { best_d = d; hit_q = (int)q; x0 = qx0; x1 = qx1; y0 = qy0; y1 = qy1; }
    }
    if (hit_q < 0) return;
    char head[200];
    snprintf(head, sizeof(head), "ui draw %u quad %d/%u: %s%ux%u rect x %.0f..%.0f y %.0f..%.0f (%.0fx%.0f)", draw_idx, hit_q + 1, g_draw_n / 6, t ? "tex " : "no texture ", td.Width, td.Height, x0, x1, y0, y1, x1 - x0, y1 - y0);
    if (best_d < g_pick_best_d) { g_pick_best_d = best_d; strncpy_s(g_pick_best, head, _TRUNCATE); }
    if (best_d > 6.f) return;
    g_pick_hits++;
    // the whole cb0 (the sprite's atlas rect / transform lives in here somewhere)
    char cb[900]; size_t l = 0; cb[0] = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_vs_cb[0] ? g_buffers.find(g_vs_cb[0]) : g_buffers.end();
        if (it != g_buffers.end()) {
            const float* f = reinterpret_cast<const float*>(it->second.data.data()); const UINT nf = (UINT)(it->second.data.size() / 4);
            l += snprintf(cb + l, sizeof(cb) - l, " cb0 %uB:", (unsigned)it->second.size);
            for (UINT k = 0; k < nf && k < 64 && l < sizeof(cb) - 16; ++k) l += snprintf(cb + l, sizeof(cb) - l, " %.4g", f[k]);
        }
    }
    // the PS constant buffers (not tracked per slot: asked from the context, looked up in the write records)
    char pcb[700]; size_t pl = 0; pcb[0] = 0;
    {
        ID3D11Buffer* pbs[2] = {}; g_ctx->PSGetConstantBuffers(0, 2, pbs);
        std::lock_guard<std::mutex> lock(g_mutex);
        for (int sl = 0; sl < 2; ++sl) {
            if (!pbs[sl]) continue;
            auto it = g_buffers.find(pbs[sl]);
            if (it == g_buffers.end()) pl += snprintf(pcb + pl, sizeof(pcb) - pl, " ps_cb%d: (no record)", sl);
            else {
                const float* f = reinterpret_cast<const float*>(it->second.data.data()); const UINT nf = (UINT)(it->second.data.size() / 4);
                pl += snprintf(pcb + pl, sizeof(pcb) - pl, " ps_cb%d %uB:", sl, (unsigned)it->second.size);
                for (UINT k = 0; k < nf && k < 40 && pl < sizeof(pcb) - 16; ++k) pl += snprintf(pcb + pl, sizeof(pcb) - pl, " %.4g", f[k]);
            }
            pbs[sl]->Release();
        }
    }
    // the other vertex streams, all four corners of the picked quad (UVs / colours must be in one of them), at the same
    // first vertex as stream 0 (R48 / B6-6: base vertex, quad-aligned start, then the quad)
    char st[600]; size_t tl = 0; st[0] = 0;
    unsigned pick_v = 0; const bool pick_v_ok = ui_first_v(&pick_v); pick_v += (unsigned)hit_q * 4;
    for (UINT slot = 1; slot <= 5; ++slot) {
        ID3D11Buffer* sb = nullptr; UINT sstride = 0, soff = 0; g_ctx->IAGetVertexBuffers(slot, 1, &sb, &sstride, &soff);
        if (!sb) continue;
        const uint8_t* ssrc = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            ui_vbs_learn(sb);
            auto it = g_vb_last_map.find(sb);
            if (pick_v_ok && it != g_vb_last_map.end() && it->second.p && sstride >= 4 && (uint64_t)soff + ((uint64_t)pick_v + 4) * sstride <= it->second.size && vb_live(it->second)) ssrc = static_cast<const uint8_t*>(it->second.p) + soff + (size_t)pick_v * sstride;
        }
        sb->Release();
        tl += snprintf(st + tl, sizeof(st) - tl, " v%u(stride %u):", slot, sstride);
        if (!ssrc) { tl += snprintf(st + tl, sizeof(st) - tl, " ?"); continue; }
        const unsigned nf = sstride / 4 < 4 ? sstride / 4 : 4;
        for (unsigned k = 0; k < 4 && tl < sizeof(st) - 80; ++k) {
            float vals[4] = {}; if (!guarded_read_floats(ssrc + k * sstride, vals, nf)) break;   // ends at <= ssrc + 4*sstride: checked above
            const uint32_t* u = reinterpret_cast<const uint32_t*>(vals);
            tl += snprintf(st + tl, sizeof(st) - tl, " [");
            for (unsigned j = 0; j < nf; ++j) {
                if (std::isfinite(vals[j]) && fabsf(vals[j]) < 1e6f && (vals[j] == 0.f || fabsf(vals[j]) > 1e-6f)) tl += snprintf(st + tl, sizeof(st) - tl, "%s%.4g", j ? " " : "", vals[j]);
                else tl += snprintf(st + tl, sizeof(st) - tl, "%s%08X", j ? " " : "", u[j]);
            }
            tl += snprintf(st + tl, sizeof(st) - tl, "]");
        }
    }
    diag("pick (%.0f,%.0f): %s tex=%p fmt %u%s%s%s", px, py, head, static_cast<void*>(t), static_cast<unsigned>(td.Format), cb, pcb, st);   // fmt: the SRV's texture format (build 7, 6.10)
    size_t sl = strlen(g_pick_summary);
    if (sl < sizeof(g_pick_summary) - 260) snprintf(g_pick_summary + sl, sizeof(g_pick_summary) - sl, "%s%s|sig=%u,%u,%.0f,%.0f", sl ? "\n" : "", head, td.Width, td.Height, x1 - x0, y1 - y0);
}

// Reads the x,y of four vertices from a (possibly no longer mapped) pointer under an SEH guard. No C++
// objects in here: __try cannot unwind them. Writes 8 floats.
static bool guarded_read_xy(const uint8_t* base, UINT stride, float* xy8) {
    __try {
        for (unsigned k = 0; k < 4; ++k) { const float* f = reinterpret_cast<const float*>(base + k * stride); xy8[k * 2] = f[0]; xy8[k * 2 + 1] = f[1]; }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The screen rectangle of the sprite about to be drawn: vertices 0..3 of the slot-0 stream at the bound
// offset (every UI sprite is DrawIndexed(6, 0, 0) over a fixed 0,1,2,2,1,3 index pattern, so the quad
// is the first four vertices), read from the stream's last Map pointer if it was mapped this frame.
// False if unknown (the first frame after the stream is learned, or a map from another frame).
static bool ui_sprite_rect(float* x0, float* y0, float* x1, float* y1) {
    ID3D11Buffer* vb = nullptr; UINT stride = 0, voff = 0; g_ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &voff);
    if (!vb) return false;
    bool ok = false; const uint8_t* src = nullptr; uint32_t age = 0;
    unsigned first_v = 0; const bool fv_ok = ui_first_v(&first_v);   // R48
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ui_vbs_learn(vb);   // learn the UI's streams as they are used; pointers are kept from the next Map on
        auto it = g_vb_last_map.find(vb);
        if (fv_ok && it != g_vb_last_map.end() && it->second.p && stride >= 8 && (uint64_t)voff + ((uint64_t)first_v + 4) * stride <= it->second.size && vb_live(it->second)) {
            src = static_cast<const uint8_t*>(it->second.p) + voff + (size_t)first_v * stride;
            age = g_frames - it->second.frame;
        }
        else g_rect_unknown++;
    }
    vb->Release();
    if (!src) return false;
    float xy[8];
    if (!guarded_read_xy(src, stride, xy)) return false;
    if (age > g_rect_age_max) g_rect_age_max = age;   // R9 diagnostic: how old a mapping a successful read used
    *x0 = *x1 = xy[0]; *y0 = *y1 = xy[1];
    for (unsigned k = 1; k < 4; ++k) { if (xy[k*2] < *x0) *x0 = xy[k*2]; if (xy[k*2] > *x1) *x1 = xy[k*2]; if (xy[k*2+1] < *y0) *y0 = xy[k*2+1]; if (xy[k*2+1] > *y1) *y1 = xy[k*2+1]; }
    ok = std::isfinite(*x0) && std::isfinite(*x1) && std::isfinite(*y0) && std::isfinite(*y1);
    return ok;
}
extern "C" __declspec(dllexport) unsigned ffxvr_ui_hidden_draws() { unsigned n = g_ui_hidden_draws; g_ui_hidden_draws = 0; return n; }
/// Cursor-rule bookkeeping: (rect lookups that failed) | (lookups made) << 16, both since the last call.
extern "C" __declspec(dllexport) unsigned ffxvr_ui_rect_stats() { unsigned n = (g_rect_unknown & 0xFFFF) | ((g_rect_checked & 0xFFFF) << 16); g_rect_unknown = 0; g_rect_checked = 0; return n; }

extern "C" __declspec(dllexport) void ffxvr_frame_arm() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_frame_ev.clear();
    g_frame_arm = true;
    g_vb_capture = true;   // geometry buffers are copied at Unmap until the census frame is written
}

/// The arguments of the draw call about to be reported through ffxvr_on_draw (kind 1 DrawIndexed, 2 Draw).
extern "C" __declspec(dllexport) void ffxvr_draw_args(int kind, unsigned n, unsigned start, int base_vertex) { g_draw_kind = kind; g_draw_n = n; g_draw_start = start; g_draw_base = base_vertex; }

static void frame_dump() {
    frame_flush_draws();
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\ffxvr_frame.log", g_dir);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a") != 0 || f == nullptr) return;
    fprintf(f, "frame structure, %zu events; geometry buffers captured: %zu (%u via Map/Unmap, %u via UpdateSubresource)\n", g_frame_ev.size(), g_vbufs.size(), g_vb_maps, g_vb_updates);
    for (const FrameEv& e : g_frame_ev) fprintf(f, "  %s\n", e.text);
    fprintf(f, "\n");
    fclose(f);
    g_vb_capture = false; g_vbufs.clear(); g_vb_in_flight.clear(); g_vb_maps = 0; g_vb_updates = 0;   // (the caller holds g_mutex)
}


/// If the override is on and this write carries a camera, returns a patched
/// private copy of the bytes to upload instead; otherwise returns src.
// ---------------------------------------------------------------- buffer layout dump
// One-shot diagnostic: the next upload of exactly `g_dump_cb_size` bytes is
// written to ffxvr_diag.log block by block (16 floats each), each block
// annotated with what it looks like: the view at the detected base, the
// projection variants, products of those with other affine blocks in the
// same buffer, the inverse view, or just an affine matrix with its
// translation. For layouts we have not met (hut walls: 1456 B).
static void dump_cb_layout(const uint8_t* p, UINT size, UINT base, const char* via) {
    diag("cb layout dump: %u bytes via %s, view base %u (0 = no camera found)", size, via, base);
    const UINT nb = size / 64;
    const float* V = base ? reinterpret_cast<const float*>(p + base) : nullptr;
    const float* Pj = base ? reinterpret_cast<const float*>(p + base + 64) : nullptr;      // near variant
    const float* Pm = (base && size >= base + 192 + 64) ? reinterpret_cast<const float*>(p + base + 192) : nullptr;
    float PV[16] = {}, PmV[16] = {};
    if (V && Pj) mul4(Pj, V, PV);
    if (V && Pm) mul4(Pm, V, PmV);
    for (UINT b = 0; b < nb; ++b) {
        const UINT off = b * 64;
        const float* M = reinterpret_cast<const float*>(p + off);
        char what[200] = "";
        auto add = [&](const char* t) { if (what[0]) strncat_s(what, ", ", _TRUNCATE); strncat_s(what, t, _TRUNCATE); };
        const bool affine = (M[12] == 0.f && M[13] == 0.f && M[14] == 0.f && fabsf(M[15] - 1.f) < 1e-4f);
        const bool persp = (fabsf(M[14] + 1.f) < 0.01f && fabsf(M[15]) < 1e-4f);
        if (V && off == base) add("VIEW");
        if (Pj && off == base + 64) add("PROJ(near variant)");
        if (Pm && off == base + 192) add("PROJ(main)");
        if (V && same4(M, V) && off != base) add("= view (copy)");
        if (V && same4(M, PV)) add("= projn*view");
        if (V && Pm && same4(M, PmV)) add("= projm*view");
        if (base && size >= base + 256 + 64 && off == base + 256) add("INV VIEW slot");
        if (V) {
            float pos[3]; if (looks_like_view(M, pos) && off != base) { char t[64]; snprintf(t, sizeof(t), "looks like a view, pos (%.1f %.1f %.1f)", pos[0], pos[1], pos[2]); add(t); }
            // products with other affine blocks X in the buffer
            for (UINT c = 0; c < nb; ++c) {
                if (c == b) continue;
                const float* X = reinterpret_cast<const float*>(p + c * 64);
                if (!(X[12] == 0.f && X[13] == 0.f && X[14] == 0.f && fabsf(X[15] - 1.f) < 1e-4f)) continue;
                float t1[16], t2[16], t3[16];
                mul4(V, X, t1); if (same4(M, t1)) { char t[48]; snprintf(t, sizeof(t), "= view*block%u", c); add(t); }
                mul4(PV, X, t2); if (same4(M, t2)) { char t[48]; snprintf(t, sizeof(t), "= projn*view*block%u", c); add(t); }
                if (Pm) { mul4(PmV, X, t3); if (same4(M, t3)) { char t[48]; snprintf(t, sizeof(t), "= projm*view*block%u", c); add(t); } }
            }
        }
        if (!what[0]) {
            if (persp) add("perspective-shaped");
            else if (affine) { char t[80]; snprintf(t, sizeof(t), "affine, t=(%.1f %.1f %.1f)", M[3], M[7], M[11]); add(t); }
            else {
                bool zero = true; for (int i = 0; i < 16; ++i) if (M[i] != 0.f) { zero = false; break; }
                add(zero ? "zeros" : "data");
            }
        }
        diag("  +%4u block %2u: %-40s  [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]",
             off, b, what, M[0], M[1], M[2], M[3], M[4], M[5], M[6], M[7], M[8], M[9], M[10], M[11], M[12], M[13], M[14], M[15]);
    }
    if (size % 64) diag("  +%4u: %u trailing bytes", nb * 64, size % 64);
}

/// The 2D layer system's dummy camera, given the head's rotation only (the
/// painting hangs at its authored direction, world-fixed, no parallax) and
/// the eye's field of view. Everything derived from the view in the buffer
/// (its inverse, proj*view, proj*view*X for the layer transforms X) is
/// recomputed; nothing else is touched. `orig` is the game's upload, `p`
/// the copy being rewritten; the eye is picked by g_eye_shift like write_camera.
static void write_backdrop_camera(const uint8_t* orig, uint8_t* p, UINT size, UINT base) {
    const float* V = reinterpret_cast<const float*>(orig + base);
    float Mq[9], MqT[9]; quat_to_mat(g_xr_q, Mq);
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) MqT[r*3+c] = Mq[c*3+r];
    const float R[9] = { V[0], V[1], V[2], V[4], V[5], V[6], V[8], V[9], V[10] };
    float Rn[9]; mul3(MqT, R, Rn);
    const float t[3] = { V[3], V[7], V[11] };
    float tn[3]; for (int i = 0; i < 3; ++i) tn[i] = MqT[i*3+0]*t[0] + MqT[i*3+1]*t[1] + MqT[i*3+2]*t[2];   // rotate about the camera position
    float Vn[16] = { Rn[0], Rn[1], Rn[2], tn[0],  Rn[3], Rn[4], Rn[5], tn[1],  Rn[6], Rn[7], Rn[8], tn[2],  0, 0, 0, 1 };
    // inverse of a rigid transform: R^T, -R^T t
    float In[16] = { Rn[0], Rn[3], Rn[6], 0,  Rn[1], Rn[4], Rn[7], 0,  Rn[2], Rn[5], Rn[8], 0,  0, 0, 0, 1 };
    In[3]  = -(Rn[0]*tn[0] + Rn[3]*tn[1] + Rn[6]*tn[2]);
    In[7]  = -(Rn[1]*tn[0] + Rn[4]*tn[1] + Rn[7]*tn[2]);
    In[11] = -(Rn[2]*tn[0] + Rn[5]*tn[1] + Rn[8]*tn[2]);
    memcpy(p + base, Vn, 64);
    if (size >= base + 256 + 64) memcpy(p + base + 256, In, 64);
    // projections: both variants get the eye's frustum
    float Pjn[16]; memcpy(Pjn, orig + base + 64, 64);
    float Pmn[16]; const bool have_pm = size >= base + 192 + 64; if (have_pm) memcpy(Pmn, orig + base + 192, 64);
    const bool fov = g_xr_active && g_ovr_enabled && g_xr_use_fov;
    const XrFovTan& f = g_xr_fov_used[(g_eye_shift != 0.f) ? 1 : 0];
    const bool fov_ok = std::isfinite(f.l) && std::isfinite(f.r) && std::isfinite(f.u) && std::isfinite(f.d) && f.r - f.l > 0.1f && f.u - f.d > 0.1f && f.r - f.l < 20.f && f.u - f.d < 20.f;
    auto apply = [&](float* P) {
        const float sx = (P[0] < 0.f) ? -1.f : 1.f, sy = (P[5] < 0.f) ? -1.f : 1.f;
        P[0] = sx * 2.f / (f.r - f.l);  P[2] = sx * (f.r + f.l) / (f.r - f.l);
        P[5] = sy * 2.f / (f.u - f.d);  P[6] = sy * (f.u + f.d) / (f.u - f.d);
    };
    if (fov && fov_ok) { apply(Pjn); memcpy(p + base + 64, Pjn, 64); if (have_pm) { apply(Pmn); memcpy(p + base + 192, Pmn, 64); } g_xr_fov_applied = true; }
    // derived blocks: anything equal to P*V or P*V*X (X an affine block of this buffer) for either projection
    const float* Pj = reinterpret_cast<const float*>(orig + base + 64);
    const float* Pm = have_pm ? reinterpret_cast<const float*>(orig + base + 192) : nullptr;
    float PV[16], PVn[16], PmV[16] = {}, PmVn[16] = {};
    mul4(Pj, V, PV); mul4(Pjn, Vn, PVn);
    if (Pm) { mul4(Pm, V, PmV); mul4(Pmn, Vn, PmVn); }
    const UINT nb = size / 64;
    for (UINT b = 0; b < nb; ++b) {
        const UINT off = b * 64;
        if (off == base || off == base + 64 || off == base + 192 || off == base + 256) continue;
        const float* M = reinterpret_cast<const float*>(orig + off);
        if (same4(M, PV)) { memcpy(p + off, PVn, 64); continue; }
        if (Pm && same4(M, PmV)) { memcpy(p + off, PmVn, 64); continue; }
        bool done = false;
        for (UINT c = 0; c < nb && !done; ++c) {
            if (c == b) continue;
            const float* X = reinterpret_cast<const float*>(orig + c * 64);
            if (!(X[12] == 0.f && X[13] == 0.f && X[14] == 0.f && fabsf(X[15] - 1.f) < 1e-4f)) continue;
            float t1[16], t2[16];
            mul4(PV, X, t1);
            if (same4(M, t1)) { mul4(PVn, X, t2); memcpy(p + off, t2, 64); done = true; break; }
            if (Pm) { mul4(PmV, X, t1); if (same4(M, t1)) { mul4(PmVn, X, t2); memcpy(p + off, t2, 64); done = true; break; } }
        }
    }
}

/// Phase 3 diagnostic: the first frame after arming, every skinned buffer whose
/// world translation sits on the player is appended raw to ffxvr_bones.bin
/// (record: "BONE", u32 size, u32 frame, u32 view base, f32 world[3], bytes).
static void bones_capture(const uint8_t* p, UINT size, UINT base, const float* world) {
    const float dx = world[0] - g_fp_pos[0], dy = world[1] - g_fp_pos[1], dz = world[2] - g_fp_pos[2];
    const float r = g_fp_hide_radius + 1.5f * g_fp_step;
    if (dx*dx + dy*dy + dz*dz >= r * r) return;
    if (g_bones_frame == 0) g_bones_frame = g_frames;
    if (g_frames != g_bones_frame) { g_bones_left = 0; return; }
    wchar_t path[MAX_PATH]; _snwprintf_s(path, _TRUNCATE, L"%s\\ffxvr_bones_%d.bin", g_dir, g_bones_take);
    FILE* f = nullptr; if (_wfopen_s(&f, path, L"ab") != 0 || f == nullptr) return;
    const uint32_t hdr[3] = { size, g_frames, base };
    fwrite("BONE", 1, 4, f); fwrite(hdr, 4, 3, f); fwrite(world, 4, 3, f); fwrite(p, 1, size, f); fclose(f);
    g_bones_left--;
    diag("bones: take %d captured %u B (view base %u, world %.1f %.1f %.1f), %d left", g_bones_take, size, base, world[0], world[1], world[2], g_bones_left);
}
static bool affine_at(const uint8_t* p, UINT off) {
    const float* f = reinterpret_cast<const float*>(p + off);
    bool ok = f[12] == 0.f && f[13] == 0.f && f[14] == 0.f && f[15] == 1.f;
    for (int i = 0; ok && i < 12; ++i) ok = std::isfinite(f[i]);
    for (int i : { 0, 1, 2, 4, 5, 6, 8, 9, 10 }) if (ok && fabsf(f[i]) > 3.f) ok = false;
    return ok;
}
static UINT palette_start(const uint8_t* p, UINT size) {
    for (UINT off : { 1344u, 1360u, 1456u, 1280u, 1392u, 1408u }) {
        if (off + 192 > size) continue;
        if (affine_at(p, off) && affine_at(p, off + 64) && affine_at(p, off + 128)) return off;
    }
    return 1344;
}
static int palette_slots(const uint8_t* p, UINT size, UINT start) {
    int n = 0;
    for (UINT off = start; off + 64 <= size && n < k_pal_max_slots; off += 64, ++n) {
        const float* f = reinterpret_cast<const float*>(p + off);
        bool ok = f[12] == 0.f && f[13] == 0.f && f[14] == 0.f && f[15] == 1.f;
        for (int i = 0; ok && i < 12; ++i) ok = std::isfinite(f[i]);
        for (int i : { 0, 1, 2, 4, 5, 6, 8, 9, 10 }) if (ok && fabsf(f[i]) > 3.f) ok = false;
        if (!ok) break;
    }
    return n;
}
// ---- VR hands ----
static void aff_point(const float* M, const float* b, float* o) { for (int i = 0; i < 3; ++i) o[i] = M[i*4+0]*b[0] + M[i*4+1]*b[1] + M[i*4+2]*b[2] + M[i*4+3]; }
static void aff_vec(const float* M, const float* b, float* o)   { for (int i = 0; i < 3; ++i) o[i] = M[i*4+0]*b[0] + M[i*4+1]*b[1] + M[i*4+2]*b[2]; }
static float v3len(const float* v) { return sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]); }
static void v3norm(float* v) { const float l = v3len(v); if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; } }
static void v3cross(const float* a, const float* b, float* o) { o[0] = a[1]*b[2] - a[2]*b[1]; o[1] = a[2]*b[0] - a[0]*b[2]; o[2] = a[0]*b[1] - a[1]*b[0]; }
static float v3dot(const float* a, const float* b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
/// Rotation (3x3 row-major) taking unit vector a to unit vector b, minimal angle.
static void rot_between(const float* a, const float* b, float* R) {
    float v[3]; v3cross(a, b, v); const float c = v3dot(a, b), s2 = v3dot(v, v);
    if (s2 < 1e-10f) {
        if (c > 0.f) { for (int i = 0; i < 9; ++i) R[i] = (i % 4 == 0) ? 1.f : 0.f; return; }
        float ax[3] = { 1.f, 0.f, 0.f }; if (fabsf(a[0]) > 0.9f) { ax[0] = 0.f; ax[1] = 1.f; }   // 180 degrees about any perpendicular
        float p[3]; v3cross(a, ax, p); v3norm(p);
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) R[i*3+j] = 2.f * p[i] * p[j] - (i == j ? 1.f : 0.f);
        return;
    }
    const float k = (1.f - c) / s2;
    R[0] = 1.f + k*(-v[2]*v[2] - v[1]*v[1]); R[1] = -v[2] + k*v[0]*v[1];            R[2] = v[1] + k*v[0]*v[2];
    R[3] = v[2] + k*v[0]*v[1];              R[4] = 1.f + k*(-v[2]*v[2] - v[0]*v[0]); R[5] = -v[0] + k*v[1]*v[2];
    R[6] = -v[1] + k*v[0]*v[2];             R[7] = v[0] + k*v[1]*v[2];              R[8] = 1.f + k*(-v[1]*v[1] - v[0]*v[0]);
}
/// Affine 4x4 = T(to) * R * T(-from): rotate by R (3x3) about `from`, then move that point to `to`.
static void delta_about(const float* R, const float* from, const float* to, float* D) {
    for (int i = 0; i < 3; ++i) { D[i*4+0] = R[i*3+0]; D[i*4+1] = R[i*3+1]; D[i*4+2] = R[i*3+2]; D[i*4+3] = to[i] - (R[i*3+0]*from[0] + R[i*3+1]*from[1] + R[i*3+2]*from[2]); }
    D[12] = D[13] = D[14] = 0.f; D[15] = 1.f;
}
static void mat3_from_cols(const float* c0, const float* c1, const float* c2, float* R) { for (int i = 0; i < 3; ++i) { R[i*3+0] = c0[i]; R[i*3+1] = c1[i]; R[i*3+2] = c2[i]; } }
static void mat3_mul(const float* A, const float* B, float* O) { for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) O[i*3+j] = A[i*3+0]*B[0*3+j] + A[i*3+1]*B[1*3+j] + A[i*3+2]*B[2*3+j]; }
static void mat3_mulT(const float* A, const float* B, float* O) { for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) O[i*3+j] = A[i*3+0]*B[j*3+0] + A[i*3+1]*B[j*3+1] + A[i*3+2]*B[j*3+2]; }   // A * B^T
static void euler_mat(float yaw, float pitch, float roll, float* R) {   // Ry(yaw) * Rx(pitch) * Rz(roll)
    const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch), cr = cosf(roll), sr = sinf(roll);
    const float Ry[9] = { cy, 0, sy,  0, 1, 0,  -sy, 0, cy }, Rx[9] = { 1, 0, 0,  0, cp, -sp,  0, sp, cp }, Rz[9] = { cr, -sr, 0,  sr, cr, 0,  0, 0, 1 };
    float t[9]; mat3_mul(Rx, Rz, t); mat3_mul(Ry, t, R);
}
static void refg_build();
/// The deltas for this frame, from the controllers and last frame's reference
/// palette transported by this record's actor matrix W.
static void hands_compute(const float* W) {
    for (int g = 0; g < G_COUNT; ++g) g_delta_on[g] = false;
    g_hand_live[0] = g_hand_live[1] = false;
    if (!g_hands_on) { strcpy_s(g_hands_why, "off"); return; }
    if (!g_hb_valid || !g_xr_active || g_ovr_suspend) { strcpy_s(g_hands_why, "needs first person in the headset"); return; }
    if (!g_ref_valid) { strcpy_s(g_hands_why, "no 53-slot reference part seen yet"); return; }
    if (!g_vrpad.ready) { strcpy_s(g_hands_why, "no controllers"); return; }
    // transport: X = W * Wref^-1 moves last frame's joints to where the actor is now (Wref is rigid: inverse by transpose)
    float Wi[16] = {}; { const float* r = g_ref_W; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) Wi[i*4+j] = r[j*4+i]; for (int i = 0; i < 3; ++i) Wi[i*4+3] = -(Wi[i*4+0]*r[3] + Wi[i*4+1]*r[7] + Wi[i*4+2]*r[11]); Wi[15] = 1.f; }
    float X[16]; mul4(W, Wi, X);
    refg_build(); for (int k = 0; k < k_ref_slots; ++k) if (g_refg[k] >= 0) mul4(X, g_ref_pal[k], g_ref_T[k]); g_ref_T_valid = true;
    const float up[3] = { 0.f, 1.f, 0.f };
    float side_model[3] = { 1.f, 0.f, 0.f }, side_w[3]; aff_vec(W, side_model, side_w); v3norm(side_w);   // the actor's left, in the world
    if (g_hands_collapse_head || g_hands_show > 0) {
        float Mh[16]; mul4(X, g_ref_pal[k_sl_head[0]], Mh); float J[3]; aff_point(Mh, k_bN_hd, J);
        float Z[9] = { 1e-5f, 0, 0,  0, 1e-5f, 0,  0, 0, 1e-5f }; const float zero[3] = { 0.f, 0.f, 0.f }; delta_about(Z, zero, J, g_delta[G_HEAD]); g_delta_on[G_HEAD] = true;
    }
    if (!g_side_known && g_fp_move_dir_valid && g_fp_step > 0.f) {   // measured once; "reset calibration" measures again
        const float* f = g_fp_move_dir; const float left_w[3] = { f[2], 0.f, -f[0] };
        float Mp[16], Mm[16]; mul4(X, g_ref_pal[k_slL_up[0]], Mp); mul4(X, g_ref_pal[k_slR_up[0]], Mm);
        const float bm[3] = { -k_bL_sh[0], k_bL_sh[1], k_bL_sh[2] }; float Sp[3], Sm[3]; aff_point(Mp, k_bL_sh, Sp); aff_point(Mm, bm, Sm);
        const float d[3] = { Sp[0]-Sm[0], Sp[1]-Sm[1], Sp[2]-Sm[2] };
        g_side_acc = fmaxf(-50.f, fminf(50.f, g_side_acc + v3dot(d, left_w)));
        if (fabsf(g_side_acc) > 20.f) { const bool was = g_px_is_left; g_px_is_left = g_side_acc > 0.f; if (!g_side_known || was != g_px_is_left) diag("hands: +x side of the bind space is Tidus's %s (measured from the run)", g_px_is_left ? "LEFT" : "RIGHT"); g_side_known = true; }
        const float zm[3] = { 0.f, 0.f, 1.f }; float zw[3]; aff_vec(W, zm, zw); g_fwd_sign = v3dot(zw, f) > 0.f ? 1.f : -1.f;   // model +z faces the travel direction?
    }
    for (int h = 0; h < 2; ++h) {
        const XrHand& hd = g_vrpad.hand[h]; if (!hd.grip_valid) continue;
        if (g_hands_only == 1 && h != 0) continue; if (g_hands_only == 2 && h != 1) continue;
        const bool px = ((h == 0) == g_px_is_left) != g_hands_swap;  // hand 0 (left controller) drives the arm on Tidus's left
        const float sgn = px ? 1.f : -1.f;
        const float b_sh[3] = { sgn*k_bL_sh[0], k_bL_sh[1], k_bL_sh[2] }, b_el[3] = { sgn*k_bL_el[0], k_bL_el[1], k_bL_el[2] }, b_wr[3] = { sgn*k_bL_wr[0], k_bL_wr[1], k_bL_wr[2] };
        const float b_kn[3] = { sgn*k_bL_kn[0], k_bL_kn[1], k_bL_kn[2] }, b_th[3] = { sgn*k_bL_th[0], k_bL_th[1], k_bL_th[2] };
        const int s_up = px ? k_slL_up[0] : k_slR_up[0], s_fo = px ? k_slL_fo[0] : k_slR_fo[0], s_hd = px ? 49 : 33;
        // current joints (world, this frame's actor pose, last frame's arm pose)
        float Mu[16], Mf[16], Mh[16]; mul4(X, g_ref_pal[s_up], Mu); mul4(X, g_ref_pal[s_fo], Mf); mul4(X, g_ref_pal[s_hd], Mh);
        float S0[3], E0[3], W0[3]; aff_point(Mu, b_sh, S0); aff_point(Mf, b_el, E0); aff_point(Mh, b_wr, W0);   // the bones as animated
        // The shoulder the arm hangs from is the player's, not Tidus's: beside and below the eyes, turned with the head. Tidus's
        // body can face anywhere (he turns to run), so an arm rooted at his animated shoulder crosses the chest when he turns.
        const float sw = (h == 0 ? -1.f : 1.f) * g_hands_sh_w * g_hb_sc, sd = g_hands_sh_down * g_hb_sc, sb = g_hands_sh_back * g_hb_sc;
        float S[3]; for (int i = 0; i < 3; ++i) S[i] = g_hb_eye[i] + g_hb_right[i] * sw - g_hb_fwd[i] * sb; S[1] -= sd;
        const float l1 = 2.71f, l2 = 2.48f;
        // target: the grip in the world. Reference space (metres; x right, y up, z back) -> world via the heading frame rows.
        float hr[3]; hand_to_ref(hd.grip_p, hr); if (g_hands_mirror) hr[0] = -hr[0];
        const float ref_inv[4] = { -g_xr_ref_q[0], -g_xr_ref_q[1], -g_xr_ref_q[2], g_xr_ref_q[3] }; float qr[4]; quat_mul(ref_inv, hd.grip_q, qr);
        float Rg[9]; quat_to_mat(qr, Rg);                                       // grip axes (columns) in reference space
        float off[3] = { (h == 0 ? -1.f : 1.f) * g_hands_off[0], g_hands_off[1], g_hands_off[2] }; float offr[3]; for (int i = 0; i < 3; ++i) offr[i] = Rg[i*3+0]*off[0] + Rg[i*3+1]*off[1] + Rg[i*3+2]*off[2];
        for (int i = 0; i < 3; ++i) hr[i] += offr[i];
        float P[3]; for (int i = 0; i < 3; ++i) P[i] = g_hb_P[i] + (g_hb_Rb[0*3+i]*hr[0] + g_hb_Rb[1*3+i]*hr[1] + g_hb_Rb[2*3+i]*hr[2]) * g_hb_sc;
        // target hand frame: fingers along the grip's -Z, thumb side along its +Y (mirrored roll for the right hand), then the calibration
        float RbT[9]; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) RbT[i*3+j] = g_hb_Rb[j*3+i];
        float Rgw[9]; mat3_mul(RbT, Rg, Rgw);                                    // grip axes in the world
        if (g_hands_cal_pending) {
            // the pose: fingers along shoulder -> hand, thumb toward the other hand (palms down)
            float f[3] = { P[0]-S[0], P[1]-S[1], P[2]-S[2] }; v3norm(f);
            float t[3];
            const XrHand& ho = g_vrpad.hand[1 - h];
            if (ho.grip_valid) { float hro[3]; hand_to_ref(ho.grip_p, hro); if (g_hands_mirror) hro[0] = -hro[0]; float Po[3]; for (int i = 0; i < 3; ++i) Po[i] = g_hb_P[i] + (g_hb_Rb[0*3+i]*hro[0] + g_hb_Rb[1*3+i]*hro[1] + g_hb_Rb[2*3+i]*hro[2]) * g_hb_sc; for (int i = 0; i < 3; ++i) t[i] = Po[i] - P[i]; }
            else { const float dir = (h == 0) ? 1.f : -1.f; for (int i = 0; i < 3; ++i) t[i] = dir * g_hb_Rb[0*3+i]; }   // one controller: toward the heading's right / left
            { const float k = v3dot(t, f); for (int i = 0; i < 3; ++i) t[i] -= k * f[i]; } v3norm(t);
            float x[3]; v3cross(f, t, x);                                             // target Rgc columns: X = f x t, Y = t, Z = -f
            float nz[3] = { -f[0], -f[1], -f[2] }; float Rt[9]; mat3_from_cols(x, t, nz, Rt);
            float RgwT[9]; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) RgwT[i*3+j] = Rgw[j*3+i];
            mat3_mul(RgwT, Rt, g_hands_Rc[h]);                                       // Rc = Rgw^T * Rt
            xr_haptic(h, 0.8f, 0.15f); g_hands_cal_count++; g_side_known = true;   // the side is settled with the calibration
        }
        float Rc0[9]; euler_mat(g_hands_cal[0] * sgn, g_hands_cal[1], g_hands_cal[2] * sgn, Rc0);
        float Rc[9]; mat3_mul(g_hands_Rc[h], Rc0, Rc); float Rgc[9]; mat3_mul(Rgw, Rc, Rgc);
        float fw[3] = { -Rgc[2], -Rgc[5], -Rgc[8] }, tw[3] = { Rgc[1], Rgc[4], Rgc[7] };
        float nw[3]; v3cross(fw, tw, nw); v3norm(nw); float tw2[3]; v3cross(nw, fw, tw2);
        float Hn[9]; mat3_from_cols(fw, tw2, nw, Hn);
        // current hand frame: the same two directions through the hand bone's rotation
        float fm[3] = { b_kn[0]-b_wr[0], b_kn[1]-b_wr[1], b_kn[2]-b_wr[2] }, tm[3] = { b_th[0]-b_wr[0], b_th[1]-b_wr[1], b_th[2]-b_wr[2] };
        v3norm(fm); { const float d = v3dot(tm, fm); for (int i = 0; i < 3; ++i) tm[i] -= d * fm[i]; } v3norm(tm);
        float fc[3], tc[3]; aff_vec(Mh, fm, fc); aff_vec(Mh, tm, tc); v3norm(fc); { const float d = v3dot(tc, fc); for (int i = 0; i < 3; ++i) tc[i] -= d * fc[i]; } v3norm(tc);
        float nc[3]; v3cross(fc, tc, nc); float Hc[9]; mat3_from_cols(fc, tc, nc, Hc);
        float Dh[9]; mat3_mulT(Hn, Hc, Dh);                                      // Hn * Hc^T
        // IK: the elbow from the shoulder; beyond Tidus's reach both bones stretch (up to g_hands_stretch), then the target is clamped
        float d[3] = { P[0]-S[0], P[1]-S[1], P[2]-S[2] }; float dl = v3len(d);
        const float reach0 = (l1 + l2) * 0.995f;
        const float k = (dl > reach0) ? fminf(dl / reach0, fmaxf(1.f, g_hands_stretch)) : 1.f;
        const float l1k = l1 * k, l2k = l2 * k, reach = (l1k + l2k) * 0.995f;
        if (dl > reach) { for (int i = 0; i < 3; ++i) { d[i] *= reach / dl; P[i] = S[i] + d[i]; } dl = reach; }
        if (dl < 0.5f) continue;
        float dn[3] = { d[0]/dl, d[1]/dl, d[2]/dl };
        const float a = (l1k*l1k - l2k*l2k + dl*dl) / (2.f * dl); const float hh = sqrtf(fmaxf(0.f, l1k*l1k - a*a));
        float pole[3] = { -up[0] + 0.5f*sgn*side_w[0], -up[1] + 0.5f*sgn*side_w[1], -up[2] + 0.5f*sgn*side_w[2] };   // elbows down and out
        { const float k = v3dot(pole, dn); for (int i = 0; i < 3; ++i) pole[i] -= k * dn[i]; } v3norm(pole);
        float E[3]; for (int i = 0; i < 3; ++i) E[i] = S[i] + dn[i]*a + pole[i]*hh;
        // deltas: upper arm about the shoulder, forearm elbow -> new elbow, hand wrist -> grip
        float u0[3] = { E0[0]-S0[0], E0[1]-S0[1], E0[2]-S0[2] }, u1[3] = { E[0]-S[0], E[1]-S[1], E[2]-S[2] }; v3norm(u0); v3norm(u1);
        float f0[3] = { W0[0]-E0[0], W0[1]-E0[1], W0[2]-E0[2] }, f1[3] = { P[0]-E[0], P[1]-E[1], P[2]-E[2] }; v3norm(f0); v3norm(f1);
        float Ru[9], Rf[9]; rot_between(u0, u1, Ru); rot_between(f0, f1, Rf);
        if (k > 1.001f) {   // stretch: scale along the new bone axis after the rotation, S' = I + (k-1) a a^T
            float Su[9], Sf[9], t[9];
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { Su[i*3+j] = (i == j ? 1.f : 0.f) + (k - 1.f) * u1[i] * u1[j]; Sf[i*3+j] = (i == j ? 1.f : 0.f) + (k - 1.f) * f1[i] * f1[j]; }
            mat3_mul(Su, Ru, t); memcpy(Ru, t, sizeof(t)); mat3_mul(Sf, Rf, t); memcpy(Rf, t, sizeof(t));
        }
        const int gu = px ? G_L_UP : G_R_UP, gf = px ? G_L_FO : G_R_FO, gh = px ? G_L_HD : G_R_HD;
        delta_about(Ru, S0, S, g_delta[gu]); delta_about(Rf, E0, E, g_delta[gf]); delta_about(Dh, W0, P, g_delta[gh]);
        { const int side = px ? 0 : 1; memcpy(g_hand_W0[side], W0, 12); memcpy(g_hand_P[side], P, 12); memcpy(g_hand_E[side], E, 12); memcpy(g_hand_Hn[side], Hn, 36); g_hand_live[side] = true; }
        g_delta_on[gu] = g_delta_on[gf] = g_delta_on[gh] = true;
    }
    g_hands_cal_pending = false;
    strcpy_s(g_hands_why, "on"); g_hands_frames++;
}
static void refg_build() {
    if (g_refg_built) return; g_refg_built = true; memset(g_refg, -1, sizeof(g_refg));
    for (int sl : k_slL_up) g_refg[sl] = G_L_UP; for (int sl : k_slL_fo) g_refg[sl] = G_L_FO; for (int sl : k_slL_hd) g_refg[sl] = G_L_HD;
    for (int sl : k_slR_up) g_refg[sl] = G_R_UP; for (int sl : k_slR_fo) g_refg[sl] = G_R_FO; for (int sl : k_slR_hd) g_refg[sl] = G_R_HD;
    for (int sl : k_sl_head) g_refg[sl] = G_HEAD;
}
/// The group of a bone matrix from a non-reference record: the nearest transported reference bone, if near enough.
static int group_of(const float* M, int* ref_slot = nullptr) {
    if (ref_slot) *ref_slot = -1;
    if (!g_ref_T_valid) return -1;
    int best = -1; float best_s = 1e9f;
    for (int k = 0; k < k_ref_slots; ++k) {
        if (g_refg[k] < 0) continue;
        const float* T = g_ref_T[k];
        const float dt = fabsf(M[3]-T[3]) + fabsf(M[7]-T[7]) + fabsf(M[11]-T[11]); if (dt > 1.5f) continue;
        float dr = 0.f; for (int i : { 0, 1, 2, 4, 5, 6, 8, 9, 10 }) dr += fabsf(M[i] - T[i]);
        const float sc = dt + dr; if (dr < 1.0f && sc < best_s) { best_s = sc; best = k; }
    }
    if (ref_slot) *ref_slot = best;
    return best >= 0 ? g_refg[best] : -1;
}
/// Applies this frame's deltas to one record's palette copies.
static int hands_apply(int rec, int n, const uint8_t* orig, uint8_t* eye1, uint8_t* eye2, int variant) {
    if (rec >= k_pal_max_recs) return 0;
    if (!g_hands_on) return 0;   // the NaN cut of the adjoining segments ran with the hands off: Tidus lost forearm pieces in every view
    refg_build(); int hand_slots = 0;
    const UINT po = g_pal_off_cur;
    // Pass 1: groups per slot, and the hand delta of THIS record from its own wrist-pivot bone (slots 45/46/49
    // left, 33/35 right in the reference layout), so the pivot is this frame's, not last frame's transported one.
    int8_t grp[k_pal_max_slots]; int refs[k_pal_max_slots];
    float Dhand[2][16]; bool have_hand[2] = { false, false }; int hand_pri[2] = { 0, 0 };   // 2 = the hand bone itself (49 / 33), 1 = a wrist-twist bone
    for (int sl = 0; sl < n; ++sl) {
        int rs = -1;
        if (rec == g_ref_claim && n == k_ref_slots) { grp[sl] = g_refg[sl]; rs = sl; }
        else grp[sl] = (int8_t)group_of(reinterpret_cast<const float*>(orig + po + sl * 64), &rs);
        refs[sl] = rs;
        const int side = (rs == 45 || rs == 46 || rs == 49) ? 0 : (rs == 33 || rs == 35) ? 1 : -1;
        const int pri = (rs == 49 || rs == 33) ? 2 : 1;
        if (side < 0 || hand_pri[side] >= pri || !g_hand_live[side] || !g_delta_on[side == 0 ? G_L_HD : G_R_HD]) continue;
        const float* M = reinterpret_cast<const float*>(orig + po + sl * 64);
        const float sgn = side == 0 ? 1.f : -1.f;
        const float b_wr[3] = { sgn*k_bL_wr[0], k_bL_wr[1], k_bL_wr[2] }, b_kn[3] = { sgn*k_bL_kn[0], k_bL_kn[1], k_bL_kn[2] }, b_th[3] = { sgn*k_bL_th[0], k_bL_th[1], k_bL_th[2] };
        float W0[3]; aff_point(M, b_wr, W0);
        float fm[3] = { b_kn[0]-b_wr[0], b_kn[1]-b_wr[1], b_kn[2]-b_wr[2] }, tm[3] = { b_th[0]-b_wr[0], b_th[1]-b_wr[1], b_th[2]-b_wr[2] };
        v3norm(fm); { const float d = v3dot(tm, fm); for (int i = 0; i < 3; ++i) tm[i] -= d * fm[i]; } v3norm(tm);
        float fc[3], tc[3]; aff_vec(M, fm, fc); aff_vec(M, tm, tc); v3norm(fc); { const float d = v3dot(tc, fc); for (int i = 0; i < 3; ++i) tc[i] -= d * fc[i]; } v3norm(tc);
        float nc[3]; v3cross(fc, tc, nc); float Hc[9]; mat3_from_cols(fc, tc, nc, Hc);
        float Dh[9]; mat3_mulT(g_hand_Hn[side], Hc, Dh);
        delta_about(Dh, W0, g_hand_P[side], Dhand[side]); have_hand[side] = true; hand_pri[side] = pri;
    }
    for (int sl = 0; sl < n; ++sl) {
        int g = grp[sl];
        if (g == G_L_HD || g == G_R_HD) hand_slots++;
        // The game blends bone matrices linearly, so a vertex on a seam between a drawn bone and a collapsed one
        // lands between the two: the only seam-free arrangement is ONE collapse point per draw, at the drawn
        // hand's own wrist. Hence the split: with both hands in a part, the part is drawn once per hand (variant
        // 0 / 1), each time with that hand's bones driven and every other bone - the other hand included -
        // collapsed onto that hand's wrist target. Seam vertices stay inside the hand; the rest is a dot there.
        bool drawn; const float* Dp = nullptr; float D[16]; bool nan_it = false;
        if (variant >= 0) {
            const int gh = variant == 0 ? G_L_HD : G_R_HD;
            drawn = (g == gh);
            if (drawn) { if (g_delta_on[g]) Dp = g_delta[g]; }
            else if (g_hand_live[variant]) { const float Z[9] = { 1e-5f, 0, 0,  0, 1e-5f, 0,  0, 0, 1e-5f }; const float zero[3] = { 0.f, 0.f, 0.f }; delta_about(Z, zero, g_hand_P[variant], D); Dp = D; }
        }
        else {
            drawn = (g == G_L_HD || g == G_R_HD) || (g_hands_show <= 2 && (g == G_L_FO || g == G_R_FO)) || (g_hands_show <= 1 && (g == G_L_UP || g == G_R_UP)) || (g_hands_show == 0 && g >= 0 && g != G_HEAD);
            if (drawn) {
                const int hs = (g == G_L_HD) ? 0 : (g == G_R_HD) ? 1 : -1;
                if (hs >= 0 && have_hand[hs]) Dp = Dhand[hs];           // this record's own pivot
                else if (g >= 0 && g_delta_on[g]) Dp = g_delta[g];
            }
            else if (g_hands_show > 0 || g == G_HEAD) {
                // The segment adjoining a drawn one is CUT: a NaN matrix. The shader sums weight * matrix over a
                // vertex's bones (Phyre's EvaluateSkinPosition, float3x4, w ignored - confirmed by the ray to the
                // origin with a zero matrix), so any vertex touching this bone becomes NaN and its triangles are
                // dropped: the seam vanishes instead of being dragged, and the drawn segment ends at its own
                // last hand-only ring. Bone slot 0 is never cut (a padding index with weight 0 would still NaN).
                const bool adjoining = ((g == G_L_FO || g == G_R_FO) && g_hands_show >= 3) || ((g == G_L_UP || g == G_R_UP) && g_hands_show == 2);
                if (adjoining && sl != 0 && g_hands_nan_cut) { nan_it = true; }
                else {
                    const float Z[9] = { 1e-5f, 0, 0,  0, 1e-5f, 0,  0, 0, 1e-5f }; const float zero[3] = { 0.f, 0.f, 0.f };
                    const int side = (g == G_L_FO || g == G_L_UP) ? 0 : (g == G_R_FO || g == G_R_UP) ? 1 : -1;
                    if (side >= 0 && g_hand_live[side]) { delta_about(Z, zero, (g == G_L_FO || g == G_R_FO) ? g_hand_P[side] : g_hand_E[side], D); Dp = D; }
                    else if (g_delta_on[G_HEAD]) Dp = g_delta[G_HEAD];
                }
            }
        }
        if (g_hands_dump && variant <= 0) {
            const float* M0 = reinterpret_cast<const float*>(orig + po + sl * 64);
            diag("hands dump: rec %2d slot %2d ref %2d group %2d %s  t=(%.1f %.1f %.1f)", rec, sl, refs[sl], g, drawn ? "DRAWN" : nan_it ? "CUT" : Dp ? "collapsed" : "untouched", M0[3], M0[7], M0[11]);
        }
        if (!Dp && !nan_it) { g_ap_none++; continue; }
        if (drawn) g_ap_arm++; else g_ap_col++;
        for (uint8_t* dst : { eye1, eye2 }) {
            if (!dst) continue;
            float* M = reinterpret_cast<float*>(dst + po + sl * 64);
            if (nan_it) { const float qnan = std::numeric_limits<float>::quiet_NaN(); for (int i = 0; i < 16; ++i) M[i] = qnan; }
            else { float o[16]; mul4(Dp, M, o); memcpy(M, o, 64); }
        }
    }
    return hand_slots;
}
/// One of the player's skinned uploads this frame: keeps its palette for the
/// map, and applies the wiggle to the copies that go to the GPU (both eyes).
static int bones_record(const uint8_t* p, UINT size, const float* world, uint8_t* eye1, EyeImage* e) {
    uint8_t* eye2 = e ? e->eye2.data() : nullptr;
    const float dx = world[0] - g_fp_pos[0], dy = world[1] - g_fp_pos[1], dz = world[2] - g_fp_pos[2];
    const float r = g_fp_hide_radius + 1.5f * g_fp_step;
    if (dx*dx + dy*dy + dz*dz >= r * r) return 0;
    const int rec = g_bone_rec++;
    if (rec >= k_pal_max_recs) return 0;
    const UINT po = palette_start(p, size); g_pal_off_cur = po;
    const int n = palette_slots(p, size, po);
    g_pal[rec].n = n; g_pal[rec].off = po; for (int sl = 0; sl < n; ++sl) memcpy(g_pal[rec].m[sl], p + po + sl * 64, 64);
    g_pal_n = rec + 1;
    const float* W = reinterpret_cast<const float*>(p + 640);
    if (g_delta_frame != g_frames) { g_delta_frame = g_frames; hands_compute(W); }
    const bool claim = (n == k_ref_slots && g_ref_claim < 0); if (claim) g_ref_claim = rec;
    int hand_slots;
    if (g_hands_split && g_hands_show == 3 && g_hands_on && e != nullptr && g_hand_live[0] && g_hand_live[1]) {
        // hands only, both controllers: split - the copies as they stand (camera written) become the right-hand variant
        e->eye1b.assign(eye1, eye1 + size); e->eye2b.assign(eye2, eye2 + size);
        hand_slots = hands_apply(rec, n, p, eye1, eye2, 0);
        hands_apply(rec, n, p, e->eye1b.data(), e->eye2b.data(), 1);
        e->split = hand_slots > 0;
    }
    else { hand_slots = hands_apply(rec, n, p, eye1, eye2, -1); if (e) e->split = false; }
    if (claim) { g_ref_rec = rec; memcpy(g_ref_W, W, 64); memcpy(g_ref_pal, p + po, sizeof(g_ref_pal)); g_ref_valid = true; }
    if (g_bseq_left > 0 && rec == g_bseq_rec && n > 0) {
        wchar_t path[MAX_PATH]; _snwprintf_s(path, _TRUNCATE, L"%s\\ffxvr_bones_seq.bin", g_dir);
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"ab") == 0 && f) { const uint32_t hdr[2] = { g_frames, (uint32_t)n }; fwrite("SEQ1", 1, 4, f); fwrite(hdr, 4, 2, f); fwrite(p + po, 64, n, f); fclose(f); }
        if (--g_bseq_left == 0) diag("bones: sequence capture done (record %d)", g_bseq_rec);
    }
    if (g_wig_amt == 0.f) return hand_slots;
    for (int sl = 0; sl < n; ++sl) {
        const bool hit = (rec == g_wig_rec && sl == g_wig_slot) || ((g_wig_map[rec][sl >> 3] >> (sl & 7)) & 1);
        if (!hit) continue;
        for (uint8_t* dst : { eye1, eye2 }) { if (!dst) continue; float* f = reinterpret_cast<float*>(dst + po + sl * 64); f[7] += g_wig_amt; }
    }
    return hand_slots;
}
/// Frame end: the wiggle map for next frame (every slot equal to the picked one), and the record count.
static void bones_frame_end() {
    g_pal_n_last = g_pal_n; for (int r = 0; r < g_pal_n; ++r) g_pal_slots_last[r] = g_pal[r].n;
    memset(g_wig_map, 0, sizeof(g_wig_map)); g_wig_matches = 0;
    if (!(g_ref_rec >= 0 && g_ref_rec < g_pal_n && g_pal[g_ref_rec].n == k_ref_slots)) g_ref_rec = -1;
    if (g_wig_rec >= 0 && g_wig_rec < g_pal_n && g_wig_slot >= 0 && g_wig_slot < g_pal[g_wig_rec].n) {
        const uint8_t* ref = g_pal[g_wig_rec].m[g_wig_slot];
        for (int r = 0; r < g_pal_n; ++r) for (int sl = 0; sl < g_pal[r].n; ++sl) {
            if ((r == g_wig_rec && sl == g_wig_slot) || memcmp(g_pal[r].m[sl], ref, 64) != 0) continue;
            g_wig_map[r][sl >> 3] |= (uint8_t)(1 << (sl & 7)); g_wig_matches++;
        }
    }
    g_pal_n = 0; g_bone_rec = 0;
    g_ref_claim = -1;
    g_ap_arm_l = g_ap_arm; g_ap_col_l = g_ap_col; g_ap_none_l = g_ap_none; g_ap_arm = g_ap_col = g_ap_none = 0;
    if (g_hands_dump) { diag("hands dump: end of frame, ref record %d, %d records", g_ref_rec, g_pal_n_last); g_hands_dump = false; }
}
extern "C" __declspec(dllexport) void ffxvr_bone_wiggle(int rec, int slot, float amount) { g_wig_rec = rec; g_wig_slot = slot; g_wig_amt = amount; }
extern "C" __declspec(dllexport) void ffxvr_bones_seq(int rec, int frames) {
    wchar_t path[MAX_PATH]; _snwprintf_s(path, _TRUNCATE, L"%s\\ffxvr_bones_seq.bin", g_dir);
    FILE* f = nullptr; if (_wfopen_s(&f, path, L"wb") == 0 && f) fclose(f);
    g_bseq_rec = rec; g_bseq_left = frames; diag("bones: sequence capture armed: record %d, %d frames", rec, frames);
}
extern "C" __declspec(dllexport) int ffxvr_bones_seq_left() { return g_bseq_left; }
extern "C" __declspec(dllexport) void ffxvr_hands_set(int on, int collapse_head, float cal_yaw, float cal_pitch, float cal_roll, float off_right, float off_up, float off_back, int swap, int mirror, int only) {
    g_hands_on = on != 0; g_hands_collapse_head = (collapse_head & 1) != 0; g_hands_show = (collapse_head >> 1) & 3; g_hands_swap = swap != 0; g_hands_mirror = mirror != 0; g_hands_only = only;
    g_hands_cal[0] = cal_yaw; g_hands_cal[1] = cal_pitch; g_hands_cal[2] = cal_roll; g_hands_off[0] = off_right; g_hands_off[1] = off_up; g_hands_off[2] = off_back;
}
extern "C" __declspec(dllexport) const char* ffxvr_hands_status(uint32_t* frames) {
    if (frames) *frames = g_hands_frames;
    static char buf[160];
    snprintf(buf, sizeof(buf), "%s | sides: %s (+x = Tidus's %s) | slots last frame: %d arm, %d collapsed, %d untouched, %d records", g_hands_why, g_side_known ? "locked" : "run a few steps to measure", g_px_is_left ? "left" : "right", g_ap_arm_l, g_ap_col_l, g_ap_none_l, g_pal_n_last);
    return buf;
}
/// Captures the grip -> hand rotation for both hands at the next frame (pose: arms straight out, palms down).
extern "C" __declspec(dllexport) void ffxvr_hands_calibrate() { g_hands_cal_pending = true; }
extern "C" __declspec(dllexport) void ffxvr_hands_dump() { g_hands_dump = true; }
extern "C" __declspec(dllexport) void ffxvr_hands_method(int nan_cut, int split) { g_hands_nan_cut = nan_cut != 0; g_hands_split = split != 0; }
/// The measured side (1 = +x is Tidus's left) and the stretch limit; set < 0 reads, otherwise writes the side (and marks it known).
extern "C" __declspec(dllexport) int ffxvr_hands_side(int set_px_left, float stretch) {
    if (stretch > 0.f) g_hands_stretch = stretch;
    if (set_px_left == -3) { g_px_is_left = !g_px_is_left; g_side_known = true; }   // manual flip
    if (set_px_left >= 0) { g_px_is_left = set_px_left != 0; g_side_known = true; }
    if (set_px_left == -2) { g_side_known = false; g_side_acc = 0.f; }   // measure again
    return (g_px_is_left ? 1 : 0) | (g_side_known ? 2 : 0);
}
/// Reads (set = 0) or writes (set = 1) the captured rotations: 18 floats, left then right, row-major 3x3.
extern "C" __declspec(dllexport) uint32_t ffxvr_hands_cal_data(float* rc18, int set) {
    if (rc18) { if (set) { memcpy(g_hands_Rc[0], rc18, 36); memcpy(g_hands_Rc[1], rc18 + 9, 36); } else { memcpy(rc18, g_hands_Rc[0], 36); memcpy(rc18 + 9, g_hands_Rc[1], 36); } }
    return g_hands_cal_count;
}
/// Last frame: the number of player records, each record's slot count (out32), and how many other slots the picked bone was found in.
extern "C" __declspec(dllexport) int ffxvr_bone_status(int* slots32, int* matches) {
    if (slots32) for (int r = 0; r < k_pal_max_recs; ++r) slots32[r] = r < g_pal_n_last ? g_pal_slots_last[r] : 0;
    if (matches) *matches = g_wig_matches;
    return g_pal_n_last;
}
/// Arms the capture above; optpos = the actor's 26 sub-positions (actor space, x y z each).
/// Each call is a new take: ffxvr_bones_1.bin, _2.bin, ... (the session's earlier takes stay).
extern "C" __declspec(dllexport) int ffxvr_bones_capture(const float* optpos78, int max_buffers) {
    g_bones_take++;
    wchar_t path[MAX_PATH]; _snwprintf_s(path, _TRUNCATE, L"%s\\ffxvr_bones_%d.bin", g_dir, g_bones_take);
    FILE* f = nullptr; if (_wfopen_s(&f, path, L"wb") == 0 && f) fclose(f);   // start fresh
    for (int k = 0; k < 26; ++k) for (int i = 0; i < 3; ++i) g_bones_optpos[k][i] = optpos78 ? optpos78[k*3+i] : 0.f;
    diag("bones: take %d armed (%d buffers max); player %.2f %.2f %.2f (render space); step %.2f", g_bones_take, max_buffers, g_fp_pos[0], g_fp_pos[1], g_fp_pos[2], g_fp_step);
    for (int k = 0; k < 26; ++k) diag("bones: optpos %2d  actor %.2f %.2f %.2f  render %.2f %.2f %.2f", k, g_bones_optpos[k][0], g_bones_optpos[k][1], g_bones_optpos[k][2], -g_bones_optpos[k][0], -g_bones_optpos[k][1], g_bones_optpos[k][2]);
    g_bones_left = max_buffers; g_bones_frame = 0;
    return g_bones_take;
}
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

    if (g_dump_cb_left > 0 && size == g_dump_cb_size) { g_dump_cb_left--; dump_cb_layout(p, size, base, "upload"); }

    // The 2D layer system's dummy camera: rows (-1 0 0)(0 1 0)(0 0 -1), position (0,0,-1). Not ours to move.
    if (base != 0) {
        const bool dummy = fabsf(v[0] + 1.f) < 1e-3f && fabsf(v[1]) < 1e-3f && fabsf(v[2]) < 1e-3f
                        && fabsf(v[4]) < 1e-3f && fabsf(v[5] - 1.f) < 1e-3f && fabsf(v[6]) < 1e-3f
                        && fabsf(v[8]) < 1e-3f && fabsf(v[9]) < 1e-3f && fabsf(v[10] + 1.f) < 1e-3f
                        && fabsf(v[3]) < 1e-3f && fabsf(v[7]) < 1e-3f && fabsf(v[11] + 1.f) < 1e-3f;
        if (dummy) {
            g_bg_uploads_frame++; g_bg_uploads_total++;
            if (g_backdrop_mode != 0 || !g_xr_active || !g_xr_rotate) return src;      // screen mode: the game's own frame
            // props-over-backdrop: the painting turns with the world, not the head
            if (g_scratch.size() < size) g_scratch.resize(size);
            memcpy(g_scratch.data(), p, size);
            write_backdrop_camera(p, g_scratch.data(), size, base);
            if (g_stereo && buf != nullptr) {
                EyeImage& e = ring_take(buf, size, 1);
                e.eye1.assign(g_scratch.data(), g_scratch.data() + size);
                e.eye2.assign(p, p + size);
                g_eye_shift = (g_ipd != 0.f) ? g_ipd : 1e-6f;
                write_backdrop_camera(p, e.eye2.data(), size, base);
                g_eye_shift = 0.f;
            }
            g_ovr_applied++;
            return g_scratch.data();
        }
        // Counted here, before anything can return early: the frame-type decision
        // (3D or screen) reads these, and a suspended override must still count.
        g_cam_uploads_frame++;
        const float* mp = reinterpret_cast<const float*>(p + base + 192);     // main projection: |P[0]| <= 0.2 is the intro's near-2D camera
        if (fabsf(mp[0]) > 0.2f && fabsf(mp[5]) > 0.2f) g_cam3d_uploads_frame++;
    }
    if (g_ovr_suspend) return src;              // the frame goes to the virtual screen: the game's own camera
    if (base == 0 && !g_have_PV) return src;    // no camera, and no known product this frame to look for: nothing to do

    if (g_scratch.size() < size) g_scratch.resize(size);

    unsigned mask = 0;
    if (base != 0) {
        memcpy(g_scratch.data(), p, size);
        if (g_ovr_freeze) {
            if (!g_ovr_have_base) { memcpy(g_ovr_base, v, sizeof(v)); g_ovr_have_base = true; }
            memcpy(v, g_ovr_base, sizeof(v));
        }
        else {
            g_ovr_have_base = false;
        }
        mask = write_camera(p, g_scratch.data(), size, base, v);
        if (base == 48) mask |= 128;

        // The object's world translation, for hiding the player's own body in
        // first person and for the bone work: skinned buffers (>= 1872 B) carry it at +640.
        float wpos[3] = {}; bool has_world = false;
        if (size >= 1872 && base + 416 + 160 + 64 <= size) {
            const float* W = reinterpret_cast<const float*>(p + base + 416 + 160);
            wpos[0] = W[3]; wpos[1] = W[7]; wpos[2] = W[11]; has_world = true;
            if (g_bones_left > 0 && g_fp_have_pos) bones_capture(p, size, base, wpos);
        }
        if (!(g_stereo && buf != nullptr) && has_world && g_fp_have_pos) bones_record(p, size, wpos, g_scratch.data(), nullptr);   // flat screen: eye 1 only

        if (g_stereo && buf != nullptr) {               // the same buffer seen from the other eye
            EyeImage& e = ring_take(buf, size, 1);
            e.eye2.assign(p, p + size);
            e.has_world = has_world; memcpy(e.world_pos, wpos, sizeof(wpos));

            // write_camera also publishes the frame's known product for the
            // product-only buffers; that must stay eye 1's, so save and restore.
            float savePV[16], savePVn[16]; const bool saveHave = g_have_PV;
            memcpy(savePV, g_last_PV, sizeof(savePV)); memcpy(savePVn, g_last_PVn, sizeof(savePVn));
            g_eye_shift = (g_ipd != 0.f) ? g_ipd : 1e-6f;   // non-zero marks "eye 2"; with the headset the real offset is used instead
            write_camera(p, e.eye2.data(), size, base, v);
            g_eye_shift = 0.f;
            if (mask & 8) memcpy(g_last_PVn2, g_last_PVn, sizeof(g_last_PVn2));   // eye 2's proj*view
            memcpy(g_last_PV, savePV, sizeof(savePV)); memcpy(g_last_PVn, savePVn, sizeof(savePVn)); g_have_PV = saveHave;

            // R45/R46: the hands (and the bone wiggle) after both cameras are written, and eye 1's image taken after
            // them: the split variants then carry eye 2's camera, and restoring eye 1 keeps its hands. write_camera
            // touches only the camera and world blocks, the hands only the palette, so with the hands off (and no
            // wiggle) this order changes nothing.
            if (has_world && g_fp_have_pos) e.pal_hands = bones_record(p, size, wpos, g_scratch.data(), &e) > 0;
            e.eye1.assign(g_scratch.data(), g_scratch.data() + size);
        }
    }
    else {
        if (replace_known_products(p, g_scratch.data(), size, g_last_PVn) == 0) return src;   // nothing of ours in here (and nothing copied)
        mask = 256;
        if (g_stereo && buf != nullptr) {
            EyeImage& e = ring_take(buf, size, 2);
            e.eye1.assign(g_scratch.data(), g_scratch.data() + size);
            e.eye2.assign(p, p + size);
            replace_known_products(e.eye2.data(), e.eye2.data(), size, g_last_PVn2);   // in place
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

    if (size >= 1872 && size < 16384) {
        const float* W = reinterpret_cast<const float*>(static_cast<const uint8_t*>(src) + 640);
        const float t[3] = { W[3], W[7], W[11] };
        if (std::isfinite(t[0]) && std::isfinite(t[1]) && std::isfinite(t[2]) && W[15] == 1.f) {
            bool seen = false;
            for (int i = 0; i < g_world_live_n; ++i) {
                if (fabsf(g_world_live[i][0]-t[0]) < 0.5f && fabsf(g_world_live[i][1]-t[1]) < 0.5f && fabsf(g_world_live[i][2]-t[2]) < 0.5f) { seen = true; break; }
            }
            if (!seen && g_world_live_n < k_world_max) { memcpy(g_world_live[g_world_live_n++], t, sizeof(t)); }
        }
    }

    CbRecord& rec = g_buffers[buf];
    rec.size    = size;
    rec.via     = via;
    rec.updates += 1;
    rec.frame = g_frames;
    // rec.data is diagnostic-only (the F9 pick, the frame census; nothing reads past 64 floats), so only the first
    // 256 bytes are kept. camera_from_write and the world extraction above read `src` and must keep running on EVERY
    // write: never gate record_buffer as a whole.
    constexpr UINT kCbKeep = 256;
    const UINT keep = size < kCbKeep ? size : kCbKeep;
    if (rec.data.size() != keep) rec.data.resize(keep);
    memcpy(rec.data.data(), src, keep);
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
    const DWORD period = diag_armed() ? 1000 : 30000;   // every second while debugging, else every 30 s (still proves the hooks live)
    if (!force && (now - g_last_beat) < period) return;
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

    {   // copied under the lock, printed after it (file I/O must not hold up the render thread's writes)
        struct BufLine { const void* buf; UINT size; unsigned long long updates; const char* via; };
        constexpr size_t kMaxLines = 300;
        std::vector<BufLine> lines; size_t total = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            total = g_buffers.size();
            lines.reserve(total < kMaxLines ? total : kMaxLines);
            for (const auto& [buf, rec] : g_buffers) {
                if (lines.size() >= kMaxLines) break;
                lines.push_back({ static_cast<const void*>(buf), rec.size, static_cast<unsigned long long>(rec.updates), rec.via });   // via: a string literal
            }
        }
        fprintf(f, "\n  distinct constant buffers:  %zu\n", total);
        for (const BufLine& b : lines) fprintf(f, "      %p  %5u bytes  %8llu updates  via %s\n", b.buf, b.size, b.updates, b.via);
        if (total > lines.size()) fprintf(f, "      ... %zu more\n", total - lines.size());
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

    if (diag_armed()) fprintf(f, "\n  latched slot-0 buffer: %zu bytes, latched at draw #%d, reads %u\n", g_full.size(), g_draws_latched_at, g_cam_reads);
    else fprintf(f, "\n  latched slot-0 buffer: not latched (diagnostics disarmed), reads %u\n", g_cam_reads);
    fprintf(f, "  xinput: the game's XInputGetState calls %u (with a pad %u), hooked %d; XInputSetState calls %u (non-zero %u), hooked %d\n", g_xi_hook_calls_all, g_xi_hook_calls, g_xi_hooked ? 1 : 0, g_xis_calls, g_xis_nonzero, g_xis_hooked ? 1 : 0);
    fprintf(f, "  build 7 diagnostics: %s; main thread %lu (joins seen on other threads %u); loading stalls logged %u\n", g_diag7 ? "on" : "off", g_main_tid, g_main_other, g_stalls);
    fprintf(f, "  cursor stage C2: game source %s, skipping the game's cursors now %ld (frames skipped %u)\n", g_c2_on ? "on" : "off", (long)g_c2_skip, g_c2_skip_frames);
    fprintf(f, "  ui vertex streams: purged on address reuse %u, mapping unreadable %u, oldest mapping read %u frames\n", g_vb_purged, g_vb_unreadable, g_rect_age_max);
    { unsigned held = 0, waits = 0; xr_counters(&held, &waits); fprintf(f, "  xr: frames held without an eye image %u, swapchain wait timeouts %u\n", held, waits); }
    fprintf(f, "  view matrix changed in %u frames so far\n", g_cam_changes);
    if (!diag_armed()) fprintf(f, "  changed since previous frame: not measured (open the overlay's Debug header)");
    else if (g_diff_count < 0) fprintf(f, "  changed since previous frame: n/a (frames not consecutive)");
    else fprintf(f, "  changed since previous frame: %d floats", g_diff_count);
    if (diag_armed() && g_diff_count > 0) {
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
        std::vector<std::pair<UINT, OvrStat>> stats;
        { std::lock_guard<std::mutex> lock(g_mutex); stats.assign(g_ovr_stats.begin(), g_ovr_stats.end()); }   // printed outside the lock
        if (!stats.empty()) {
            fprintf(f, "\n  camera override, by buffer size (blocks patched in ALL writes / in ANY write):\n");
            static const char* const names[9] = { "view", "inv", "pos", "VP", "VW", "PVW", "IVW", "shifted", "product-only" };
            for (const auto& [size, st] : stats) {
                fprintf(f, "    %6u B  %8u writes   all:", size, st.count);
                for (int i = 0; i < 9; ++i) if (st.mask_all & (1u << i)) fprintf(f, " %s", names[i]);
                fprintf(f, "   any:");
                for (int i = 0; i < 9; ++i) if (st.mask_any & (1u << i)) fprintf(f, " %s", names[i]);
                fprintf(f, "\n");
            }
        }
    }


    fprintf(f, "\n  eye 2: stereo=%d twins=%u backbuffer=%p draws last frame=%u (same inputs as eye 1: %u, upper bound)\n", g_stereo ? 1 : 0,
            g_twin_count, static_cast<void*>(g_backbuffer), g_eye2_draws_report, g_e2_same_inputs_report);
    fprintf(f, "         skipped: no stereo %u, no image %u, cb mismatch %u, no depth target %u, small cb %u; ok %u; begin failed %u; no depth twin %u; twin srvs dropped %u\n",
            g_e2_no_stereo, g_e2_no_image, g_e2_cb_mismatch, g_e2_no_dsv, g_e2_small, g_e2_ok, g_e2_begin_fail, g_e2_no_depth_twin, g_twin_srvs_dropped);
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

/// Once per frame, from ffxvr_begin_frame (outside its locks). The capture-done dump runs at the end of
/// begin_frame with the normal capture end.
void tick() {
    // Ctrl+F9: the constant-buffer capture (a large dump; plain F9 is the UI pick in the managed side).
    const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0 && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

    if (down && !g_f9_was_down) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_arm_next = true;   // begin at the next frame boundary, not mid-overlay
    }
    g_f9_was_down = down;

    heartbeat(false);
}

// ---------------------------------------------------------------- hooks

void STDMETHODCALLTYPE hk_VSSetConstantBuffers(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b) {
    if (c != g_ctx) { g_orig_vssetcb(c, s, n, b); return; }   // another context (the OpenXR runtime's): not ours
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
// Never call CreateBuffer while holding g_mutex: this hook locks it.
HRESULT STDMETHODCALLTYPE hk_CreateBuffer(ID3D11Device* dev, const D3D11_BUFFER_DESC* desc,
                                          const D3D11_SUBRESOURCE_DATA* init, ID3D11Buffer** out) {
    if (dev != g_device) return g_orig_createbuffer(dev, desc, init, out);   // the vtable is shared by every device of the class: not ours
    const HRESULT hr = g_orig_createbuffer(dev, desc, init, out);
    if (FAILED(hr) || out == nullptr || *out == nullptr) return hr;
    const bool cb_record = desc != nullptr && init != nullptr && init->pSysMem != nullptr && (desc->BindFlags & D3D11_BIND_CONSTANT_BUFFER);
    const bool purge = g_ui_vbs_any.load(std::memory_order_relaxed);
    if (!cb_record && !purge) return hr;
    if (cb_record) bump(C_CREATEBUF);
    std::lock_guard<std::mutex> lock(g_mutex);   // one lock for both
    // A new buffer at the address of a freed UI stream must not inherit its mapping (R9). Only the pointer record
    // goes: the UI membership stays, so the stream's next Map is recorded at once (a stream the game re-creates at the
    // same address keeps working).
    if (purge && g_vb_last_map.erase(*out)) g_vb_purged++;
    if (cb_record) record_buffer(*out, desc->ByteWidth, init->pSysMem, "CreateBuffer");
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateTexture2D(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC* desc,
                                             const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
    if (dev != g_device) return g_orig_createtex2d(dev, desc, init, out);   // another device: no MSAA override, no log
    if (desc != nullptr && desc->SampleDesc.Count > 1 && desc->Width >= 1280 && desc->Height >= 720
        && (desc->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) != 0) {
        g_msaa_seen++;
        g_msaa_last_w = desc->Width; g_msaa_last_h = desc->Height; g_msaa_last_n = desc->SampleDesc.Count;
        const int want = g_msaa_override;
        if ((want == 2 || want == 4) && (UINT)want < desc->SampleDesc.Count) {
            UINT levels = 0;
            if (SUCCEEDED(dev->CheckMultisampleQualityLevels(desc->Format, (UINT)want, &levels)) && levels > 0) {
                D3D11_TEXTURE2D_DESC d = *desc; d.SampleDesc.Count = (UINT)want; d.SampleDesc.Quality = 0;
                const HRESULT hr = g_orig_createtex2d(dev, &d, init, out);
                if (SUCCEEDED(hr)) {
                    g_msaa_changed++; g_msaa_last_n = (UINT)want;
                    diag("msaa: %ux%u fmt %d bind 0x%X: %u samples -> %d (override)", desc->Width, desc->Height, (int)desc->Format, (unsigned)desc->BindFlags, (unsigned)desc->SampleDesc.Count, want);
                    return hr;
                }
                diag("msaa: override to %d failed (0x%08X) for %ux%u fmt %d; creating as the game asked", want, (unsigned)hr, desc->Width, desc->Height, (int)desc->Format);
            }
        }
        else diag("msaa: %ux%u fmt %d bind 0x%X created at %u samples (override %d)", desc->Width, desc->Height, (int)desc->Format, (unsigned)desc->BindFlags, (unsigned)desc->SampleDesc.Count, want);
    }
    return g_orig_createtex2d(dev, desc, init, out);
}

HRESULT STDMETHODCALLTYPE hk_Map(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP t, UINT fl, D3D11_MAPPED_SUBRESOURCE* m) {
    const HRESULT hr = g_orig_map(c, r, sub, t, fl, m);
    if (c != g_ctx) return hr;                                  // another context: not ours
    bump(C_MAP);
    if (FAILED(hr) || m == nullptr || m->pData == nullptr) return hr;

    ID3D11Buffer* buf = nullptr; UINT size = 0;
    if (as_constant_buffer(r, &buf, &size)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_in_flight[m->pData] = { buf, size };
    }
    else if (g_vb_capture || g_ui_vbs_any.load(std::memory_order_relaxed)) {   // not g_ui_vbs.empty(): the set can change on another thread
        UINT bind = 0;
        if (as_geometry_buffer(r, &buf, &size, &bind) && size <= 64u * 1024 * 1024) {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_vb_capture) g_vb_in_flight[m->pData] = { buf, size };
            if (g_ui_vbs.count(buf)) {
                if (g_vb_last_map.size() >= 512 && g_vb_last_map.find(buf) == g_vb_last_map.end()) {   // cap (never reached in practice): drop the least recently drawn
                    auto old = g_vb_last_map.begin();
                    for (auto it = g_vb_last_map.begin(); it != g_vb_last_map.end(); ++it) if (it->second.last_draw < old->second.last_draw) old = it;
                    g_vb_last_map.erase(old);
                }
                VbMap& vm = g_vb_last_map[buf];
                if (vm.p != m->pData || vm.size != size) vm.checked_frame = ~0u;   // a new pointer or size: re-check it (not on every NO_OVERWRITE map)
                vm.p = m->pData; vm.size = size; vm.frame = g_frames;
                if (vm.last_draw == 0) vm.last_draw = g_frames;
            }
        }
    }
    return hr;
}

void STDMETHODCALLTYPE hk_Unmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) {
    if (c != g_ctx) { g_orig_unmap(c, r, sub); return; }       // another context: not ours
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
    else if (g_vb_capture) {
        UINT bind = 0;
        if (as_geometry_buffer(r, &buf, &size, &bind)) {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (auto it = g_vb_in_flight.begin(); it != g_vb_in_flight.end(); ++it) {
                if (it->second.first != buf) continue;
                std::vector<uint8_t>& d = g_vbufs[buf]; d.resize(size); memcpy(d.data(), it->first, size);
                g_vb_maps++;
                g_vb_in_flight.erase(it);
                break;
            }
        }
    }

    g_orig_unmap(c, r, sub);   // tick() runs once per frame from ffxvr_begin_frame, not per Unmap
}

// ---------------------------------------------------------------- install

// Copy the latched slot-0 buffer somewhere the CPU can read, and diff it against the previous frame's copy
// (diagnostic only: the camera itself comes from the CPU-side writes). Uses the original Map/Unmap so our own hooks
// neither recurse nor record the staging buffers as game data. Only while diag_armed(); never blocks.
void read_camera() {
    if (!diag_armed()) {
        if (g_slot0) { g_slot0->Release(); g_slot0 = nullptr; }   // not latched while disarmed either (ffxvr_on_draw)
        return;
    }
    if (g_device == nullptr || g_ctx == nullptr || g_orig_map == nullptr || g_orig_unmap == nullptr) return;

    // Collect: pending copies oldest first. The GPU completes in order, so stop at the first still in flight.
    for (;;) {
        CamRb* e = nullptr;
        for (CamRb& c : g_cam_rb) if (c.pending && (e == nullptr || c.frame < e->frame)) e = &c;
        if (e == nullptr) break;
        D3D11_MAPPED_SUBRESOURCE ms = {};
        const HRESULT hr = g_orig_map(g_ctx, e->st, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) break;
        e->pending = false;
        if (FAILED(hr) || ms.pData == nullptr) { if (SUCCEEDED(hr)) g_orig_unmap(g_ctx, e->st, 0); continue; }
        const uint8_t* p = static_cast<const uint8_t*>(ms.pData);
        g_full_prev.swap(g_full);
        g_full.assign(p, p + e->size);
        g_orig_unmap(g_ctx, e->st, 0);
        // Diff against the previous copy: consecutive frames, same size only.
        g_diff_count = 0;
        if (e->frame != g_cam_last_collected + 1) g_diff_count = -1;
        else if (g_full_prev.size() == g_full.size()) {
            const float* fa = reinterpret_cast<const float*>(g_full.data());
            const float* fb = reinterpret_cast<const float*>(g_full_prev.data());
            const size_t n = g_full.size() / 4;
            for (size_t i = 0; i < n; ++i) {
                if (fa[i] != fb[i]) {
                    if (g_diff_count < 8) g_diff_offs[g_diff_count] = static_cast<uint32_t>(i * 4);
                    g_diff_count++;
                }
            }
        }
        g_cam_last_collected = e->frame;
    }

    // Issue this frame's copy into a free entry (none free: skip this frame).
    if (g_slot0 == nullptr) return;
    D3D11_BUFFER_DESC src = {};
    g_slot0->GetDesc(&src);
    if (src.ByteWidth < 48) return;
    CamRb* e = nullptr;
    for (CamRb& c : g_cam_rb) if (!c.pending) { e = &c; break; }
    if (e == nullptr) return;
    if (e->st == nullptr || e->size != src.ByteWidth) {
        if (e->st) { e->st->Release(); e->st = nullptr; }
        D3D11_BUFFER_DESC sd = {};
        sd.ByteWidth      = src.ByteWidth;
        sd.Usage          = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(g_device->CreateBuffer(&sd, nullptr, &e->st))) { e->st = nullptr; e->size = 0; return; }
        e->size = src.ByteWidth;
    }
    g_ctx->CopyResource(e->st, g_slot0);
    e->frame = g_frames; e->pending = true;
}

bool patch(void** vtable, int index, void* replacement, void** out_original, const char* name = "?") {
    if (vtable[index] == replacement) { diag("patch %s: already installed", name); return true; }   // never record ourselves as the original
    DWORD old = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old)) return false;
    *out_original = vtable[index];
    vtable[index] = replacement;
    VirtualProtect(&vtable[index], sizeof(void*), old, &old);

    if (g_patched_n < 12) g_patched[g_patched_n++] = { vtable, index, name, replacement, *out_original };
    return true;
}
/// Only for a failed init: restore what we changed, newest first, and only slots that still hold our pointer.
/// The g_orig_* pointers stay valid (they are the real functions), so a hook already running on another thread
/// still completes.
static void unpatch_all() {
    for (int i = g_patched_n - 1; i >= 0; --i) {
        Patched& pt = g_patched[i]; DWORD old = 0;
        if (VirtualProtect(&pt.vt[pt.index], sizeof(void*), PAGE_READWRITE, &old)) {
            const bool ours = pt.vt[pt.index] == pt.ours;
            if (ours) pt.vt[pt.index] = pt.original;
            VirtualProtect(&pt.vt[pt.index], sizeof(void*), old, &old);
            diag("init failed: %s %s", pt.name, ours ? "restored" : "not ours any more, left alone");
        }
    }
    g_patched_n = 0;
}

} // namespace

static void ui_census_geometry();   // census helper, defined below ffxvr_on_draw's neighbours
static void ui_pass_begin();        // the UI pass opener, defined with the clear hooks
static void measure_now();          // the Measure report, defined with ffxvr_measure (run deferred from begin_frame)
static void snap_release();         // frees the four scene-snapshot objects, defined above snapshot_scene
static void opaque_release(int slot);   // frees one preview copy (g_opaque), defined with opaque_copy


// ---------------------------------------------------------------------------

// The export set's version. ffxvr.dll requires every export it binds and this exact number (module.cs k_abi):
// bump it whenever an export is added, removed or changes its signature, in the same commit as the managed side.
constexpr int FFXVR_ABI = 7;   // 2: + ffxvr_diag_keepalive, ffxvr_eye2_same_inputs; 3: + ffxvr_view_smooth_set, ffxvr_lowres_status, - ffxvr_anchor_set; 4: + ffxvr_xr_debug; 5: + ffxvr_draw_skip_ranges (ffxvr_draw_skip kept as a shim); 6: + ffxvr_diag7_set, ffxvr_diag_line, ffxvr_main_note, ffxvr_motion_cols; 7 (branch cursors): + ffxvr_cursor_game, ffxvr_cursor_skip
extern "C" __declspec(dllexport) int ffxvr_abi() { return FFXVR_ABI; }

/// Build 7 diagnostics (log only): bit 0 on (ini diag_build7; called once at module init, before ffxvr_init, so the
/// XInputSetState hook and the stall watchdog follow it), bit 1 re-arms the per-battle rumble log (battle start).
extern "C" __declspec(dllexport) void ffxvr_diag7_set(int flags) {
    g_diag7 = (flags & 1) != 0;
    if (!g_diag7) InterlockedExchange(&g_stall_open, 0);   // switched off mid-stall: no stale "ended" line later (B7-7)
    if (flags & 2) g_xis_until = g_xis_w + 20;
    if (g_diag7 && g_wd_thread == nullptr) {
        g_wd_stop = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (g_wd_stop) g_wd_thread = CreateThread(nullptr, 64 * 1024, stall_watchdog, nullptr, 0, nullptr);
        diag("build 7 diagnostics on: stall watchdog %s", g_wd_thread ? "started" : "FAILED to start");
    }
}
/// Cursor stage C2 (render thread, every frame): the game source is live (cursor_source=game, the cursor hooks installed,
/// markers on). Only then may begin_frame decide to skip the game's cursors.
extern "C" __declspec(dllexport) void ffxvr_cursor_game(int on) { g_c2_on = on != 0; }
/// Cursor stage C2 (the game's main thread, from the cursor detours): 1 = skip the game's cursor draw this frame. One aligned
/// interlocked-published value, decided at the last frame boundary: never torn, at most one frame old.
extern "C" __declspec(dllexport) int ffxvr_cursor_skip() { return static_cast<int>(InterlockedCompareExchange(&g_c2_skip, 0, 0)); }
/// One line from the managed side into ffxvr_diag.log (render thread only; the caller throttles).
extern "C" __declspec(dllexport) void ffxvr_diag_line(const char* s) { if (s != nullptr) diag("%s", s); }
/// Called by managed's job-join hook (the game's main thread): the first caller is taken as the main thread, for the
/// stall watchdog. Returns 1 on that thread, 0 elsewhere (counted).
extern "C" __declspec(dllexport) int ffxvr_main_note() {
    const DWORD t = GetCurrentThreadId();
    if (g_main_tid == 0) { g_main_tid = t; return 1; }
    if (t == g_main_tid) return 1;
    g_main_other++; return 0;
}
/// Extra Motion-log columns for this frame (6.4): post-join snapshot drawn root xyz, Present-time drawn root xyz,
/// snapshot logic position xyz (render space), exec byte, frames since the snapshot, snapshot valid.
extern "C" __declspec(dllexport) void ffxvr_motion_cols(const float* v, int n) {
    if (v == nullptr || n <= 0) { g_motion_cols_fresh = false; return; }
    for (int i = 0; i < 12; ++i) g_motion_cols[i] = i < n ? v[i] : 0.f;
    g_motion_cols_fresh = true;
}

/// Keeps the costly diagnostics armed for `frames` more frames (see g_diag_until). Managed calls it with 2 every
/// frame the overlay's Debug header is expanded, so closing the header (or the window) disarms them.
extern "C" __declspec(dllexport) void ffxvr_diag_keepalive(int frames) {
    const uint32_t u = g_frames + (uint32_t)(frames < 1 ? 1 : frames);
    if (u > g_diag_until) g_diag_until = u;
}

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
    install_xinput_hook();
    install_xinput_set_hook();   // build 7 (log only; nothing unless ffxvr_diag7_set turned it on)

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
            // ID3D11Device vtable index 5: CreateTexture2D (MSAA override).
            patch(dvt, 5, reinterpret_cast<void*>(&hk_CreateTexture2D),
                  reinterpret_cast<void**>(&g_orig_createtex2d), "CreateTexture2D (device)");
        }
    }

    if (!ok) { unpatch_all(); g_ctx = nullptr; return -2; }   // no half-installed state: the hooks that went in come out

    g_installed       = true;
    g_patched_context = game_context;

    {   // start this run's trace fresh
        wchar_t tpath[MAX_PATH];
        _snwprintf_s(tpath, _TRUNCATE, L"%s\\ffxvr_trace.log", g_dir);
        FILE* t = nullptr;
        if (_wfopen_s(&t, tpath, L"w") == 0 && t != nullptr) {
            fprintf(t, "camera trace: one line per heartbeat (1 s while debugging, 30 s otherwise), from UpdateSubresource writes of per-object buffers\n");
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
// ---------------------------------------------------------------- housekeeping / perf
/// Every few hundred frames: drop twins of textures the game stopped
/// rendering into (area changes free and recreate them; the twin of a
/// freed texture would otherwise hold its VRAM for good) and constant-buffer
/// records that stopped being updated.
static void twin_release(Twin& t);
static void twin_srv_trim();   // R12, defined with twin_srv
static void sweep_caches() {
    if (g_frames % 300 != 0) return;
    for (auto it = g_twins.begin(); it != g_twins.end();) {
        // Scene-sized twins cost hundreds of MB and a long allocation; the
        // game re-uses its targets across battles, so they stay for 10 min.
        const bool big = it->second.w >= 1280;
        const uint32_t idle = big ? 36000u : 900u;
        if (it->first != g_backbuffer && g_frames - it->second.last_used > idle) { twin_release(it->second); it = g_twins.erase(it); g_twins_swept++; }
        else ++it;
    }
    if (!snap_wanted() && g_snap_tex && g_frames - g_snap_frame > 600) {   // R3: idle scene snapshots (2x scene-target VRAM) go after 10 s
        snap_release();
        opaque_release(1); opaque_release(2);   // and the preview's scene-size copies of them (requested together, so idle together)
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_buffers.begin(); it != g_buffers.end();) {
        if (g_frames - it->second.frame > 3000) { it = g_buffers.erase(it); g_buffers_pruned++; }
        else ++it;
    }
}

struct Perf { double frame_ms, xr_ms, frame_max_ms, wait_ms, end_ms, e2_ms, gpu_ms, gpu_max_ms; uint32_t twins, twin_srvs, buffers, in_flight, plain_srvs; uint64_t private_mb, vram_mb, vram_budget_mb; };
static Perf g_perf = {};
static LARGE_INTEGER g_qpf = {}, g_t_frame_prev = {}; static double g_acc_frame = 0, g_acc_xr = 0; static int g_acc_n = 0; static ULONGLONG g_perf_log_tick = 0;
double g_xr_ms_this_frame = 0; double g_e2_ms_this_frame = 0; static double g_acc_wait = 0, g_acc_end = 0, g_acc_e2 = 0, g_acc_max = 0; LARGE_INTEGER g_e2_t0 = {};
static IDXGIAdapter3* g_adapter3 = nullptr; static bool g_adapter3_tried = false;
// GPU time per frame: a ring of timestamp-disjoint queries spanning Present to
// Present, read back a few frames late so the readback never stalls.
constexpr int k_gpuq_ring = 6;
static ID3D11Query* g_gpuq_disjoint[k_gpuq_ring] = {}; static ID3D11Query* g_gpuq_t0[k_gpuq_ring] = {}; static ID3D11Query* g_gpuq_t1[k_gpuq_ring] = {};
static int g_gpuq_cur = -1; static uint32_t g_gpuq_frames = 0; static bool g_gpuq_failed = false;
static double g_acc_gpu = 0, g_acc_gpu_max = 0; static int g_acc_gpu_n = 0;
static bool g_gpuq_t0_pending = false;    // t0 is stamped at the game's first clear, so the idle before it is left out
static void gpu_timing_start_span() {
    if (!g_gpuq_t0_pending || g_gpuq_cur < 0 || !g_ctx) return;
    g_gpuq_t0_pending = false;
    g_ctx->End(g_gpuq_t0[g_gpuq_cur]);
}
// The span closes at Present, before xrWaitFrame: a timestamp issued after
// the wait would be reached late by an idle GPU and count the wait as GPU time.
static bool g_gpuq_span_closed = false;
static void gpu_timing_end_span() {
    if (g_gpuq_failed || !g_ctx || g_gpuq_cur < 0 || g_gpuq_span_closed) return;
    if (g_gpuq_t0_pending) gpu_timing_start_span();
    g_ctx->End(g_gpuq_t1[g_gpuq_cur]); g_ctx->End(g_gpuq_disjoint[g_gpuq_cur]);
    g_gpuq_span_closed = true;
}
static void gpu_timing_tick() {
    if (g_gpuq_failed || !g_device || !g_ctx) return;
    if (!g_gpuq_disjoint[0]) {
        D3D11_QUERY_DESC dj = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, ts = { D3D11_QUERY_TIMESTAMP, 0 };
        for (int i = 0; i < k_gpuq_ring; ++i)
            if (FAILED(g_device->CreateQuery(&dj, &g_gpuq_disjoint[i])) || FAILED(g_device->CreateQuery(&ts, &g_gpuq_t0[i])) || FAILED(g_device->CreateQuery(&ts, &g_gpuq_t1[i]))) { g_gpuq_failed = true; return; }
    }
    gpu_timing_end_span();
    g_gpuq_span_closed = false;
    g_gpuq_frames++;
    // Read the slot that is about to be reused (k_gpuq_ring - 1 frames old).
    const int next = (g_gpuq_cur + 1) % k_gpuq_ring;
    if (g_gpuq_frames > (uint32_t)k_gpuq_ring) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {}; UINT64 t0 = 0, t1 = 0;
        if (g_ctx->GetData(g_gpuq_disjoint[next], &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && !dj.Disjoint && dj.Frequency &&
            g_ctx->GetData(g_gpuq_t0[next], &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && g_ctx->GetData(g_gpuq_t1[next], &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && t1 >= t0) {
            const double ms = (double)(t1 - t0) * 1000.0 / (double)dj.Frequency;
            g_acc_gpu += ms; if (ms > g_acc_gpu_max) g_acc_gpu_max = ms; g_acc_gpu_n++;
        }
    }
    g_gpuq_cur = next;
    g_ctx->Begin(g_gpuq_disjoint[g_gpuq_cur]); g_gpuq_t0_pending = true;
}
static void perf_tick() {
    if (g_qpf.QuadPart == 0) QueryPerformanceFrequency(&g_qpf);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (g_t_frame_prev.QuadPart) {
        const double fm = (now.QuadPart - g_t_frame_prev.QuadPart) * 1000.0 / g_qpf.QuadPart;
        g_acc_frame += fm; if (fm > g_acc_max) g_acc_max = fm; g_acc_xr += g_xr_ms_this_frame; g_acc_e2 += g_e2_ms_this_frame; g_acc_n++;
        double w = 0, e = 0; xr_timing(&w, &e); g_acc_wait += w; g_acc_end += e;
    }
    g_t_frame_prev = now; g_xr_ms_this_frame = 0; g_e2_ms_this_frame = 0;
    gpu_timing_tick();
    if (g_acc_n < 60) return;
    g_perf.frame_ms = g_acc_frame / g_acc_n; g_perf.xr_ms = g_acc_xr / g_acc_n; g_perf.frame_max_ms = g_acc_max;
    g_perf.wait_ms = g_acc_wait / g_acc_n; g_perf.end_ms = g_acc_end / g_acc_n; g_perf.e2_ms = g_acc_e2 / g_acc_n;
    g_acc_frame = g_acc_xr = g_acc_wait = g_acc_end = g_acc_e2 = g_acc_max = 0; g_acc_n = 0;
    if (g_acc_gpu_n > 0) { g_perf.gpu_ms = g_acc_gpu / g_acc_gpu_n; g_perf.gpu_max_ms = g_acc_gpu_max; g_acc_gpu = g_acc_gpu_max = 0; g_acc_gpu_n = 0; }
    g_perf.twins = (uint32_t)g_twins.size(); g_perf.twin_srvs = 0; for (auto& kv : g_twins) g_perf.twin_srvs += (uint32_t)kv.second.srvs.size();
    { std::lock_guard<std::mutex> lock(g_mutex); g_perf.buffers = (uint32_t)g_buffers.size(); g_perf.in_flight = (uint32_t)g_in_flight.size(); }
    g_perf.plain_srvs = g_plain_srvs_count();
    PROCESS_MEMORY_COUNTERS_EX pm = {}; pm.cb = sizeof(pm);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof(pm))) g_perf.private_mb = pm.PrivateUsage >> 20;
    if (!g_adapter3_tried && g_device) {
        g_adapter3_tried = true;
        IDXGIDevice* dd = nullptr; IDXGIAdapter* ad = nullptr;
        if (SUCCEEDED(g_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)) && dd) {
            if (SUCCEEDED(dd->GetAdapter(&ad)) && ad) { ad->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&g_adapter3); ad->Release(); }
            dd->Release();
        }
    }
    if (g_adapter3) {
        DXGI_QUERY_VIDEO_MEMORY_INFO vi = {};
        if (SUCCEEDED(g_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vi))) { g_perf.vram_mb = vi.CurrentUsage >> 20; g_perf.vram_budget_mb = vi.Budget >> 20; }
    }
    const ULONGLONG t = GetTickCount64();
    const bool slow = g_perf.frame_ms > 40.0;
    if (t - g_perf_log_tick > (slow ? 5000ULL : 30000ULL)) {
        g_perf_log_tick = t;
        diag("perf: frame %.1f ms (max %.0f; gpu %.1f max %.0f; xr %.1f = wait %.1f + end %.1f; eye2 re-issue %.1f) | twins %u (srvs %u, swept %u) buffers %u (pruned %u) | private %llu MB, vram %llu / %llu MB | draws %u eye2 %u (same-input %u, upper bound) hidden %u hist %u | ui verts read %u, truncated %u, start!=0 %u (not read %u), base!=0 %u, skip overflow %u",
             g_perf.frame_ms, g_perf.frame_max_ms, g_perf.gpu_ms, g_perf.gpu_max_ms, g_perf.xr_ms, g_perf.wait_ms, g_perf.end_ms, g_perf.e2_ms, g_perf.twins, g_perf.twin_srvs, g_twins_swept, g_perf.buffers, g_buffers_pruned,
             (unsigned long long)g_perf.private_mb, (unsigned long long)g_perf.vram_mb, (unsigned long long)g_perf.vram_budget_mb, g_draw_in_frame_last, g_eye2_draws_report, g_e2_same_inputs_report, g_fp_hidden_draws, g_hist_draws,
             g_ui_verts_read, g_ui_trunc, g_ui_start_nonzero, g_ui_start_bail, g_ui_base_nonzero, g_skip_overflow);
        g_ui_verts_read = g_ui_trunc = g_ui_start_nonzero = g_ui_start_bail = g_ui_base_nonzero = g_skip_overflow = 0;   // R39/R48: since the last perf: line
    }
}

extern "C" __declspec(dllexport) void ffxvr_begin_frame() {
    g_render_tid = GetCurrentThreadId();
    diag7_frame();                  // build 7: Present time for the stall watchdog, its "ended" line, rumble changes
    {   // the frame time every filter uses (stick turn, follow pans, auto depth): measured here, whatever the overlay does
        static LARGE_INTEGER s_dt_prev = {};
        if (g_qpf.QuadPart == 0) QueryPerformanceFrequency(&g_qpf);
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        if (s_dt_prev.QuadPart && g_qpf.QuadPart) {
            const float dt = (float)((now.QuadPart - s_dt_prev.QuadPart) / (double)g_qpf.QuadPart);
            g_dt = dt < k_dt_min ? k_dt_min : dt > k_dt_max ? k_dt_max : dt; g_dt_measured = true;
        }
        s_dt_prev = now;
    }
    if (g_pick_armed) {
        g_pick_armed = false;
        if (g_pick_hits == 0) { snprintf(g_pick_summary, sizeof(g_pick_summary), "no UI draw under the point (%.0f,%.0f); UI draws recognised last frame: %u (stereo %d, headset %d); scene-pass draws using a 2048x4096 sheet (text drawn in the world?): %u; nearest: %s", g_pick_nx * 1920.f - 960.f, g_pick_ny * 1080.f - 540.f, g_ui_draws_report, g_stereo ? 1 : 0, g_xr_want ? 1 : 0, g_pick_scene_text, g_pick_best); diag("pick: %s", g_pick_summary); }
        g_pick_scene_text = 0;
    }
    if (g_slowmo_ms > 0) Sleep((DWORD)g_slowmo_ms);
    if (g_atlas_dump_armed && g_atlas_dumped_n > 0) g_atlas_dump_armed = false;   // one frame with UI draws is enough
    g_ui_flat_pass = false; g_ui_flat_resolved = false; g_flat_scene_tex = nullptr; g_flat_scene_draws = 0; g_flat_draws_per_target.clear();
    bool dump_now = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_frames++;

        if (g_capture_done) {                // record_buffer's safety valve ended the capture: dump it below, outside the lock
            g_capture_done = false;
            dump_now       = true;
        }
        else if (g_capturing) {
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
    tick();                         // Ctrl+F9 poll + heartbeat, once per frame (takes g_mutex itself)
    read_camera();
    sweep_caches();
    twin_srv_trim();                // R12: oversized twin SRV caches are trimmed here, where none of our views is mid-bind
    gpu_timing_end_span();          // GPU span of the frame just finished, closed before the runtime wait
    xr_tick();                      // submit the frame just finished, locate the head for the next one
    {   // cursor stage C2: skip the game's cursors only while the headset shows our markers (a 3D frame with the UI pass, markers in 3D)
        // C2-8: sticky on the UI pass. A 3D frame without the pass un-skips only after k_c2_nopass of them in a row, so the first 3D
        // frame after headset start or after a screen frame (no pass yet) is skipped too; if the pass never comes the cursors return.
        const bool base = g_c2_on && g_xr_want && xr_active() && g_markers_3d && g_ui_enabled && !g_ovr_suspend && !g_xr_screen_now && !g_screen3d_now;
        if (!base || g_ui_last_frame_had_pass) g_c2_nopass = 0; else if (g_c2_nopass < 1000) g_c2_nopass++;
        const bool skip = base && g_c2_nopass <= k_c2_nopass;
        InterlockedExchange(&g_c2_skip, skip ? 1 : 0); if (skip) g_c2_skip_frames++;
    }
    ptr_sprites_release();          // after xr_tick: its markers pass draws the pointer sprites captured this frame (R43)
    perf_tick();
    g_overlay_phase = true;
    g_first_draw_pending = true;
    g_backbuffer_seen_this_frame = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_frame_live)  { g_frame_live = false; frame_event("-- Present --"); frame_dump(); }
        if (g_frame_arm)   { g_frame_arm = false; g_frame_live = true; g_noimg_census_lines = 0; g_scene_census_lines = 0; frame_event("-- frame start (after Present) --"); }
        if (g_stacks_live) { g_stacks_live = false; census_dump(); }
        if (g_stacks_arm)  { g_stacks_arm = false; g_stacks_live = true; }
    }
    g_latched      = false;
    g_draw_in_frame_last = (uint32_t)g_draw_in_frame;
    g_draw_in_frame = 0;
    g_scene_draws_last = g_scene_draw_idx; g_scene_draw_idx = 0;
    g_targets_frame_n = 0; g_cleared_frame_n = 0;
    g_have_PV       = false;
    g_eye2_draws_report = g_eye2_draws; g_eye2_draws = 0;
    g_e2_same_inputs_report = g_e2_same_inputs; g_e2_same_inputs = 0;
    g_fp_parts_last = g_fp_part_idx; g_fp_part_idx = 0;
    bones_frame_end();
    {   // UI pass health: how many 3D frames had a captured UI pass, and the frame structure the detector saw
        static uint32_t resolves_acc = 0, clears_acc = 0, no_pass_acc = 0;
        if (g_xr_active && !g_ovr_suspend) {
            g_ui_frames_3d++; resolves_acc += g_ui_resolves_frame; clears_acc += g_ui_depth_clears_frame;
            if (g_ui_last_frame_had_pass) g_ui_frames_with_pass++; else no_pass_acc++;
        }
        g_ui_resolves_frame = 0; g_ui_depth_clears_frame = 0;
        static ULONGLONG next = 0; const ULONGLONG now = GetTickCount64();
        if (now >= next) {
            next = now + 10000;
            if (g_ui_frames_3d) diag("ui: 3D frames %u, with a captured UI pass %u (none: %u) | per frame avg: scene resolves %.1f, depth clears on the scene target %.1f | ui draws last %u | ui_enabled %d xr_want %d | imgui layer draws last %u (force %d, min %u)",
                g_ui_frames_3d, g_ui_frames_with_pass, no_pass_acc, resolves_acc / (float)g_ui_frames_3d, clears_acc / (float)g_ui_frames_3d, g_ui_draws_report, g_ui_enabled ? 1 : 0, g_xr_want ? 1 : 0, g_imgui_draws_last, g_imgui_force ? 1 : 0, g_imgui_min_draws);
            g_ui_frames_3d = g_ui_frames_with_pass = 0; resolves_acc = clears_acc = no_pass_acc = 0;
        }
    }
    {   // R4: how the once-per-frame view tick behaves (every 10 s while 3D frames run)
        static ULONGLONG next = 0; static uint32_t f0 = 0; const ULONGLONG now = GetTickCount64();
        if (now >= next) {
            next = now + 10000; const uint32_t nf = g_frames - f0; f0 = g_frames;
            if (g_view_ticks && (g_xr_want || diag_armed()))   // headset on, or debugging: not every 10 s on the flat desktop
                diag("view: frames %u, ticks %u, eye-1 camera uploads/frame %.1f, uploads with another camera %u, eye-1 uploads before the tick %u", nf, g_view_ticks, nf ? g_view_uploads / (float)nf : 0.f, g_view_othercam, g_view_early);
            g_view_ticks = g_view_uploads = g_view_othercam = g_view_early = 0;
        }
    }
    g_scene_tex = g_scene_live_tex; g_scene_draws = g_scene_live_draws; g_scene_live_tex = nullptr; g_scene_live_draws = 0; g_draws_per_target.clear();
    g_slot0_report = g_slot0_seen;   // snapshot before the reset, so the UI sees it
    g_slot0_seen   = 0;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_world_live_n > 0) {
            memcpy(g_world_report, g_world_live, sizeof(g_world_report));
            g_world_report_n = g_world_live_n; g_world_report_frame = g_frames;
        }
        else if (g_frames - g_world_report_frame > 2) g_world_report_n = 0;   // nothing skinned for a few frames (an empty shot): the list is stale (R40)
        g_world_live_n = 0;
    }

    if (dump_now) dump_capture();
    // A deferred Measure (R3): once the frame just finished took a snapshot, or after 5 frames whatever happened.
    if (g_measure_pending && (g_snap_frame + 1 == g_frames || g_frames >= g_measure_deadline)) { g_measure_pending = false; measure_now(); }
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

// Census, second line per 2D UI draw: the draw call, shaders, blend state, topology and the first
// vertices (x y z w of each, as the vertex buffer holds them; for the UI these are screen positions).
static void ui_census_geometry() {
    if (!g_ctx) return;
    char line[400]; size_t l = 0;
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr; g_ctx->VSGetShader(&vs, nullptr, nullptr); g_ctx->PSGetShader(&ps, nullptr, nullptr);
    ID3D11BlendState* bs = nullptr; FLOAT bf[4]; UINT bm = 0; g_ctx->OMGetBlendState(&bs, bf, &bm);
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED; g_ctx->IAGetPrimitiveTopology(&topo);
    ID3D11Buffer* vb = nullptr; UINT stride = 0, voff = 0; g_ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &voff);
    ID3D11Buffer* ib = nullptr; DXGI_FORMAT ifmt = DXGI_FORMAT_UNKNOWN; UINT ioff = 0; g_ctx->IAGetIndexBuffer(&ib, &ifmt, &ioff);
    { D3D11_BUFFER_DESC vbd = {}; if (vb) vb->GetDesc(&vbd);
      l += snprintf(line + l, sizeof(line) - l, "         %s n=%u start=%u base=%d topo=%d vs=%p ps=%p blend=%p stride=%u vb=%p/%uB", g_draw_kind == 1 ? "DrawIndexed" : "Draw", g_draw_n, g_draw_start, g_draw_base, (int)topo, (void*)vs, (void*)ps, (void*)bs, stride, (void*)vb, (unsigned)vbd.ByteWidth); }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto vit = vb ? g_vbufs.find(vb) : g_vbufs.end();
        if (vit == g_vbufs.end() || stride < 8) l += snprintf(line + l, sizeof(line) - l, " verts: (not captured)");
        else {
            const std::vector<uint8_t>& vd = vit->second;
            auto iit = (g_draw_kind == 1 && ib) ? g_vbufs.find(ib) : g_vbufs.end();
            // textured sprites: the first six vertices (one quad); textureless draws: up to 24 distinct
            // vertices, continued on extra lines - the cursor is textureless and may be many triangles
            bool notex = texture_of(g_cur_ps_srv[0]) == nullptr;
            if (!notex) { D3D11_TEXTURE2D_DESC td = {}; texture_of(g_cur_ps_srv[0])->GetDesc(&td); if (td.Width == 1024 && td.Height == 1024) notex = true; }   // the cursor atlas: full vertex list too
            const unsigned nv = g_draw_n < (notex ? 72u : 6u) ? g_draw_n : (notex ? 72u : 6u);
            unsigned printed = 0; float lastv[4] = { 1e30f, 1e30f, 1e30f, 1e30f };
            l += snprintf(line + l, sizeof(line) - l, " verts:");
            for (unsigned k = 0; k < nv; ++k) {
                unsigned vi;
                if (g_draw_kind == 1) {
                    if (iit == g_vbufs.end()) { l += snprintf(line + l, sizeof(line) - l, " (indices not captured)"); break; }
                    const std::vector<uint8_t>& id = iit->second; const unsigned isz = ifmt == DXGI_FORMAT_R16_UINT ? 2 : 4;
                    const size_t ip = ioff + (size_t)(g_draw_start + k) * isz; if (ip + isz > id.size()) break;
                    vi = isz == 2 ? *reinterpret_cast<const uint16_t*>(id.data() + ip) : *reinterpret_cast<const uint32_t*>(id.data() + ip);
                    vi += (unsigned)g_draw_base;
                }
                else vi = g_draw_start + k;
                const unsigned nf = stride / 4 < 4 ? stride / 4 : 4;
                const size_t vp = voff + (size_t)vi * stride; if (vp + nf * 4 > vd.size()) break;
                const float* f = reinterpret_cast<const float*>(vd.data() + vp);
                if (notex) {   // skip repeats of the previous vertex (shared quad corners)
                    bool same = true; for (unsigned j = 0; j < nf; ++j) if (f[j] != lastv[j]) same = false;
                    if (same) continue; for (unsigned j = 0; j < nf; ++j) lastv[j] = f[j];
                    if (++printed > 24) { l += snprintf(line + l, sizeof(line) - l, " ..."); break; }
                }
                if (l > sizeof(line) - 60) {   // continue on a new line
                    FrameEv c; strncpy_s(c.text, line, _TRUNCATE); if (g_frame_ev.size() < 2000) g_frame_ev.push_back(c);
                    l = (size_t)snprintf(line, sizeof(line), "            ...");
                }
                l += snprintf(line + l, sizeof(line) - l, " [");
                for (unsigned j = 0; j < nf && l < sizeof(line) - 16; ++j) l += snprintf(line + l, sizeof(line) - l, "%s%.4g", j ? " " : "", f[j]);
                if (l < sizeof(line) - 4) l += snprintf(line + l, sizeof(line) - l, "]");
            }
        }
        FrameEv e; strncpy_s(e.text, line, _TRUNCATE); if (g_frame_ev.size() < 2000) g_frame_ev.push_back(e);
        // the other vertex streams (UVs, colours): first and third vertex of each, so a sprite's atlas rect shows
        l = (size_t)snprintf(line, sizeof(line), "            streams:");
        for (UINT slot = 1; slot <= 2; ++slot) {
            ID3D11Buffer* sb = nullptr; UINT sstride = 0, soff = 0; g_ctx->IAGetVertexBuffers(slot, 1, &sb, &sstride, &soff);
            if (!sb) continue;
            auto sit = g_vbufs.find(sb);
            l += snprintf(line + l, sizeof(line) - l, " v%u(stride %u)", slot, sstride);
            if (sit == g_vbufs.end() || sstride < 4) l += snprintf(line + l, sizeof(line) - l, " ?");
            else {
                const std::vector<uint8_t>& sd = sit->second; const unsigned nf = sstride / 4 < 4 ? sstride / 4 : 4;
                auto iit = (g_draw_kind == 1 && ib) ? g_vbufs.find(ib) : g_vbufs.end();
                for (unsigned k = 0; k < 3 && k < g_draw_n; k += 2) {
                    unsigned vi = g_draw_start + k;
                    if (g_draw_kind == 1) {
                        if (iit == g_vbufs.end()) break;
                        const std::vector<uint8_t>& id = iit->second; const unsigned isz = ifmt == DXGI_FORMAT_R16_UINT ? 2 : 4;
                        const size_t ip = ioff + (size_t)(g_draw_start + k) * isz; if (ip + isz > id.size()) break;
                        vi = (isz == 2 ? *reinterpret_cast<const uint16_t*>(id.data() + ip) : *reinterpret_cast<const uint32_t*>(id.data() + ip)) + (unsigned)g_draw_base;
                    }
                    const size_t vp = soff + (size_t)vi * sstride; if (vp + nf * 4 > sd.size() || l > sizeof(line) - 80) break;
                    const float* f = reinterpret_cast<const float*>(sd.data() + vp); const uint32_t* u = reinterpret_cast<const uint32_t*>(f);
                    l += snprintf(line + l, sizeof(line) - l, " [");
                    for (unsigned j = 0; j < nf; ++j) {
                        if (std::isfinite(f[j]) && fabsf(f[j]) < 1e6f && fabsf(f[j]) > 1e-6f) l += snprintf(line + l, sizeof(line) - l, "%s%.4g", j ? " " : "", f[j]);
                        else l += snprintf(line + l, sizeof(line) - l, "%s%08X", j ? " " : "", u[j]);   // not a float: a packed colour or similar
                    }
                    l += snprintf(line + l, sizeof(line) - l, "]");
                }
            }
            sb->Release();
        }
        if (l > 21) { FrameEv e2; strncpy_s(e2.text, line, _TRUNCATE); if (g_frame_ev.size() < 2000) g_frame_ev.push_back(e2); }
    }
    if (vs) vs->Release(); if (ps) ps->Release(); if (bs) bs->Release(); if (vb) vb->Release(); if (ib) ib->Release();
}

extern "C" __declspec(dllexport) int ffxvr_on_draw(int which) {
    if (g_in_eye2) return 0;
    { const DWORD tid = GetCurrentThreadId(); if (g_render_tid != 0 && tid != g_render_tid) { g_other_thread_calls++; g_other_thread_last = tid; } }
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
    const bool fp_hiding = g_fp_mode == 1 && g_fp_hide_body;
    if ((fp_hiding || g_fp_solo >= 0) && g_fp_have_pos && g_xr_want && !g_ovr_suspend && !g_screen3d_now && !g_backdrop_room && g_eye2_cur != nullptr && g_eye2_cur->has_world) {
        const float dx = g_eye2_cur->world_pos[0] - g_fp_pos[0], dy = g_eye2_cur->world_pos[1] - g_fp_pos[1], dz = g_eye2_cur->world_pos[2] - g_fp_pos[2];
        const float r = g_fp_hide_radius + 1.5f * g_fp_step;
        if (dx*dx + dy*dy + dz*dz < r * r) {   // one of the player's own parts
            const bool scene = g_cur_rtv_n > 0 && g_scene_tex != nullptr && texture_of(g_cur_rtv[0]) == g_scene_tex;
            const int bm = (g_hands_on && g_fp_body_mode == 0) ? 1 : g_fp_body_mode;   // VR hands: the body is shown, head parts hidden
            if (fp_hiding && bm == 0 && g_fp_solo < 0) { g_fp_hidden_draws++; return -1; }       // the whole body, in every pass
            if (g_hands_on && g_hands_show >= 2 && fp_hiding && g_fp_solo < 0 && !g_eye2_cur->pal_hands) { g_fp_hidden_draws++; return -1; }   // floating hands: parts without hand bones are not drawn at all
            if (scene) {
                const uint32_t idx = g_fp_part_idx++;
                if (g_fp_solo >= 0) { if ((int)idx != g_fp_solo) { g_fp_hidden_draws++; return -1; } }
                else if (fp_hiding && bm == 1 && idx < 32 && ((g_fp_head_mask >> idx) & 1u)) { g_fp_hidden_draws++; return -1; }   // a head part
            }
        }
    }
    if (g_blitz_overlay && g_ui_state == UI_RESOLVED && g_xr_want && g_ui_enabled && !g_ovr_suspend && !g_screen3d_now && g_eye2_cur == nullptr
        && g_cur_rtv_n > 0 && g_ctx && texture_of(g_cur_rtv[0]) == g_scene_live_tex) {
        ID3D11BlendState* bs = nullptr; float bf[4]; UINT bm = 0; g_ctx->OMGetBlendState(&bs, bf, &bm);
        bool blend = false; if (bs) { D3D11_BLEND_DESC bd; bs->GetDesc(&bd); blend = bd.RenderTarget[0].BlendEnable != 0; bs->Release(); }
        ID3D11DepthStencilState* ds = nullptr; UINT sref = 0; g_ctx->OMGetDepthStencilState(&ds, &sref);
        bool zwrite = false; if (ds) { D3D11_DEPTH_STENCIL_DESC dd; ds->GetDesc(&dd); zwrite = dd.DepthEnable && dd.DepthWriteMask; ds->Release(); }
        if (blend && !zwrite) {
            // Resuming after the pause is NOT early any more: the resolve that follows the real UI pass must
            // end it (UI_DONE), or the frame is never treated as a UI frame (first test: UI in one eye only).
            if (g_ui_early_keep) { g_ui_early_keep = false; g_ui_state = UI_PASS; }
            else { ui_pass_begin(); if (g_ui_state == UI_PASS) g_ui_early = true; }
        }
    }
    g_ui_draw_now = (g_ui_state == UI_PASS && g_cur_rtv_n > 0 && texture_of(g_cur_rtv[0]) == g_scene_live_tex);
    if (g_ui_draw_now && g_ui_early) g_ui_early_draws++;
    if (!g_stereo && g_cur_rtv_n > 0 && g_cur_dsv != nullptr) {
        ID3D11Texture2D* gt = texture_of(g_cur_rtv[0]);
        if (gt) { const uint32_t c = ++g_flat_draws_per_target[gt]; if (c > g_flat_scene_draws) { g_flat_scene_draws = c; g_flat_scene_tex = gt; } }
    }
    g_ui_flat_draw_now = (!g_ui_draw_now && g_ui_flat_pass && !g_xr_want && g_cur_rtv_n > 0 && texture_of(g_cur_rtv[0]) == (g_stereo ? g_scene_live_tex : g_flat_scene_tex));
    if (g_ui_draw_now || g_ui_flat_draw_now) {
        // Experiment: skip UI draws whose slot-0 texture has a given size (the battle cursor sprite is
        // suspected to come from the 1024x1024 atlas; see design-notes "cursor census").
        if (g_pick_armed) ui_pick_check(g_ui_draws_frame);
        ui_hover_collect();
        if (g_atlas_dump_armed) {
            ID3D11Texture2D* at = texture_of(g_cur_ps_srv[0]);
            if (at) {
                D3D11_TEXTURE2D_DESC ad = {}; at->GetDesc(&ad);
                bool seen = false; for (int i = 0; i < g_atlas_dumped_n; ++i) if (g_atlas_dumped[i] == at) seen = true;
                if (!seen && g_atlas_dumped_n < 16 && (ad.Width == 1024 || ad.Width == 1632)) {
                    g_atlas_dumped[g_atlas_dumped_n++] = at;
                    wchar_t name[64]; _snwprintf_s(name, _TRUNCATE, L"ui_atlas_%ux%u_%02d.dds", ad.Width, ad.Height, g_atlas_dumped_n);
                    dump_dds(at, name);
                }
            }
        }
        if (g_ui_hide_w != 0 || g_ui_hide_notex || g_ui_hide_cursor || g_hide_pointers) {
            ID3D11Texture2D* t = texture_of(g_cur_ps_srv[0]);
            bool hide = false;
            if (t) { D3D11_TEXTURE2D_DESC d = {}; if (g_ui_hide_w != 0) { t->GetDesc(&d); if (d.Width == g_ui_hide_w && d.Height == g_ui_hide_h) hide = true; } }
            // R39: one shared read of the batch (up to k_ui_max_q quads); a quad goes into qmask when it is on the hide list or the
            // pointer rule calls it a pointer. All quads -> the draw is skipped; some -> each run of them is a skipped index range.
            static_assert(k_ui_max_q <= 32, "qmask is 32 bits: k_ui_max_q > 32 needs a uint64_t mask");
            uint32_t qmask = 0;
            int qactor[k_ui_max_q] = {}; const float* qxy = nullptr;   // per hidden quad: the actor it sits over; the cursor art is captured
                                                                       // once the hidden set is final (B6-2: not for runs past the 8th)
            if (!hide && (g_ui_hide_cursor || g_hide_pointers) && g_draw_kind == 1 && g_draw_n >= 6 && g_draw_n % 6 == 0) {
                unsigned tw = 0, th = 0; if (t) { D3D11_TEXTURE2D_DESC d = {}; t->GetDesc(&d); tw = d.Width; th = d.Height; }
                const float* xy = nullptr; unsigned nv = 0; const unsigned nq_draw = g_draw_n / 6; bool examined = false;
                bool any = false; if (g_ui_hide_cursor) for (int i = 0; i < g_hide_sig_n; ++i) if (g_hide_sigs[i].tw == tw && g_hide_sigs[i].th == th) any = true;
                if (any) {
                    // every quad of the draw (the game batches several sprites into one draw): a quad that matches a signature is left out
                    nv = ui_quads_cached(&xy); const unsigned nq = nv / 4; examined = true;
                    g_rect_checked++;
                    for (unsigned q = 0; q < nq; ++q) {
                        float x0 = xy[q*8], x1 = x0, y0 = xy[q*8+1], y1 = y0;
                        for (unsigned k = 1; k < 4; ++k) { const float x = xy[q*8+k*2], y = xy[q*8+k*2+1]; if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
                        const float w = x1 - x0, h = y1 - y0;
                        for (int i = 0; i < g_hide_sig_n; ++i) {
                            const HideSig& g = g_hide_sigs[i];
                            if (g.tw == tw && g.th == th && w >= g.w0 && w <= g.w1 && h >= g.h0 && h <= g.h1) {
                                g_hide_sig_hits[i]++; qmask |= 1u << q;
                                if (g_actor_ui_frame != (int)g_frames) { g_actor_ui_frame = (int)g_frames; actors_project_ui(); }
                                int actor = -1; quad_is_pointer(x0, y0, x1, y1, &actor); qactor[q] = actor;
                                break;
                            }
                        }
                    }
                    if (nv == 0) g_rect_unknown++;
                }
                if (g_hide_pointers) {   // the pointer rule, for the quads the hide list left (it no longer stops at a hide-list match)
                    if (!t || tw == 1024 || tw == 8192) { nv = ui_quads_cached(&xy); ui_backgrounds_note(xy, nv); }   // the window art lives on these
                    if (!t || (tw == 1024 && th == 1024)) {
                        if (g_actor_ui_frame != (int)g_frames) { g_actor_ui_frame = (int)g_frames; actors_project_ui(); }
                        const unsigned nq = nv / 4; examined = true;
                        for (unsigned q = 0; q < nq; ++q) {
                            if (qmask & (1u << q)) continue;
                            float x0, y0, x1, y1; quad_rect(xy, q, &x0, &y0, &x1, &y1);
                            int actor = -1;
                            if (quad_is_pointer(x0, y0, x1, y1, &actor) && !quad_in_run(xy, nq, q)) { g_pointer_hits++; qmask |= 1u << q; qactor[q] = actor; }
                        }
                    }
                }
                if (examined && nq_draw > k_ui_max_q) g_ui_trunc++;   // quads past the cap are never hidden (R39)
                int matched = 0; for (uint32_t m = qmask; m != 0; m &= m - 1) matched++;   // bit count (no __popcnt64 on x86)
                if (matched > 0 && nv / 4 >= nq_draw && matched == (int)nq_draw) hide = true;
                qxy = xy;
            }
            if (!hide && qmask != 0) {   // runs of hidden quads -> index ranges, the rest drawn in pieces around them
                g_skip_n = 0;
                for (unsigned q = 0; q < k_ui_max_q; ) {
                    if (!(qmask & (1u << q))) { ++q; continue; }
                    unsigned e = q; while (e + 1 < k_ui_max_q && (qmask & (1u << (e + 1)))) ++e;
                    if (g_skip_n < 8) g_skip[g_skip_n++] = { q * 6, (e - q + 1) * 6 };
                    else { g_skip_overflow++; for (unsigned b = q; b <= e; ++b) qmask &= ~(1u << b); }   // extra runs stay visible: not hidden, no art
                    q = e + 1;
                }
            }
            if (qmask != 0 && qxy) {   // the game's cursor art for the quads that really are hidden (in quad order)
                for (unsigned q = 0; q < k_ui_max_q; ++q) {
                    if (!(qmask & (1u << q))) continue;
                    float x0, y0, x1, y1; quad_rect(qxy, q, &x0, &y0, &x1, &y1); ptr_sprite_capture(q, x0, y0, x1, y1, qactor[q]);
                }
            }
            if (!hide && qmask != 0) {
                g_ui_draws_frame++;
                return (g_ui_flat_draw_now ? 0 : 1) | 0x20000;   // drawn in pieces around the hidden quads
            }
            if (!t && (g_ui_hide_notex == 1 || (g_ui_hide_notex == 2 && g_draw_n != 18))) hide = true;   // textureless experiment; 2 keeps the three 18-index window backgrounds
            if (hide) {
                if (g_frame_live) {
                    char t2[96]; snprintf(t2, sizeof(t2), "      ui draw %u (frame draw %d): HIDDEN by ui_hide (tex %s, n=%u)", g_ui_draws_frame, idx, t ? "yes" : "no", g_draw_n);
                    { std::lock_guard<std::mutex> lock(g_mutex); FrameEv e; strncpy_s(e.text, t2, _TRUNCATE); if (g_frame_ev.size() < 2000) g_frame_ev.push_back(e); }
                    ui_census_geometry();   // its vertices as well, so a hidden draw can still be identified
                }
                g_ui_hidden_draws++; g_ui_draws_frame++; return -1;
            }
        }
        if (g_frame_live) {   // census: every 2D UI draw with its texture and the head of its VS constant buffer (sprite transforms live there)
            char line[400]; size_t l = 0;
            l += snprintf(line + l, sizeof(line) - l, "      ui draw %u (frame draw %d):", g_ui_draws_frame, idx);
            for (int i = 0; i < 4; ++i) {
                ID3D11Texture2D* t = texture_of(g_cur_ps_srv[i]); if (!t) continue;
                D3D11_TEXTURE2D_DESC d = {}; t->GetDesc(&d);
                l += snprintf(line + l, sizeof(line) - l, " s%d:%p %ux%u f%u", i, static_cast<void*>(t), d.Width, d.Height, static_cast<unsigned>(d.Format));
            }
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                for (int sl = 0; sl < 2; ++sl) {
                    auto it = g_vs_cb[sl] ? g_buffers.find(g_vs_cb[sl]) : g_buffers.end();
                    if (it == g_buffers.end()) continue;
                    const float* f = reinterpret_cast<const float*>(it->second.data.data()); const UINT nf = (UINT)(it->second.data.size() / 4);
                    l += snprintf(line + l, sizeof(line) - l, " | cb%d %uB:", sl, (unsigned)it->second.size);
                    for (UINT k = 0; k < nf && k < 12 && l < sizeof(line) - 16; ++k) l += snprintf(line + l, sizeof(line) - l, " %.4g", f[k]);
                    if (sl == 0 && nf > 12) {   // the rest of cb0, on extra lines (sprite transforms / atlas rects live somewhere in here)
                        FrameEv e0; strncpy_s(e0.text, line, _TRUNCATE); if (g_frame_ev.size() < 2000) g_frame_ev.push_back(e0);
                        l = (size_t)snprintf(line, sizeof(line), "            cb0+48:");
                        for (UINT k = 12; k < nf && k < 64; ++k) {
                            if (l > sizeof(line) - 24) { FrameEv e1; strncpy_s(e1.text, line, _TRUNCATE); if (g_frame_ev.size() < 2000) g_frame_ev.push_back(e1); l = (size_t)snprintf(line, sizeof(line), "            cb0+%u:", k * 4); }
                            l += snprintf(line + l, sizeof(line) - l, " %.4g", f[k]);
                        }
                    }
                }
                FrameEv e; strncpy_s(e.text, line, _TRUNCATE); if (g_frame_ev.size() < 2000) g_frame_ev.push_back(e);
            }
            ui_census_geometry();
        }
        if (g_ui_hide_cursor && (g_ui_draws_frame & 15) == 0) { float a, b, c, d; ui_sprite_rect(&a, &b, &c, &d); }   // keeps the stream set learned (cheap)
        g_ui_draws_frame++;
        if (g_ui_flat_draw_now) return 0;   // flat mode: drawn where the game puts it
        return 1;   // into the UI texture (eye2_begin handles it)
    }
    g_imgui_draw_now = (g_imgui_layer && g_overlay_phase && g_xr_want && g_imgui_rtv != nullptr && g_backbuffer && g_cur_rtv_n > 0 && texture_of(g_cur_rtv[0]) == g_backbuffer);
    if (g_imgui_draw_now) { g_imgui_draws_frame++; return 1; }   // into the ImGui layer texture (eye2_begin handles it)
    bool eye2 = g_stereo && (g_cur_rtv_n > 0 || g_cur_dsv != nullptr);
    if (g_stereo && g_backbuffer && g_cur_rtv_n > 0 && texture_of(g_cur_rtv[0]) == g_backbuffer) {
        g_bb_draws_seen++;
        if (g_overlay_phase) g_bb_draws_overlay++;
    }
    // History-blend draw (see g_hist_mode): the first draw this frame into a
    // full-resolution target that was not cleared, with blending on.
    int ret_flags = 0;
    if (g_stereo && !g_overlay_phase && g_cur_rtv_n == 1 && g_cur_dsv == nullptr && g_ctx) {
        ID3D11Texture2D* ht = texture_of(g_cur_rtv[0]);
        bool first = true;
        for (int i = 0; i < g_targets_frame_n; ++i) if (g_targets_frame[i] == ht) { first = false; break; }
        if (first && ht && g_targets_frame_n < 64) g_targets_frame[g_targets_frame_n++] = ht;
        D3D11_TEXTURE2D_DESC hd = {}; if (ht) ht->GetDesc(&hd);
        bool screen_shaped = false;                          // the backbuffer's aspect: shadow atlases (square) stay out
        if (ht && g_backbuffer) { D3D11_TEXTURE2D_DESC bb; g_backbuffer->GetDesc(&bb); const double r = (double)hd.Width * bb.Height, l = (double)hd.Height * bb.Width; screen_shaped = fabs(r - l) < 0.05 * r; }
        if (first && ht && ht != g_backbuffer && hd.SampleDesc.Count == 1 && hd.Width >= 1280 && screen_shaped) {
            bool cleared = false;
            for (int i = 0; i < g_cleared_frame_n; ++i) if (g_cleared_frame[i] == ht) { cleared = true; break; }
            if (!cleared) {
                ID3D11BlendState* bs = nullptr; float bf[4]; UINT bm = 0; g_ctx->OMGetBlendState(&bs, bf, &bm);
                bool blend = false; D3D11_BLEND_DESC bd = {}; if (bs) { bs->GetDesc(&bd); blend = bd.RenderTarget[0].BlendEnable != 0; bs->Release(); }
                if (g_frame_live) {
                    FrameEv e; snprintf(e.text, sizeof(e.text), "      draw %d %s into %p %ux%u (uncleared, first draw this frame) blend %d src %d dst %d mode %d",
                                        idx, blend ? "HISTORY BLEND" : "uncleared-target draw", (void*)ht, hd.Width, hd.Height, blend ? 1 : 0, (int)bd.RenderTarget[0].SrcBlend, (int)bd.RenderTarget[0].DestBlend, g_hist_mode);
                    std::lock_guard<std::mutex> lock(g_mutex); if (g_frame_ev.size() < 2000) g_frame_ev.push_back(e);
                }
                if (blend) {
                    g_hist_draws++;
                    if (g_hist_mode == 2) return -1;
                    if (g_hist_mode == 1) ret_flags |= 16;
                }
            }
        }
    }
    else if (g_cur_rtv_n > 0 && g_cur_rtv[0]) {            // keep the "already drawn into" list complete for the other passes too
        ID3D11Texture2D* ht = texture_of(g_cur_rtv[0]);
        bool first = true;
        for (int i = 0; i < g_targets_frame_n; ++i) if (g_targets_frame[i] == ht) { first = false; break; }
        if (first && ht && g_targets_frame_n < 64) g_targets_frame[g_targets_frame_n++] = ht;
    }

    // Scene-pass draw (multisampled target): the bisect tool and, for draws
    // without a camera image, the experiment switch; census lines for both.
    if (g_stereo && !g_overlay_phase && g_cur_rtv_n > 0 && g_cur_dsv != nullptr) {
        ID3D11Texture2D* tt = texture_of(g_cur_rtv[0]);
        D3D11_TEXTURE2D_DESC td = {}; if (tt) tt->GetDesc(&td);
        if (tt && td.SampleDesc.Count > 1) {
            const int sidx = (int)g_scene_draw_idx++;
            if (g_eye2_cur == nullptr) g_noimg_draws++;
            if (g_frame_live && g_scene_census_lines < 700 && g_ctx) {
                ID3D11Buffer* vcb[4] = {}; ID3D11Buffer* pcb[4] = {}; UINT vsz[4] = {}, psz[4] = {};
                g_ctx->VSGetConstantBuffers(0, 4, vcb); g_ctx->PSGetConstantBuffers(0, 4, pcb);
                for (int k = 0; k < 4; ++k) {
                    if (vcb[k]) { D3D11_BUFFER_DESC bd; vcb[k]->GetDesc(&bd); vsz[k] = bd.ByteWidth; vcb[k]->Release(); }
                    if (pcb[k]) { D3D11_BUFFER_DESC bd; pcb[k]->GetDesc(&bd); psz[k] = bd.ByteWidth; pcb[k]->Release(); }
                }
                ID3D11BlendState* bs = nullptr; float bf[4]; UINT bm = 0; g_ctx->OMGetBlendState(&bs, bf, &bm);
                int blend = -1; if (bs) { D3D11_BLEND_DESC bd; bs->GetDesc(&bd); blend = bd.RenderTarget[0].BlendEnable ? 1 : 0; bs->Release(); }
                ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr; g_ctx->VSGetShader(&vs, nullptr, nullptr); g_ctx->PSGetShader(&ps, nullptr, nullptr);
                ID3D11DepthStencilState* ds = nullptr; UINT sref = 0; g_ctx->OMGetDepthStencilState(&ds, &sref);
                int zwrite = -1; if (ds) { D3D11_DEPTH_STENCIL_DESC dd; ds->GetDesc(&dd); zwrite = (dd.DepthEnable && dd.DepthWriteMask) ? 1 : 0; ds->Release(); }
                char s0[48] = ""; { ID3D11Texture2D* t0 = texture_of(g_cur_ps_srv[0]); if (t0) { D3D11_TEXTURE2D_DESC td = {}; t0->GetDesc(&td); snprintf(s0, sizeof(s0), " s0 %p %ux%u", static_cast<void*>(t0), td.Width, td.Height); } }
                FrameEv e;
                snprintf(e.text, sizeof(e.text), "      scene draw %d (frame draw %d) %s: vs cb %u/%u/%u/%u ps cb %u/%u/%u/%u blend %d zwrite %d vs %p ps %p%s n=%u",
                         sidx, idx, g_eye2_cur == nullptr ? "NO IMAGE" : (g_eye2_cur->kind == 1 ? "camera" : "product"),
                         vsz[0], vsz[1], vsz[2], vsz[3], psz[0], psz[1], psz[2], psz[3], blend, zwrite, (void*)vs, (void*)ps, s0, g_draw_n);
                if (vs) vs->Release(); if (ps) ps->Release();
                std::lock_guard<std::mutex> lock(g_mutex);
                if (g_frame_ev.size() < 2000) { g_frame_ev.push_back(e); g_scene_census_lines++; }
            }
            if (g_pick_armed) { ID3D11Texture2D* t0 = texture_of(g_cur_ps_srv[0]); if (t0) { D3D11_TEXTURE2D_DESC td = {}; t0->GetDesc(&td); if (td.Width == 2048 && td.Height == 4096) g_pick_scene_text++; } }
            if (g_bisect_mode == 1 && sidx >= g_bisect_a && sidx <= g_bisect_b) return -1;
            if (g_bisect_mode == 2 && (sidx < g_bisect_a || sidx > g_bisect_b)) return -1;
            if (g_eye2_cur == nullptr) {
                if (g_noimg_mode == 2) return -1;
                if (g_noimg_mode == 1) { g_e2_no_image++; return 0; }
            }
        }
    }
    if (g_first_draw_pending) { g_imgui_cb = g_slot0_cur; g_first_draw_pending = false; }   // (kept for the census)
    if (g_overlay_phase) eye2 = false;      // between our hook and the game's first clear only the overlay draws (handled above)
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
    const int passes = (eye2 && split_ok(g_eye2_cur)) ? 3 : (eye2 ? 1 : 0);
    if (g_latched || g_ctx == nullptr || !diag_armed()) return passes | ret_flags;   // the latch only feeds read_camera

    ID3D11Buffer* b = nullptr;
    g_ctx->VSGetConstantBuffers(0, 1, &b);          // AddRefs
    if (b == nullptr) return passes | ret_flags;

    D3D11_BUFFER_DESC bd = {};
    b->GetDesc(&bd);
    // The camera lives in the ~1 KB per-object buffers (view at +64, inverse at
    // +320). The ~7 KB ones are skinned characters with a different layout.
    if (bd.ByteWidth < CAM_MIN_SIZE || bd.ByteWidth > 2048) { b->Release(); return passes | ret_flags; }

    if (g_slot0 != nullptr) g_slot0->Release();
    g_slot0             = b;                        // keep the reference
    g_latched           = true;
    g_draws_latched_at  = idx;
    return passes | ret_flags;
}

/// Called from the managed inline detour on UpdateSubresource, which the
/// runtime would not let us vtable-patch. Records (and scans) the write when
/// the target is a constant buffer being replaced whole.
extern "C" __declspec(dllexport) const void* ffxvr_on_updatesub(void* resource, const void* box, const void* src) {
    bump(C_UPDATESUB);
    { const DWORD tid = GetCurrentThreadId(); if (g_render_tid != 0 && tid != g_render_tid) { g_other_thread_calls++; g_other_thread_last = tid; } }
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
    g_ovr_enabled = (enabled != 0) || g_xr_want;   // the headset needs the override: the overlay cannot turn it off
    g_ovr_freeze  = freeze  != 0;
    g_ovr_yaw     = yaw;
    g_ovr_pitch   = pitch;
    g_ovr_move[0] = right; g_ovr_move[1] = up; g_ovr_move[2] = forward;
    if (!g_ovr_freeze) g_ovr_have_base = false;
}

extern "C" __declspec(dllexport) int ffxvr_override_applied() { return static_cast<int>(g_ovr_applied); }
extern "C" __declspec(dllexport) int ffxvr_override_mask()    { return static_cast<int>(g_ovr_mask); }


/// Appends a line to ffxvr_diag.log (the heartbeat rewrites ffxvr_native.log
/// every second, so anything appended there is lost).
static void diag(const char* fmt, ...) {
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\ffxvr_diag.log", g_dir);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a") != 0 || f == nullptr) {
        // The file opens unshared: another thread may hold it for a moment. Off the render thread (a resize's backbuffer
        // line, a main-thread caller), a few 1 ms retries instead of a lost line (B7-13); the render thread never waits.
        f = nullptr;
        if (GetCurrentThreadId() == g_render_tid) return;
        for (int k = 0; k < 3 && f == nullptr; ++k) { Sleep(1); if (_wfopen_s(&f, path, L"a") != 0) f = nullptr; }
        if (f == nullptr) return;
    }
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

static void g_plain_srvs_forget(ID3D11Texture2D* t);
static void twin_release(Twin& t) {
    for (auto& kv : t.srvs) if (kv.second) kv.second->Release();
    t.srvs.clear();
    if (t.rtv) t.rtv->Release();
    if (t.dsv) t.dsv->Release();
    if (t.tex) { g_plain_srvs_forget(t.tex); t.tex->Release(); }
    t.rtv = nullptr; t.dsv = nullptr; t.tex = nullptr;
}
static Twin* twin_of(ID3D11Texture2D* game) {
    if (game == nullptr || g_device == nullptr) return nullptr;
    D3D11_TEXTURE2D_DESC d; game->GetDesc(&d);
    uint32_t retry_after = 600;   // a failure's back-off: 600 frames, doubling per repeated failure, at most 36000
    auto it = g_twins.find(game);
    if (it != g_twins.end()) {
        // The game frees and recreates textures at area changes, and a new
        // texture can land at an old address: the twin must match the
        // texture that is there now.
        Twin& old = it->second;
        // Same size and format is not enough: a plain texture freed and an
        // MSAA one allocated at its address would get a 1-sample twin, and
        // every draw into it with the MSAA depth twin is refused (eye 2 blank).
        const bool same = old.w == d.Width && old.h == d.Height && old.fmt == d.Format && old.samples == d.SampleDesc.Count && old.mips == d.MipLevels && old.bind == d.BindFlags;
        if (old.tex == nullptr) {   // a failed twin (R12): the same texture is retried after a back-off, a different one at once
            if (same && g_frames - old.fail_frame < old.retry_after) return &old;   // no last_used refresh: the sweep may drop it
            if (same) retry_after = old.retry_after * 2 > 36000u ? 36000u : old.retry_after * 2;
            g_twins.erase(it);
        }
        else {
            old.last_used = g_frames;
            if (same) return &old;
            twin_release(old);
            g_twins.erase(it);
            g_twins_recycled++;
            if (g_twins_recycled <= 10) diag("twin recycled: %p now %ux%u fmt %u samples %u mips %u bind 0x%X", static_cast<void*>(game), d.Width, d.Height, static_cast<unsigned>(d.Format), d.SampleDesc.Count, d.MipLevels, d.BindFlags);
        }
    }

    Twin t = {}; t.game = game; t.w = d.Width; t.h = d.Height; t.fmt = d.Format; t.samples = d.SampleDesc.Count; t.mips = d.MipLevels; t.bind = d.BindFlags; t.last_used = g_frames;

    D3D11_TEXTURE2D_DESC cd = d;
    cd.Usage = D3D11_USAGE_DEFAULT; cd.CPUAccessFlags = 0; cd.MiscFlags = 0;
    if (game == g_backbuffer) { cd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE; }

    auto fail = [&](const char* what, HRESULT hr) -> Twin* {
        static uint32_t logged = 0;
        if (logged++ < 20) diag("twin FAILED (%s hr=0x%08X): %ux%u fmt %u mips %u array %u samples %u bind 0x%X misc 0x%X usage %u; retry in %u frames",
             what, static_cast<unsigned>(hr), d.Width, d.Height, static_cast<unsigned>(d.Format), d.MipLevels, d.ArraySize,
             d.SampleDesc.Count, d.BindFlags, d.MiscFlags, static_cast<unsigned>(d.Usage), retry_after);
        if (t.rtv) t.rtv->Release();
        if (t.dsv) t.dsv->Release();
        if (t.tex) t.tex->Release();
        // Remembered as failed, with the description it failed for: no views; retried after the back-off, or at once
        // when a different texture appears at this address (R12; it used to be never retried, for the session).
        Twin none = {}; none.game = game; none.w = d.Width; none.h = d.Height; none.fmt = d.Format; none.samples = d.SampleDesc.Count; none.mips = d.MipLevels; none.bind = d.BindFlags;
        none.last_used = g_frames; none.fail_frame = g_frames; none.retry_after = retry_after;
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
    if (t.srvs.size() >= 256) g_twin_srv_trim = true;   // the game recreates SRVs; stale keys would pile up forever. Trimmed at the
                                                         // frame boundary: freeing here could free a view bound to an earlier slot of this same draw
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
/// R12: at begin_frame (step 7), the SRV caches that reached 256 entries are emptied, except the nullptr key: ImGui
/// holds srvs[nullptr] across begin_frame (ffxvr_eye2_srv -> ffxvr_eye2_final_srv; the eye preview is recorded before begin_frame).
static void twin_srv_trim() {
    if (!g_twin_srv_trim) return;
    g_twin_srv_trim = false;
    for (auto& tw : g_twins) {
        Twin& t = tw.second;
        if (t.srvs.size() < 256) continue;
        for (auto it = t.srvs.begin(); it != t.srvs.end();) {
            if (it->first == nullptr) { ++it; continue; }
            if (it->second) it->second->Release();
            it = t.srvs.erase(it);
        }
        g_twin_srvs_dropped++;
    }
}


/// The four snapshot objects, released together (a new source, or idle in sweep_caches).
static void snap_release() {
    if (g_snap_srv)      { g_snap_srv->Release();      g_snap_srv = nullptr; }
    if (g_snap_tex)      { g_plain_srvs_forget(g_snap_tex);  g_snap_tex->Release();  g_snap_tex = nullptr; }
    if (g_snap_game_srv) { g_snap_game_srv->Release(); g_snap_game_srv = nullptr; }
    if (g_snap_game)     { g_plain_srvs_forget(g_snap_game); g_snap_game->Release(); g_snap_game = nullptr; }
    g_snap_from = nullptr;
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
        snap_release();
        D3D11_TEXTURE2D_DESC d; src.tex->GetDesc(&d);
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = 0;
        if (SUCCEEDED(g_device->CreateTexture2D(&d, nullptr, &g_snap_tex)))  g_device->CreateShaderResourceView(g_snap_tex,  nullptr, &g_snap_srv);
        if (SUCCEEDED(g_device->CreateTexture2D(&d, nullptr, &g_snap_game))) g_device->CreateShaderResourceView(g_snap_game, nullptr, &g_snap_game_srv);
        g_snap_from = game;
    }
    {
        InEye2 guard;                                  // our copies must not be mirrored
        if (g_snap_tex)  g_ctx->CopyResource(g_snap_tex,  src.tex);
        if (g_snap_game) g_ctx->CopyResource(g_snap_game, game);
    }
    g_snap_frame = g_frames;
}

/// Snapshot both eyes if `tex` is the texture that took the scene pass this
/// frame and no snapshot was taken yet. `how` records the trigger.
static bool snapshot_if_scene(ID3D11Texture2D* tex, uint32_t how) {
    if (!snap_wanted()) return false;
    if (!g_stereo || tex == nullptr || g_snap_frame == g_frames) return false;
    if (tex != g_scene_live_tex || g_scene_live_draws < 60) return false;
    snapshot_scene(tex);
    g_snap_by = how;
    return true;
}

/// The UI pass starts: both eye images are the scene resolved just before
/// it; the UI texture is cleared to transparent for the pass's draws.
static ID3D11Texture2D* ensure_copy(ID3D11Texture2D*& tex, ID3D11RenderTargetView*& rtv, const D3D11_TEXTURE2D_DESC& like);
static void ui_pass_begin() {
    if (g_scene_resolved == nullptr || g_device == nullptr) return;
    D3D11_TEXTURE2D_DESC d; g_scene_resolved->GetDesc(&d);
    if (!ensure_copy(g_eye1_final, g_eye1_final_rtv, d) || !ensure_copy(g_eye2_final, g_eye2_final_rtv, d) || !ensure_copy(g_ui_tex, g_ui_rtv, d)) return;
    if (g_ui_blend == nullptr) {
        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;  bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;  bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;   bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        g_device->CreateBlendState(&bd, &g_ui_blend);
    }
    ID3D11Texture2D* twin = nullptr;
    { auto it = g_twins.find(g_scene_resolved); if (it != g_twins.end()) twin = it->second.tex; }
    {
        InEye2 guard;
        g_ctx->CopyResource(g_eye1_final, g_scene_resolved);
        g_ctx->CopyResource(g_eye2_final, twin ? twin : g_scene_resolved);
        const float clear[4] = { 0.f, 0.f, 0.f, 0.f };
        g_ctx->ClearRenderTargetView(g_ui_rtv, clear);
    }
    g_ui_state = UI_PASS;
    g_ui_draws_frame = 0;
}

/// Switches every current target to its twin, every sampled render texture
/// to its twin, and uploads the eye-2 image of the object buffer if there is
/// one. The managed detour then re-issues the draw.
ID3D11ShaderResourceView* g_saved_ps_srv[16] = {};
ID3D11BlendState* g_saved_blend = nullptr; float g_saved_blend_factor[4]; UINT g_saved_sample_mask = 0xFFFFFFFF;
bool g_ui_pass_active_draw = false;
extern "C" __declspec(dllexport) int ffxvr_eye2_begin(int pass) {
    QueryPerformanceCounter(&g_e2_t0);
    (void)pass;
    if (g_ui_draw_now || g_imgui_draw_now) {
        ID3D11RenderTargetView* target = g_imgui_draw_now ? g_imgui_rtv : g_ui_rtv;
        if (target == nullptr || g_ctx == nullptr) return 0;
        g_in_eye2 = true; g_ui_pass_active_draw = true;
        for (int sl = 0; sl < 14; ++sl) g_eye2_slots[sl] = nullptr;
        for (int i = 0; i < 16; ++i) g_saved_ps_srv[i] = g_cur_ps_srv[i];
        g_ctx->OMGetBlendState(&g_saved_blend, g_saved_blend_factor, &g_saved_sample_mask);
        ID3D11RenderTargetView* rts[1] = { target };
        g_ctx->OMSetRenderTargets(1, rts, nullptr);            // no depth: draw order is the layering
        g_ctx->OMSetBlendState(g_ui_blend, nullptr, 0xFFFFFFFF);
        return 1;
    }
    if (!g_stereo || g_ctx == nullptr || g_device == nullptr) { g_e2_begin_fail++; return 0; }
    if (g_cur_rtv_n == 0 && g_cur_dsv == nullptr) { g_e2_begin_fail++; return 0; }
    const bool split = split_ok(g_eye2_cur);
    if (split && pass == 0) {   // the right hand into eye 1: same targets and inputs, only the part's own buffer changes
        g_in_eye2 = true; g_split_pass = true;
        for (int sl = 0; sl < 14; ++sl) g_eye2_slots[sl] = nullptr;
        for (int i = 0; i < 16; ++i) g_saved_ps_srv[i] = g_cur_ps_srv[i];
        for (int sl = 0; sl < 14; ++sl) if (g_vs_cb[sl] == g_eye2_cur->cb) { g_eye2_slots[sl] = g_eye2_cur; g_ctx->UpdateSubresource(g_eye2_cur->cb, 0, nullptr, g_eye2_cur->eye1b.data(), 0, 0); break; }
        return 1;
    }
    g_split_pass = false;
    const bool right_variant = split && pass == 2;

    ID3D11RenderTargetView* rtvs[4] = {};
    for (UINT i = 0; i < g_cur_rtv_n; ++i) rtvs[i] = twin_rtv(g_cur_rtv[i]);
    ID3D11DepthStencilView* dsv = twin_dsv(g_cur_dsv);
    const bool to_bb = g_backbuffer && g_cur_rtv_n > 0 && texture_of(g_cur_rtv[0]) == g_backbuffer;
    if (to_bb) { g_bb_begin++; if (rtvs[0] == nullptr) g_bb_twin_null++; }
    if (g_cur_rtv_n > 0 && rtvs[0] == nullptr) { g_e2_begin_fail++; return 0; }
    if (g_cur_dsv != nullptr && dsv == nullptr) g_e2_no_depth_twin++;   // R12: eye 2 draws without depth here (unchanged); counted

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
    bool e2_data_differs = false;
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
        const bool same = g_eye2_slots[sl]->eye1 == g_eye2_slots[sl]->eye2;
        if (same) g_e2_identical++;
        const bool rv = right_variant && g_eye2_slots[sl] == g_eye2_cur;   // the split right-hand pass uploads eye2b (eye 1 drew eye1b)
        if (rv ? g_eye2_slots[sl]->eye2b != g_eye2_slots[sl]->eye1b : !same) e2_data_differs = true;   // R23: only eye-2 data that differs from eye 1's counts
        g_ctx->UpdateSubresource(cb, 0, nullptr, rv ? g_eye2_slots[sl]->eye2b.data() : g_eye2_slots[sl]->eye2.data(), 0, 0);
    }
    if (!any && !e2_data_differs) g_e2_same_inputs++;   // R23: no swapped input, no eye-2 constant data that differs: a true upper bound
    return 1;
}

/// Restores the game's targets, sampled textures and the eye-1 image.
extern "C" __declspec(dllexport) void ffxvr_eye2_end() {
    { LARGE_INTEGER t1; QueryPerformanceCounter(&t1); if (g_qpf.QuadPart && g_e2_t0.QuadPart) g_e2_ms_this_frame += (t1.QuadPart - g_e2_t0.QuadPart) * 1000.0 / g_qpf.QuadPart; }
    if (!g_in_eye2 || g_ctx == nullptr) return;
    g_ctx->OMSetRenderTargets(g_cur_rtv_n, g_cur_rtv, g_cur_dsv);
    if (g_ui_pass_active_draw) {
        g_ui_pass_active_draw = false;
        g_ctx->OMSetBlendState(g_saved_blend, g_saved_blend_factor, g_saved_sample_mask);
        if (g_saved_blend) { g_saved_blend->Release(); g_saved_blend = nullptr; }
        g_in_eye2 = false;
        return;
    }
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
    if (views == nullptr || !g_stereo || !snap_wanted()) return;   // R3: no per-bind texture_of unless a snapshot is wanted
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
    if (dst && src && !g_stereo) {
        // flat mode: the scene target (most draws so far this frame, with depth) resolved -> the UI follows
        if (src == g_flat_scene_tex && g_flat_scene_draws >= 20) {
            if (g_ui_flat_pass && g_ui_draws_frame > 0) { g_ui_flat_pass = false; g_ui_draws_report = g_ui_draws_frame; }
            else g_ui_flat_resolved = true;
        }
    }
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
                { InEye2 guard; g_ctx->ResolveSubresource(dt->tex, dst_sub, sit->second.tex, src_sub, static_cast<DXGI_FORMAT>(fmt)); }
                g_resolves_mirrored++;
                // The first resolve of the scene pass is the finished scene, in a
                // plain (non-multisampled) texture: that is the eye image.
                if (snap_wanted() && src == g_scene_live_tex && g_scene_live_draws >= 60 && g_snap_frame != g_frames) { snapshot_scene(dst); g_snap_by = 2; }
                if (src == g_scene_live_tex && g_scene_live_draws >= 20) {
                    g_ui_resolves_frame++;
                    // The UI pass ends at the resolve AFTER its draws. Some areas
                    // (the boat to Kilika) resolve once more between the depth
                    // clear that opens the pass and the first UI draw: that
                    // resolve is just a newer base, the pass reopens at the next
                    // depth clear. Once done, done: later resolves do not reopen.
                    if (g_ui_flat_pass && g_ui_draws_frame > 0) { g_ui_flat_pass = false; g_ui_draws_report = g_ui_draws_frame; }
                    if (g_ui_state == UI_PASS && g_ui_early) { g_ui_early = false; g_ui_early_keep = true; g_ui_state = UI_RESOLVED; }   // pause; the eye images stay as copied
                    else if (g_ui_state == UI_PASS) {
                        if (g_ui_draws_frame > 0) { g_ui_state = UI_DONE; g_ui_pass_frame = g_frames; g_ui_draws_report = g_ui_draws_frame; }
                        else { g_ui_state = UI_RESOLVED; g_scene_resolved = dst; }
                    }
                    else if (g_ui_state != UI_DONE) { g_ui_state = UI_RESOLVED; g_scene_resolved = dst; }
                }
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
static void ui_pass_begin();
static void ui_pass_open() {
    if (g_ui_early_keep) { g_ui_early_keep = false; g_ui_state = UI_PASS; return; }   // resume after an early overlay run
    ui_pass_begin();
}
extern "C" __declspec(dllexport) void ffxvr_on_clear_twin(void* view, int depth, const float* color, unsigned flags, float d, unsigned char st) {
    if (g_in_eye2 || g_ctx == nullptr) return;
    if (!g_stereo) {   // flat mode: the depth clear on the resolved scene target opens the UI pass
        if (depth && g_ui_flat_resolved && g_cur_rtv_n > 0 && texture_of(g_cur_rtv[0]) == g_flat_scene_tex) { g_ui_flat_pass = true; g_ui_flat_resolved = false; g_ui_draws_frame = 0; }
        return;
    }
    if (!depth) {
        ID3D11Texture2D* ct = texture_of(static_cast<ID3D11View*>(view));
        if (ct && g_cleared_frame_n < 64) g_cleared_frame[g_cleared_frame_n++] = ct;
        snapshot_if_scene(ct, 3);   // about to be wiped: last chance
    }
    if (g_scene_live_tex && g_scene_live_draws >= 20) {
        ID3D11Texture2D* bound = g_cur_rtv_n > 0 ? texture_of(g_cur_rtv[0]) : nullptr;
        if (!depth && texture_of(static_cast<ID3D11View*>(view)) == g_scene_live_tex) { if (g_ui_state == UI_RESOLVED) g_ui_state = UI_IDLE; }   // colour wiped: post ping-pong, not the UI base
        else if (depth && bound == g_scene_live_tex) {
            g_ui_depth_clears_frame++;
            if (g_ui_state == UI_RESOLVED && g_xr_want && g_ui_enabled && !g_ovr_suspend && !g_screen3d_now) ui_pass_open();   // screen frames keep their UI (subtitles) in the frame
            else if (g_ui_state == UI_RESOLVED && !g_xr_want) { g_ui_flat_pass = true; g_ui_flat_resolved = false; g_ui_draws_frame = 0; }   // stereo on, headset off: recognise the pass, draw normally
        }
    }
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
        snprintf(t, sizeof(t), "CPY %s %p %ux%u fmt %u%s -> %p %ux%u fmt %u%s%s%s", whole ? "whole" : "region",
                 static_cast<void*>(src), sd.Width, sd.Height, static_cast<unsigned>(sd.Format), st ? " (twinned)" : " (NO twin)",
                 static_cast<void*>(dst), dd.Width, dd.Height, static_cast<unsigned>(dd.Format), dt ? " (twinned)" : "",
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
    { InEye2 guard; g_ctx->CopyResource(sa, a); g_ctx->CopyResource(sb, b); }   // our copies must not be mirrored

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
    { InEye2 guard; g_ctx->CopyResource(st, tex); }
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
static void measure_now() {
    diag("measure (eye separation %.2f): eye-2 uploads %u (identical to eye 1: %u), re-entered UpdateSubresource calls %u, red clears %u, twins %u",
         g_ipd, g_e2_uploads, g_e2_identical, g_e2_reentered_updates, g_red_clears, g_twin_count);
    diag("    draw contexts: on the hooked immediate context %u, on another context %u (last %p); hooked context %p", g_draw_ctx_same, g_draw_ctx_other, g_draw_ctx_last_other, static_cast<void*>(g_ctx));
    {
        char line[256]; size_t l = 0;
        for (int sl = 0; sl < 14; ++sl) l += snprintf(line + l, sizeof(line) - l, " s%d:%u", sl, g_slot_binds[sl]);
        diag("    VS constant buffer slot binds:%s", line);
    }
    diag("    hook entries from threads other than the render thread (%lu): %u (last %lu)", g_render_tid, g_other_thread_calls, g_other_thread_last);
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
        D3D11_TEXTURE2D_DESC d = {}; if (g_snap_tex) g_snap_tex->GetDesc(&d);   // our own copy (same size and format), not the game's un-AddRef'd texture
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
}
/// The Measure button. Snapshots are on demand (R3): with a fresh one (from the frame just finished) it measures at
/// once, as before; otherwise it asks for snapshots and measures at the end of a later begin_frame (at most 5 frames).
extern "C" __declspec(dllexport) int ffxvr_measure() {
    if (g_snap_tex && g_snap_frame + 1 >= g_frames) { measure_now(); return 0; }
    g_measure_pending = true; g_measure_deadline = g_frames + 5; snap_request(5);
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

extern "C" __declspec(dllexport) void  ffxvr_stereo_set(int on, float ipd, int scope, int red, int leave) {
    // While the headset is on, eye 2 is on: the overlay cannot turn it off.
    g_stereo = (on != 0) || g_xr_want; g_ipd = ipd; g_stereo_scope = scope; g_debug_red = red != 0; g_debug_leave = leave != 0;
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
    "float4 ps(V i) : SV_Target { return float4(t.Sample(s, i.uv).rgb, 1); }\n"
    "float4 ps_alpha(V i) : SV_Target { return t.Sample(s, i.uv); }\n";
ID3D11VertexShader* g_copy_vs = nullptr; ID3D11PixelShader* g_copy_ps = nullptr; ID3D11PixelShader* g_copy_ps_alpha = nullptr; ID3D11SamplerState* g_copy_smp = nullptr;
bool g_copy_tried = false;
struct OpaqueCopy { ID3D11Texture2D* tex = nullptr; ID3D11RenderTargetView* rtv = nullptr; ID3D11ShaderResourceView* srv = nullptr; UINT w = 0, h = 0; };
OpaqueCopy g_opaque[3];
static void opaque_release(int slot) {
    if (slot < 0 || slot > 2) return;
    OpaqueCopy& oc = g_opaque[slot];
    if (oc.srv) oc.srv->Release(); if (oc.rtv) oc.rtv->Release(); if (oc.tex) { g_plain_srvs_forget(oc.tex); oc.tex->Release(); }
    oc = OpaqueCopy{};
}

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
    {
        ID3DBlob *pab = nullptr, *err2 = nullptr;
        HRESULT h2 = compile(k_copy_hlsl, sizeof(k_copy_hlsl) - 1, "copy", nullptr, nullptr, "ps_alpha", "ps_4_0", 0, 0, &pab, &err2);
        if (SUCCEEDED(h2)) { g_device->CreatePixelShader(pab->GetBufferPointer(), pab->GetBufferSize(), nullptr, &g_copy_ps_alpha); pab->Release(); }
        else diag("copy pass: ps_alpha compile failed 0x%08X", static_cast<unsigned>(h2));
        if (err2) err2->Release();
    }
    if (FAILED(hr)) { diag("copy pass: shader create failed 0x%08X", static_cast<unsigned>(hr)); return false; }
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_device->CreateSamplerState(&sd, &g_copy_smp);
    diag("copy pass: ready");
    return true;
}

static std::map<ID3D11Texture2D*, ID3D11ShaderResourceView*> g_plain_srvs;   // per source texture
static uint32_t g_plain_srvs_count() { return (uint32_t)g_plain_srvs.size(); }
static void g_plain_srvs_forget(ID3D11Texture2D* t) { auto it = g_plain_srvs.find(t); if (it != g_plain_srvs.end()) { if (it->second) it->second->Release(); g_plain_srvs.erase(it); } }
bool ffxvr_copy_pass(ID3D11Texture2D* src, ID3D11RenderTargetView* dst, UINT w, UINT h, bool keep_alpha = false);

/// Renders `src` into the opaque copy for `slot` (0..2) and returns its SRV.
static ID3D11ShaderResourceView* opaque_copy(int slot, ID3D11Texture2D* src) {
    if (src == nullptr || g_device == nullptr || g_ctx == nullptr || slot < 0 || slot > 2) return nullptr;
    D3D11_TEXTURE2D_DESC d; src->GetDesc(&d);
    if (d.SampleDesc.Count != 1) return nullptr;
    OpaqueCopy& oc = g_opaque[slot];
    if (oc.tex == nullptr || oc.w != d.Width || oc.h != d.Height) {
        if (oc.srv) oc.srv->Release(); if (oc.rtv) oc.rtv->Release(); if (oc.tex) { g_plain_srvs_forget(oc.tex); oc.tex->Release(); }
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
    return ffxvr_copy_pass(src, oc.rtv, oc.w, oc.h, false) ? oc.srv : nullptr;
}

/// Stretch-copies `src` into `dst` (w x h) with alpha forced to 1, saving
/// and restoring the pipeline state it touches. Also used for the headset.
bool ffxvr_copy_pass(ID3D11Texture2D* src, ID3D11RenderTargetView* dst, UINT w, UINT h, bool keep_alpha) {
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

    InEye2 guard;                                      // our hooks ignore this pass, restore included
    ID3D11RenderTargetView* rt[1] = { dst };
    g_ctx->OMSetRenderTargets(1, rt, nullptr);
    D3D11_VIEWPORT vp = {}; vp.Width = static_cast<float>(w); vp.Height = static_cast<float>(h); vp.MaxDepth = 1.f;
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->VSSetShader(g_copy_vs, nullptr, 0); g_ctx->PSSetShader((keep_alpha && g_copy_ps_alpha) ? g_copy_ps_alpha : g_copy_ps, nullptr, 0); g_ctx->GSSetShader(nullptr, nullptr, 0);
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
    for (auto* v : o_rtv) if (v) v->Release();
    if (o_dsv) o_dsv->Release(); if (o_vs) o_vs->Release(); if (o_ps) o_ps->Release(); if (o_gs) o_gs->Release();
    if (o_srv) o_srv->Release(); if (o_smp) o_smp->Release(); if (o_bs) o_bs->Release(); if (o_ds) o_ds->Release();
    if (o_rs) o_rs->Release(); if (o_il) o_il->Release();
    return true;
}

// ---------------------------------------------------------------------------
// 3D markers: small camera-facing triangles at world positions, drawn into
// each eye's image with that eye's view and projection, so they sit on the
// combatant at the right depth (the game's cursors are placed through its
// own camera and land in the wrong place on the UI panel).

struct Marker { float x, y, z; uint32_t color; int kind; };   // kind 0 target, 1 turn
float g_marker_scale = 1.f;
Marker g_markers[32]; int g_marker_n = 0;
static const char k_marker_hlsl[] =
    "cbuffer C : register(b0) { row_major float4x4 VP; };\n"
    "struct VI { float3 p : POSITION; float4 c : COLOR; };\n"
    "struct VO { float4 p : SV_Position; float4 c : COLOR; };\n"
    "VO vs(VI i) { VO o; o.p = mul(VP, float4(i.p, 1)); o.p.z = 0.5 * o.p.w; o.c = i.c; return o; }\n"
    "float4 ps(VO i) : SV_Target { return i.c; }\n";
ID3D11VertexShader* g_mk_vs = nullptr; ID3D11PixelShader* g_mk_ps = nullptr; ID3D11InputLayout* g_mk_il = nullptr;
ID3D11Buffer* g_mk_vb = nullptr; ID3D11Buffer* g_mk_cb = nullptr; ID3D11BlendState* g_mk_blend = nullptr;
bool g_mk_tried = false;
struct MkVertex { float p[3]; float c[4]; };
static const int k_mk_max_verts = 32 * 48;

static bool marker_pass_init() {
    if (g_mk_tried) return g_mk_vs != nullptr;
    g_mk_tried = true;
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    PFN_D3DCompile compile = m ? reinterpret_cast<PFN_D3DCompile>(GetProcAddress(m, "D3DCompile")) : nullptr;
    if (!compile) { diag("marker pass: no compiler"); return false; }
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    HRESULT hr = compile(k_marker_hlsl, sizeof(k_marker_hlsl) - 1, "mk", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err);
    if (FAILED(hr)) { diag("marker pass: vs failed: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?"); if (err) err->Release(); return false; }
    if (err) { err->Release(); err = nullptr; }
    hr = compile(k_marker_hlsl, sizeof(k_marker_hlsl) - 1, "mk", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err);
    if (FAILED(hr)) { diag("marker pass: ps failed: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?"); if (err) err->Release(); vsb->Release(); return false; }
    if (err) err->Release();
    g_device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_mk_vs);
    g_device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_mk_ps);
    D3D11_INPUT_ELEMENT_DESC ie[2] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
    g_device->CreateInputLayout(ie, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_mk_il);
    vsb->Release(); psb->Release();
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(MkVertex) * k_mk_max_verts; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_device->CreateBuffer(&bd, nullptr, &g_mk_vb);
    D3D11_BUFFER_DESC cd = {};
    cd.ByteWidth = 64; cd.Usage = D3D11_USAGE_DYNAMIC; cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_device->CreateBuffer(&cd, nullptr, &g_mk_cb);
    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_device->CreateBlendState(&bl, &g_mk_blend);
    const bool ok = g_mk_vs && g_mk_ps && g_mk_il && g_mk_vb && g_mk_cb;
    diag("marker pass: %s", ok ? "ready" : "FAILED");
    return ok;
}

/// Draws the markers into `dst` (w x h) for one eye: `V` the eye's view
/// (row-major 4x4, column-vector convention), `f` its field of view.
// Draws the captured pointer sprites into one eye image with the game's own UI pipeline. Each sprite goes
// to the marker nearest its actor (markers are in render space at the actor plus the marker height); the
// quad keeps its UI size, bottom-centred on the marker's projection. Returns a mask of markers that got one.
static uint32_t draw_ptr_sprites_eye(ID3D11RenderTargetView* dst, UINT w, UINT h, const float VP[16]) {
    uint32_t used = 0;
    if (g_ptr_sprite_n == 0 || !g_ptr_reuse || g_marker_n == 0 || !g_device) return 0;
    if (!g_ptr_vb) {
        D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 4 * 12; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        g_device->CreateBuffer(&bd, nullptr, &g_ptr_vb);
        const uint16_t idx[6] = { 0, 1, 2, 2, 1, 3 };   // the game's quad pattern (v0 top-left, v1 top-right, v2 bottom-left, v3 bottom-right)
        D3D11_BUFFER_DESC ib = {}; ib.ByteWidth = sizeof(idx); ib.Usage = D3D11_USAGE_IMMUTABLE; ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA init = { idx, 0, 0 }; g_device->CreateBuffer(&ib, &init, &g_ptr_ib);
        if (!g_ptr_vb || !g_ptr_ib) return 0;
    }
    // save what we touch
    ID3D11RenderTargetView* o_rtv[4] = {}; ID3D11DepthStencilView* o_dsv = nullptr; g_ctx->OMGetRenderTargets(4, o_rtv, &o_dsv);
    UINT nvp = 1; D3D11_VIEWPORT o_vp = {}; g_ctx->RSGetViewports(&nvp, &o_vp);
    ID3D11VertexShader* o_vs = nullptr; ID3D11PixelShader* o_ps = nullptr; ID3D11GeometryShader* o_gs = nullptr;
    g_ctx->VSGetShader(&o_vs, nullptr, nullptr); g_ctx->PSGetShader(&o_ps, nullptr, nullptr); g_ctx->GSGetShader(&o_gs, nullptr, nullptr);
    ID3D11Buffer* o_cb = nullptr; g_ctx->VSGetConstantBuffers(0, 1, &o_cb);
    ID3D11Buffer* o_vb[8] = {}; UINT o_stride[8] = {}, o_off[8] = {}; g_ctx->IAGetVertexBuffers(0, 8, o_vb, o_stride, o_off);
    ID3D11Buffer* o_ib = nullptr; DXGI_FORMAT o_ifmt = DXGI_FORMAT_UNKNOWN; UINT o_ioff = 0; g_ctx->IAGetIndexBuffer(&o_ib, &o_ifmt, &o_ioff);
    ID3D11InputLayout* o_il = nullptr; g_ctx->IAGetInputLayout(&o_il);
    D3D11_PRIMITIVE_TOPOLOGY o_topo; g_ctx->IAGetPrimitiveTopology(&o_topo);
    ID3D11BlendState* o_bs = nullptr; float o_bf[4]; UINT o_mask; g_ctx->OMGetBlendState(&o_bs, o_bf, &o_mask);
    ID3D11DepthStencilState* o_ds = nullptr; UINT o_ref; g_ctx->OMGetDepthStencilState(&o_ds, &o_ref);
    ID3D11RasterizerState* o_rs = nullptr; g_ctx->RSGetState(&o_rs);
    ID3D11ShaderResourceView* o_srv = nullptr; g_ctx->PSGetShaderResources(0, 1, &o_srv); ID3D11SamplerState* o_smp = nullptr; g_ctx->PSGetSamplers(0, 1, &o_smp);

    InEye2 guard;
    ID3D11RenderTargetView* rt[1] = { dst }; g_ctx->OMSetRenderTargets(1, rt, nullptr);
    D3D11_VIEWPORT vp = {}; vp.Width = static_cast<float>(w); vp.Height = static_cast<float>(h); vp.MaxDepth = 1.f; g_ctx->RSSetViewports(1, &vp);
    g_ctx->GSSetShader(nullptr, nullptr, 0); g_ctx->OMSetDepthStencilState(nullptr, 0); g_ctx->RSSetState(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); g_ctx->IASetIndexBuffer(g_ptr_ib, DXGI_FORMAT_R16_UINT, 0);
    for (int i = 0; i < g_ptr_sprite_n; ++i) {
        const PtrSprite& p = g_ptr_sprites[i];
        if (!p.has_world || !p.vs || !p.ps || !p.il || !p.vb[0]) continue;
        // the marker for this actor: nearest in the ground plane (markers sit marker_height above the root)
        int m = -1; float md = 1e9f;
        for (int k = 0; k < g_marker_n; ++k) { const float dx = g_markers[k].x - p.world[0], dz = g_markers[k].z - p.world[2]; const float d = dx*dx + dz*dz; if (d < md) { md = d; m = k; } }
        if (m < 0 || md > 40.f * 40.f || (used & (1u << m))) { g_ptr_unpaired++; continue; }
        const float wp[4] = { g_markers[m].x, g_markers[m].y, g_markers[m].z, 1.f };
        float c[4]; for (int r = 0; r < 4; ++r) c[r] = VP[r*4+0]*wp[0] + VP[r*4+1]*wp[1] + VP[r*4+2]*wp[2] + VP[r*4+3]*wp[3];
        if (c[3] <= 1e-4f) continue;
        const float ux = c[0] / c[3] * 960.f, uy = -c[1] / c[3] * 540.f;   // UI units: the game's ortho cb0 maps these to the eye image
        if (!std::isfinite(ux) || !std::isfinite(uy)) continue;
        const float x0 = ux - 0.5f * p.w, x1 = ux + 0.5f * p.w, y1 = uy, y0 = uy - p.h;   // bottom-centred on the marker (tip at the point)
        const float verts[12] = { x0, y0, 1.f, x1, y0, 1.f, x0, y1, 1.f, x1, y1, 1.f };
        D3D11_MAPPED_SUBRESOURCE ms = {};
        if (FAILED(g_orig_map(g_ctx, g_ptr_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) continue;
        memcpy(ms.pData, verts, sizeof(verts)); g_orig_unmap(g_ctx, g_ptr_vb, 0);
        ID3D11Buffer* vbs[8]; UINT strides[8], offs[8];
        for (int k = 0; k < 8; ++k) { vbs[k] = k == 0 ? g_ptr_vb : p.vb[k]; strides[k] = k == 0 ? 12u : p.stride[k]; offs[k] = k == 0 ? 0u : p.off[k]; }
        g_ctx->IASetVertexBuffers(0, 8, vbs, strides, offs); g_ctx->IASetInputLayout(p.il);
        g_ctx->VSSetShader(p.vs, nullptr, 0); g_ctx->PSSetShader(p.ps, nullptr, 0);
        g_orig_vssetcb(g_ctx, 0, 1, &p.cb0);
        g_ctx->PSSetShaderResources(0, 1, &p.srv); g_ctx->PSSetSamplers(0, 1, &p.smp);
        g_ctx->OMSetBlendState(p.bs, p.bf, p.bm);
        g_ctx->DrawIndexed(6, 0, 0);
        used |= 1u << m; g_ptr_drawn++;
    }
    {   // once a second: what was captured and what was drawn (ffxvr_diag.log)
        const double t = (double)GetTickCount64() * 0.001;
        if (t - g_ptr_log_t > 1.0) {
            g_ptr_log_t = t;
            char line[600]; size_t l = (size_t)snprintf(line, sizeof(line), "cursor art: %d captured, markers %d:", g_ptr_sprite_n, g_marker_n);
            for (int i = 0; i < g_ptr_sprite_n && l < sizeof(line) - 120; ++i) {
                const PtrSprite& p = g_ptr_sprites[i]; int slots = 0; for (int k = 0; k < 8; ++k) if (p.vb[k]) slots++;
                l += (size_t)snprintf(line + l, sizeof(line) - l, " [%d: %.0fx%.0f quad %u, %d streams, tex %s, actor %s (%.0f,%.0f,%.0f)]", i, p.w, p.h, p.q, slots, p.srv ? "yes" : "no", p.has_world ? "yes" : "NO", p.world[0], p.world[1], p.world[2]);
            }
            for (int k = 0; k < g_marker_n && l < sizeof(line) - 60; ++k) l += (size_t)snprintf(line + l, sizeof(line) - l, " m%d(%.0f,%.0f,%.0f)", k, g_markers[k].x, g_markers[k].y, g_markers[k].z);
            diag("%s | drawn so far %u, unpaired %u", line, g_ptr_drawn, g_ptr_unpaired);
        }
    }
    // restore
    g_ctx->OMSetRenderTargets(4, o_rtv, o_dsv); if (nvp) g_ctx->RSSetViewports(1, &o_vp);
    g_ctx->VSSetShader(o_vs, nullptr, 0); g_ctx->PSSetShader(o_ps, nullptr, 0); g_ctx->GSSetShader(o_gs, nullptr, 0);
    g_orig_vssetcb(g_ctx, 0, 1, &o_cb);
    g_ctx->IASetVertexBuffers(0, 8, o_vb, o_stride, o_off); g_ctx->IASetIndexBuffer(o_ib, o_ifmt, o_ioff);
    g_ctx->IASetInputLayout(o_il); g_ctx->IASetPrimitiveTopology(o_topo);
    g_ctx->OMSetBlendState(o_bs, o_bf, o_mask); g_ctx->OMSetDepthStencilState(o_ds, o_ref); g_ctx->RSSetState(o_rs);
    g_ctx->PSSetShaderResources(0, 1, &o_srv); g_ctx->PSSetSamplers(0, 1, &o_smp);
    for (auto* v : o_rtv) if (v) v->Release();
    if (o_dsv) o_dsv->Release(); if (o_vs) o_vs->Release(); if (o_ps) o_ps->Release(); if (o_gs) o_gs->Release(); if (o_cb) o_cb->Release();
    for (auto* v : o_vb) if (v) v->Release(); if (o_ib) o_ib->Release(); if (o_il) o_il->Release();
    if (o_bs) o_bs->Release(); if (o_ds) o_ds->Release(); if (o_rs) o_rs->Release(); if (o_srv) o_srv->Release(); if (o_smp) o_smp->Release();
    return used;
}

static void draw_markers_eye(ID3D11RenderTargetView* dst, UINT w, UINT h, const float V[16], const XrFovTan& f) {
    if (g_marker_n == 0 || dst == nullptr || !marker_pass_init()) return;
    // Projection from the tangents (x/y only; z is forced to mid-range in the shader).
    float P[16] = {};
    P[0] = 2.f / (f.r - f.l); P[2] = (f.r + f.l) / (f.r - f.l);
    P[5] = 2.f / (f.u - f.d); P[6] = (f.u + f.d) / (f.u - f.d);
    P[10] = -1.f; P[14] = -1.f;
    float VP[16]; mul4(P, V, VP);
    const uint32_t with_sprite = draw_ptr_sprites_eye(dst, w, h, VP);   // the game's own pointer art, where it has been captured
    // Camera axes in world space (rows of the view's rotation) for billboarding.
    const float right[3] = { V[0], V[1], V[2] }, up[3] = { V[4], V[5], V[6] };
    const float t = (float)((double)GetTickCount64() * 0.001);
    MkVertex verts[k_mk_max_verts]; int nv = 0;
    // The marker as it has been: a downward-pointing triangle, tip at the point, base above it.
    const float sz = 2.5f * g_marker_scale;
    (void)t;
    for (int i = 0; i < g_marker_n && nv + 6 <= k_mk_max_verts; ++i) {
        if (with_sprite & (1u << i)) continue;
        const Marker& m = g_markers[i];
        const float c[4] = { ((m.color >> 0) & 255) / 255.f, ((m.color >> 8) & 255) / 255.f, ((m.color >> 16) & 255) / 255.f, ((m.color >> 24) & 255) / 255.f };
        float tip[3] = { m.x, m.y, m.z };
        float a[3], b[3];
        for (int k = 0; k < 3; ++k) { a[k] = tip[k] + up[k] * sz * 2.f - right[k] * sz; b[k] = tip[k] + up[k] * sz * 2.f + right[k] * sz; }
        MkVertex v0 = { { a[0], a[1], a[2] }, { c[0], c[1], c[2], c[3] } };
        MkVertex v1 = { { b[0], b[1], b[2] }, { c[0], c[1], c[2], c[3] } };
        MkVertex v2 = { { tip[0], tip[1], tip[2] }, { c[0], c[1], c[2], c[3] } };
        verts[nv++] = v0; verts[nv++] = v1; verts[nv++] = v2;
        verts[nv++] = v0; verts[nv++] = v2; verts[nv++] = v1;      // both windings: no culling surprises
    }
    D3D11_MAPPED_SUBRESOURCE ms = {};
    if (SUCCEEDED(g_orig_map(g_ctx, g_mk_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) { memcpy(ms.pData, verts, sizeof(MkVertex) * nv); g_orig_unmap(g_ctx, g_mk_vb, 0); }
    if (SUCCEEDED(g_orig_map(g_ctx, g_mk_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) { memcpy(ms.pData, VP, 64); g_orig_unmap(g_ctx, g_mk_cb, 0); }

    // Save, draw, restore (the subset of state we touch).
    ID3D11RenderTargetView* o_rtv[4] = {}; ID3D11DepthStencilView* o_dsv = nullptr; g_ctx->OMGetRenderTargets(4, o_rtv, &o_dsv);
    UINT nvp = 1; D3D11_VIEWPORT o_vp = {}; g_ctx->RSGetViewports(&nvp, &o_vp);
    ID3D11VertexShader* o_vs = nullptr; ID3D11PixelShader* o_ps = nullptr; ID3D11GeometryShader* o_gs = nullptr;
    g_ctx->VSGetShader(&o_vs, nullptr, nullptr); g_ctx->PSGetShader(&o_ps, nullptr, nullptr); g_ctx->GSGetShader(&o_gs, nullptr, nullptr);
    ID3D11Buffer* o_cb = nullptr; g_ctx->VSGetConstantBuffers(0, 1, &o_cb);
    ID3D11Buffer* o_vb = nullptr; UINT o_stride = 0, o_off = 0; g_ctx->IAGetVertexBuffers(0, 1, &o_vb, &o_stride, &o_off);
    ID3D11InputLayout* o_il = nullptr; g_ctx->IAGetInputLayout(&o_il);
    D3D11_PRIMITIVE_TOPOLOGY o_topo; g_ctx->IAGetPrimitiveTopology(&o_topo);
    ID3D11BlendState* o_bs = nullptr; float o_bf[4]; UINT o_mask; g_ctx->OMGetBlendState(&o_bs, o_bf, &o_mask);
    ID3D11DepthStencilState* o_ds = nullptr; UINT o_ref; g_ctx->OMGetDepthStencilState(&o_ds, &o_ref);
    ID3D11RasterizerState* o_rs = nullptr; g_ctx->RSGetState(&o_rs);

    InEye2 guard;
    ID3D11RenderTargetView* rt[1] = { dst };
    g_ctx->OMSetRenderTargets(1, rt, nullptr);
    D3D11_VIEWPORT vp = {}; vp.Width = static_cast<float>(w); vp.Height = static_cast<float>(h); vp.MaxDepth = 1.f;
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->VSSetShader(g_mk_vs, nullptr, 0); g_ctx->PSSetShader(g_mk_ps, nullptr, 0); g_ctx->GSSetShader(nullptr, nullptr, 0);
    g_orig_vssetcb(g_ctx, 0, 1, &g_mk_cb);
    UINT stride = sizeof(MkVertex), off = 0; g_ctx->IASetVertexBuffers(0, 1, &g_mk_vb, &stride, &off);
    g_ctx->IASetInputLayout(g_mk_il); g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->OMSetBlendState(g_mk_blend, nullptr, 0xFFFFFFFF); g_ctx->OMSetDepthStencilState(nullptr, 0); g_ctx->RSSetState(nullptr);
    g_ctx->Draw(static_cast<UINT>(nv), 0);

    g_ctx->OMSetRenderTargets(4, o_rtv, o_dsv);
    if (nvp) g_ctx->RSSetViewports(1, &o_vp);
    g_ctx->VSSetShader(o_vs, nullptr, 0); g_ctx->PSSetShader(o_ps, nullptr, 0); g_ctx->GSSetShader(o_gs, nullptr, 0);
    g_orig_vssetcb(g_ctx, 0, 1, &o_cb);
    g_ctx->IASetVertexBuffers(0, 1, &o_vb, &o_stride, &o_off);
    g_ctx->IASetInputLayout(o_il); g_ctx->IASetPrimitiveTopology(o_topo);
    g_ctx->OMSetBlendState(o_bs, o_bf, o_mask); g_ctx->OMSetDepthStencilState(o_ds, o_ref); g_ctx->RSSetState(o_rs);
    for (auto* v : o_rtv) if (v) v->Release();
    if (o_dsv) o_dsv->Release(); if (o_vs) o_vs->Release(); if (o_ps) o_ps->Release(); if (o_gs) o_gs->Release();
    if (o_cb) o_cb->Release(); if (o_vb) o_vb->Release(); if (o_il) o_il->Release();
    if (o_bs) o_bs->Release(); if (o_ds) o_ds->Release(); if (o_rs) o_rs->Release();
}

/// Markers for this frame, in render space. Called from the managed side each frame.
extern "C" __declspec(dllexport) void ffxvr_markers_set(const float* xyz, const uint32_t* colors, int n) {
    if (n < 0) n = 0; if (n > 32) n = 32; if (!xyz || !colors) n = 0;
    for (int i = 0; i < n; ++i) { g_markers[i].x = xyz[i*3]; g_markers[i].y = xyz[i*3+1]; g_markers[i].z = xyz[i*3+2]; g_markers[i].color = colors[i] & 0x00FFFFFFu | 0xFF000000u; g_markers[i].kind = (colors[i] >> 24) == 0xFE ? 1 : 0; }
    g_marker_n = n;   // alpha 0xFE in the colour tags the turn pointer
}
extern "C" __declspec(dllexport) void ffxvr_markers_scale(float s) { g_marker_scale = s < 0.2f ? 0.2f : s > 5.f ? 5.f : s; }

/// Draws the markers into both eye images, each with its own view and FOV.
static void draw_markers_both(ID3D11Texture2D* e1, ID3D11RenderTargetView* r1, ID3D11Texture2D* e2, ID3D11RenderTargetView* r2) {
    if (g_marker_n == 0 || !g_cam_valid) return;
    float v[12];
    { std::lock_guard<std::mutex> lock(g_mutex); memcpy(v, g_cam, sizeof(v)); if (g_ovr_enabled && g_ovr_freeze && g_ovr_have_base) memcpy(v, g_ovr_base, sizeof(v)); }
    float V1[16], V2[16], I[16], Pn[3];
    g_eye_shift = 0.f;   build_override_view(v, V1, I, Pn, false);   // read-only (the markers are drawn at Present)
    g_eye_shift = 1e-6f; build_override_view(v, V2, I, Pn, false);
    g_eye_shift = 0.f;
    D3D11_TEXTURE2D_DESC d;
    if (e1 && r1) { e1->GetDesc(&d); draw_markers_eye(r1, d.Width, d.Height, V1, g_xr_fov_used[0]); }
    if (e2 && r2) { e2->GetDesc(&d); draw_markers_eye(r2, d.Width, d.Height, V2, g_xr_fov_used[1]); }
}

/// source 0: eye 2's final image (backbuffer twin); 1: the scene target's twin.
extern "C" __declspec(dllexport) void* ffxvr_eye2_srv(int source) {
    if (source == 1 || source == 2) snap_request(3);   // side effect: requests snapshots (R3)
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
extern "C" __declspec(dllexport) int   ffxvr_eye2_same_inputs() { return static_cast<int>(g_e2_same_inputs_report); }   // R23 step 1, last frame
extern "C" __declspec(dllexport) int   ffxvr_eye2_size(int* w, int* h, int source) {
    if (source == 1 || source == 2) {
        snap_request(3);   // side effect: requests snapshots (R3), so the preview starts even before the first one exists
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
// Headset start back-off (ffxvr_xr_set): after a failed xr_start, wait 2 s, then 5 s, between attempts; from the
// third failed attempt on, xr_start runs muted (g_xr_quiet) and a summary line is logged now and then instead.
// A session that dies within 5 s of starting (Virtual Desktop half-connected) counts as a failed attempt too; the
// failure count is only cleared once a session has lived 5 s.
static ULONGLONG g_xr_retry_at = 0, g_xr_started_at = 0; static uint32_t g_xr_fails = 0; static bool g_xr_quiet = false;
constexpr ULONGLONG k_xr_short_life_ms = 5000;
static void xr_diag(const char* fmt, ...) { if (g_xr_quiet) return; va_list a; va_start(a, fmt); char b[512]; vsnprintf(b, sizeof(b), fmt, a); va_end(a); diag("%s", b); }

extern "C" __declspec(dllexport) void ffxvr_xr_screen_set(int mode, float width, float dist) { g_xr_screen_mode = mode; g_xr_screen_width = width; g_xr_screen_dist = dist; }
extern "C" __declspec(dllexport) void ffxvr_backdrop_set(int mode) { g_backdrop_mode = mode; }

// ---------------------------------------------------------------- code patches
// Small in-place patches of the game's code, each verified against the bytes
// expected there before anything is written, restorable. Addresses are
// Ghidra-base (0x400000) virtual addresses, relocated at runtime.
struct CodePatch { const char* name; uintptr_t gva; uint8_t orig[8]; uint8_t patch[8]; int len; bool on; };
// 0, 1: the character distance cull. Ch_DrawMain (0x82ED50) calls
// Ch_VisibleTest (0x82E220) per actor: it measures the actor's distance to
// the camera (stored at actor+0x178) and, when that exceeds the actor's own
// cull distance (+0x174), sets bit 0x20 of actor+0x180 and returns 1, on
// which Ch_DrawMain hides the model (0x63C390(model, 0)). 0x82E32A is the
// "within range" branch (jne -> jmp: always within range); 0x82E378 is the
// same test on one map's special path (je -> nop).
CodePatch g_patches[] = {
    { "characters: no distance cull (0x82E32A)", 0x82E32A, { 0x75, 0x1C }, { 0xEB, 0x1C }, 2, false },
    { "characters: no distance cull, map special case (0x82E378)", 0x82E378, { 0x74, 0xB2 }, { 0x90, 0x90 }, 2, false },
    // 2: the character frustum cull. Ch_VisibleTest also calls 0x63B230(model),
    // which asks the renderer (0x6603C0, this = 0xCCB9D8) per model part whether
    // the part's scene node intersects the game camera's frustum (0x663CC0,
    // frustum at renderer->[0xDC]+0x74); no part in view -> bit 0x08 of
    // actor+0x180 -> hidden. 0x6603C0 returns "visible" without testing when
    // renderer+0x10D8C (a debug camera mode) is set: jne -> jmp makes it always do so.
    { "characters: no frustum cull (0x6603D1)", 0x6603D1, { 0x75, 0x1B }, { 0xEB, 0x1B }, 2, false },
    // 3: the scene frustum cull. 0x663CC0(node, frustum) is the node-vs-frustum
    // test used by the model parts above and by the scene renderer's two node
    // lists (0x652D35, 0x652EBC: geometry and effects). Entry replaced by
    // `mov al,1; ret 4`: everything registered in the scene is submitted.
    { "scene: no frustum cull at all (0x663CC0 returns visible)", 0x663CC0, { 0x55, 0x8B, 0xEC, 0x81, 0xEC }, { 0xB0, 0x01, 0xC2, 0x04, 0x00 }, 5, false },
    // 4: the bounding-volume test itself. 0x663A30(node, frustum) is what
    // 0x663CC0 ends in, and it is also called directly by the renderer's third
    // node list (0x6505D6, frustum of the camera at renderer+0x654), by a
    // visibility aggregate (0x6C8236/0x6C8294/0x6C83B0) and by another scene
    // traversal (0x9F3275). thiscall, ret 4. Entry -> `mov al,1; ret 4`:
    // every bounding test in the engine answers "in view".
    { "scene: bounding test always passes (0x663A30)", 0x663A30, { 0x55, 0x8B, 0xEC, 0x81, 0xEC }, { 0xB0, 0x01, 0xC2, 0x04, 0x00 }, 5, false },
};
constexpr int k_patch_dist_a = 0, k_patch_dist_b = 1, k_patch_chr_frustum = 2, k_patch_scene_frustum = 3, k_patch_bounds = 4;
constexpr uintptr_t k_exe_gva_base = 0x400000;   // Ghidra base of FFX.exe; the image is relocated at runtime
static uint8_t* gva_to_rt(uintptr_t gva) { const uint8_t* base = (const uint8_t*)GetModuleHandleA(nullptr); return base ? const_cast<uint8_t*>(base) + (gva - k_exe_gva_base) : nullptr; }
static int patch_apply(int i, bool on) {
    if (i < 0 || i >= (int)(sizeof(g_patches) / sizeof(g_patches[0]))) return -1;
    CodePatch& p = g_patches[i];
    if (p.on == on) return 0;
    uint8_t* at = gva_to_rt(p.gva); if (!at) return -2;
    const uint8_t* expect = on ? p.orig : p.patch;
    if (memcmp(at, expect, p.len) != 0) { diag("patch %s: bytes at %p are not the expected ones (%02X %02X %02X %02X %02X), not touched", p.name, (void*)at, at[0], at[1], at[2], at[3], at[4]); return -3; }
    DWORD old = 0;
    if (!VirtualProtect(at, p.len, PAGE_EXECUTE_READWRITE, &old)) return -4;
    memcpy(at, on ? p.patch : p.orig, p.len);
    VirtualProtect(at, p.len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, p.len);
    p.on = on;
    diag("patch %s: %s", p.name, on ? "ON" : "off");
    return 0;
}
extern "C" __declspec(dllexport) int ffxvr_patch_set(int i, int on) { return patch_apply(i, on != 0); }
// Character distance cull on/off as one switch (both patches).
// "Keep characters drawn": the distance cull (both sites) and the character
// frustum cull together. Applied as a set; a failure undoes the set.
static int patch_set_apply(const int* idx, int n, bool on) {
    for (int i = 0; i < n; ++i) {
        int rc = patch_apply(idx[i], on);
        if (rc != 0) { for (int j = 0; j < i; ++j) patch_apply(idx[j], !on); return rc; }
    }
    return 0;
}
static const int k_set_chr[] = { k_patch_dist_a, k_patch_dist_b, k_patch_chr_frustum };
extern "C" __declspec(dllexport) int ffxvr_cull_dist_set(int keep_all) { return patch_set_apply(k_set_chr, 3, keep_all != 0); }
extern "C" __declspec(dllexport) int ffxvr_cull_dist_get() { return (g_patches[k_patch_dist_a].on && g_patches[k_patch_dist_b].on && g_patches[k_patch_chr_frustum].on) ? 1 : 0; }
static const int k_set_scene[] = { k_patch_scene_frustum, k_patch_bounds };
extern "C" __declspec(dllexport) int ffxvr_cull_scene_set(int keep_all) { return patch_set_apply(k_set_scene, 2, keep_all != 0); }
extern "C" __declspec(dllexport) int ffxvr_cull_scene_get() { return (g_patches[k_patch_scene_frustum].on && g_patches[k_patch_bounds].on) ? 1 : 0; }
extern "C" __declspec(dllexport) int ffxvr_patch_count() { return (int)(sizeof(g_patches) / sizeof(g_patches[0])); }
extern "C" __declspec(dllexport) const char* ffxvr_patch_name(int i) { return (i >= 0 && i < ffxvr_patch_count()) ? g_patches[i].name : ""; }
extern "C" __declspec(dllexport) uint32_t ffxvr_backdrop_draws() { return g_bg_uploads_total; }
extern "C" __declspec(dllexport) int  ffxvr_xr_screen_now() { return g_xr_screen_now ? 1 : (g_screen3d_now ? 2 : 0); }
/// The stereo screen (see g_screen3d): on; depth (1 = the 1/30 rule); the screen-plane distance in game units (used
/// when auto is off); auto depth on/off; the pop-out limit as a fraction of the screen width.
extern "C" __declspec(dllexport) void ffxvr_screen3d(int on, float depth, float conv, int autodepth, float max_pop) {
    g_screen3d = on != 0; g_s3_depth = depth < 0.f ? 0.f : depth > 4.f ? 4.f : depth; g_s3_plane_set = conv < 10.f ? 10.f : conv;
    g_s3_auto = autodepth != 0; g_s3_max_pop = max_pop < 0.f ? 0.f : max_pop > 0.2f ? 0.2f : max_pop;
    if (!g_s3_auto) { /* fixed plane: still respect the background limit */ }
    if (!g_s3_auto) { g_screen3d_conv = g_s3_plane_set; g_screen3d_sep = g_s3_depth * g_s3_plane_set / 30.f; g_s3_conv_cur = g_screen3d_conv; g_s3_sep_cur = g_screen3d_sep; }
}
/// What the auto depth is doing: visible actors, nearest depth, subject depth, current plane and separation.
extern "C" __declspec(dllexport) void ffxvr_screen3d_status(int* vis, float* near_d, float* subject, float* plane, float* sep) {
    if (vis) *vis = g_s3_vis; if (near_d) *near_d = g_s3_near; if (subject) *subject = g_s3_subject; if (plane) *plane = g_screen3d_conv; if (sep) *sep = g_screen3d_sep;
}
extern "C" __declspec(dllexport) void ffxvr_xr_pitch_follow(float f) { g_xr_pitch_follow = f; }
bool g_xr_drop_image = false;   // Debug test (ffxvr_xr_debug): xr_tick drops the eye image, to exercise the hold (R15)
/// Debug test hooks (R8a/R15): drop_image 1 = every frame goes to the headset without an eye image (the last one is
/// held up to 30 frames, then black); sim_timeouts > 0 = the next N swapchain image waits report a timeout.
extern "C" __declspec(dllexport) void ffxvr_xr_debug(int drop_image, int sim_timeouts) {
    if ((drop_image != 0) != g_xr_drop_image) diag("xr debug: drop the eye image %s", drop_image ? "ON" : "off");
    g_xr_drop_image = drop_image != 0;
    if (sim_timeouts > 0) { diag("xr debug: simulating %d swapchain wait timeouts", sim_timeouts); xr_debug(sim_timeouts); }
}
// Low resolution (see lowres_check, with xr_tick): the game's backbuffer and the eye image size it was compared with.
static UINT g_lowres_bw = 0, g_lowres_bh = 0, g_lowres_ew = 0, g_lowres_eh = 0; static bool g_lowres = false;
/// Every exit from the headset (unticked, session lost, shutdown): the VR-only per-frame state goes back to flat
/// defaults, so the desktop view does not inherit a screen mode, a suspended override or a held grab.
static void leave_xr_state() {
    g_xr_want = false; g_xr_active = false;
    g_ovr_suspend = false; g_xr_screen_now = false; g_screen3d_now = false; g_backdrop_room = false;
    g_frames_without_cam = 0; g_blink_frames = 0; g_move_delta = 0.f; g_move_held = false;
    g_ui_state = UI_IDLE; g_ui_early = g_ui_early_keep = false; g_ui_last_frame_had_pass = false;   // robustness
    g_gest_active = false; g_vrpad_suppress = false; g_gest_grab[0] = g_gest_grab[1] = false;
    g_gest_chord_down = g_gest_reset_down = g_gest_nchord_down = false;
    g_fp_anchor_valid = false; g_seat_latched = false; g_fp_yaw_latched = false;
    g_lowres = false;   // no stale low-resolution line when the headset comes back on
    diag("xr: headset off, VR-only state reset");
}
extern "C" __declspec(dllexport) int ffxvr_xr_set(int on, float scale, int rotate, int translate, int use_fov, int seat) {
    g_xr_scale = scale; g_xr_rotate = rotate != 0; g_xr_translate = translate != 0; g_xr_use_fov = use_fov != 0; g_xr_seat = seat != 0;
    if (!on) { g_xr_fails = 0; g_xr_retry_at = 0; if (g_xr_want) { xr_stop(); leave_xr_state(); } return 0; }   // unticking resets the back-off
    if (g_xr_want) return 1;
    if (!g_cam_valid || g_backbuffer == nullptr) return 0;   // not until the game has rendered a 3D frame (the intro is not safe); waiting is not a failure
    const ULONGLONG now = GetTickCount64(); if (now < g_xr_retry_at) return 0;   // backing off: nothing loaded, nothing logged
    XrHost h = { g_device, g_ctx, g_dir, xr_diag, ffxvr_copy_pass };
    g_xr_quiet = g_xr_fails >= 2; g_xr_want = xr_start(h); g_xr_quiet = false;   // the first two failed attempts log in full
    if (g_xr_want) { if (g_xr_fails) diag("xr: started after %u failed attempts", g_xr_fails); g_xr_started_at = now; g_xr_retry_at = 0; g_xr_recenter_pending = true; return 1; }   // g_xr_fails clears in xr_tick after 5 s alive
    g_xr_fails++; g_xr_retry_at = now + (g_xr_fails < 3 ? 2000 : 5000);
    if (g_xr_fails == 2 || g_xr_fails % 60 == 0) diag("xr: start failed %u times (last: %s); retrying every %d s without logging", g_xr_fails, xr_status(), g_xr_fails < 3 ? 2 : 5);
    return 0;
}
extern "C" __declspec(dllexport) void ffxvr_xr_recenter() { g_xr_recenter_pending = true; g_seat_latched = false; g_fp_yaw_latched = false; }
extern "C" __declspec(dllexport) void ffxvr_cuts_set(int recentre, int blink, int follow_pans, float follow_rate, float dt) {
    g_cut_recentre = recentre != 0; g_cut_blink = blink != 0; g_follow_pans = follow_pans != 0; g_follow_rate = follow_rate;
    if (!g_dt_measured && dt > 0.f && dt < 1.f) g_dt = dt < k_dt_min ? k_dt_min : dt > k_dt_max ? k_dt_max : dt;   // fallback until begin_frame has measured one frame
}
extern "C" __declspec(dllexport) void ffxvr_turn_set(int stick, int mode, float snap_deg, float rate_deg, int blink) {
    g_turn_stick = stick != 0; g_turn_mode = mode; g_turn_snap = snap_deg; g_turn_rate = rate_deg; g_turn_blink = blink != 0;
}
extern "C" __declspec(dllexport) void ffxvr_turn(float deg) { turn_by(deg); }
extern "C" __declspec(dllexport) uint32_t ffxvr_turn_count() { return g_turn_count; }
extern "C" __declspec(dllexport) void ffxvr_perf_status(char* buf, int n) {
    if (buf == nullptr || n <= 0) return;   // a 0 size would hit the CRT's invalid-parameter handler (process abort)
    _snprintf_s(buf, (size_t)n, _TRUNCATE, "frame %.1f ms (max %.0f, gpu %.1f, xr wait %.1f end %.1f, eye2 %.1f) | twins %u, srvs %u, buffers %u | private %llu MB, vram %llu / %llu MB",
        g_perf.frame_ms, g_perf.frame_max_ms, g_perf.gpu_ms, g_perf.wait_ms, g_perf.end_ms, g_perf.e2_ms, g_perf.twins, g_perf.twin_srvs, g_perf.buffers,
        (unsigned long long)g_perf.private_mb, (unsigned long long)g_perf.vram_mb, (unsigned long long)g_perf.vram_budget_mb);
}
extern "C" __declspec(dllexport) void ffxvr_imgui_layer_set(int on, int force_show, int min_draws) { g_imgui_layer = on != 0; g_imgui_force = force_show != 0; g_imgui_min_draws = (uint32_t)(min_draws < 1 ? 1 : min_draws); }
extern "C" __declspec(dllexport) uint32_t ffxvr_imgui_draws() { return g_imgui_draws_last; }
extern "C" __declspec(dllexport) void ffxvr_msaa_set(int samples) { g_msaa_override = (samples == 2 || samples == 4) ? samples : 0; }
extern "C" __declspec(dllexport) void ffxvr_msaa_status(uint32_t* seen, uint32_t* changed, uint32_t* last_w, uint32_t* last_h, uint32_t* last_n) {
    if (seen) *seen = g_msaa_seen; if (changed) *changed = g_msaa_changed; if (last_w) *last_w = g_msaa_last_w; if (last_h) *last_h = g_msaa_last_h; if (last_n) *last_n = g_msaa_last_n;
}
extern "C" __declspec(dllexport) void ffxvr_move_set(int remap, float sign) { g_move_remap = remap != 0; g_move_sign = sign < 0 ? -1.f : 1.f; }
extern "C" __declspec(dllexport) void ffxvr_vrpad_set(int enabled) { g_vrpad_enabled = enabled != 0; }
extern "C" __declspec(dllexport) void ffxvr_gesture_set(int enabled, float tilt_gain) { g_gest_enabled = enabled != 0; g_gest_tilt_gain = tilt_gain; }
extern "C" __declspec(dllexport) void ffxvr_gesture_reset() { view_reset(); }
extern "C" __declspec(dllexport) void ffxvr_tp_set(float dist, float h, float pitch, float follow_rate) { g_tp_dist = dist; g_tp_h = h; g_tp_pitch = pitch; g_tp_follow_rate = follow_rate; }
/// The anchor smoothing time constants in ms (R4): field (diorama / third person) and battle centre; 0 = snap.
extern "C" __declspec(dllexport) void ffxvr_view_smooth_set(float field_ms, float battle_ms) {
    const float f = field_ms < 0.f ? 0.f : field_ms > 500.f ? 500.f : field_ms, b = battle_ms < 0.f ? 0.f : battle_ms > 3000.f ? 3000.f : battle_ms;
    if (!(f == f)) { g_fp_anchor_tau = 0.f; } else g_fp_anchor_tau = f * 0.001f;   // NaN -> snap
    if (!(b == b)) { g_anchor_ovr_tau = 0.f; } else g_anchor_ovr_tau = b * 0.001f;
}
extern "C" __declspec(dllexport) void ffxvr_floor_set(float units) { g_diorama_floor = units; }
/// Diorama anchor override (render space): the centre of the battle while one is on; valid = 0 hands it back to the player.
extern "C" __declspec(dllexport) void ffxvr_anchor_override(float x, float y, float z, int valid) { g_anchor_ovr[0] = x; g_anchor_ovr[1] = y; g_anchor_ovr[2] = z; g_anchor_ovr_valid = valid != 0; }
/// Chord events since the last call (cleared), plus the gesture state for the overlay.
extern "C" __declspec(dllexport) uint32_t ffxvr_gesture_poll(float* move3, float* scale_mul, int* active) {
    const uint32_t n = (g_gest_toggle_fp & 0xFF) | ((g_gest_next_chr & 0xFF) << 8); g_gest_toggle_fp = 0; g_gest_next_chr = 0;   // low byte: view toggles, next byte: next-character presses
    if (move3) { move3[0] = g_gest_move[0]; move3[1] = g_gest_move[1]; move3[2] = g_gest_move[2]; }
    if (scale_mul) *scale_mul = g_gest_scale; if (active) *active = g_gest_active ? 1 : 0;
    return n;
}
/// The controller state for the overlay: 2 hands x 12 floats (active, stick x, stick y, stick click, primary, secondary, trigger, squeeze, menu, grip valid, aim valid, 0), profiles as strings.
extern "C" __declspec(dllexport) int ffxvr_vrpad_status(float* out24, char* prof_l, char* prof_r, int n, uint32_t* frames) {
    if (frames) *frames = g_vrpad_frames;
    if (out24 == nullptr) return 0;
    for (int h = 0; h < 2; ++h) {
        const XrHand& hd = g_vrpad.hand[h]; float* o = out24 + h * 12;
        o[0] = hd.active ? 1.f : 0.f; o[1] = hd.stick[0]; o[2] = hd.stick[1]; o[3] = hd.stick_click ? 1.f : 0.f; o[4] = hd.primary ? 1.f : 0.f; o[5] = hd.secondary ? 1.f : 0.f;
        o[6] = hd.trigger; o[7] = hd.squeeze; o[8] = hd.menu ? 1.f : 0.f; o[9] = hd.grip_valid ? 1.f : 0.f; o[10] = hd.aim_valid ? 1.f : 0.f; o[11] = 0.f;
    }
    if (prof_l && n > 0) strncpy_s(prof_l, (size_t)n, g_vrpad.profile[0], _TRUNCATE);
    if (prof_r && n > 0) strncpy_s(prof_r, (size_t)n, g_vrpad.profile[1], _TRUNCATE);
    return g_vrpad.ready ? 1 : 0;
}
extern "C" __declspec(dllexport) void ffxvr_haptic(int hand, float amplitude, float seconds) { xr_haptic(hand, amplitude, seconds); }
extern "C" __declspec(dllexport) void ffxvr_move_status(float* yaw_view_deg, float* yaw_cam_deg, float* delta_deg, uint32_t* hook_calls, int* hooked) {
    if (yaw_view_deg) *yaw_view_deg = g_yaw_view * 57.29578f; if (yaw_cam_deg) *yaw_cam_deg = g_yaw_cam * 57.29578f;
    if (delta_deg) *delta_deg = g_move_delta * 57.29578f; if (hook_calls) *hook_calls = g_xi_hook_calls; if (hooked) *hooked = g_xi_hooked ? 1 : 0;
}
extern "C" __declspec(dllexport) int  ffxvr_cut_count() { return static_cast<int>(g_cut_count); }
extern "C" __declspec(dllexport) void ffxvr_fp_set(int mode, float eye_h, float follow_dist, float follow_h, int hide_body, float hide_radius) {
    if (mode != g_fp_mode) { g_fp_yaw_latched = false; g_seat_latched = false; g_xr_recenter_pending = true; }
    const int mode_before = g_fp_mode;
    g_fp_mode = mode; g_fp_eye_h = eye_h; g_fp_follow_dist = follow_dist; g_fp_follow_h = follow_h; g_fp_hide_body = hide_body != 0; g_fp_hide_radius = hide_radius;
    if (mode != mode_before) { view_defaults(mode); g_fp_anchor_valid = false; }   // the grab offset means something different per mode: start each mode from its default
}
extern "C" __declspec(dllexport) void ffxvr_fp_body_set(int mode, uint32_t head_mask, int solo) { g_fp_body_mode = mode; g_fp_head_mask = head_mask; g_fp_solo = solo; }
extern "C" __declspec(dllexport) uint32_t ffxvr_fp_parts() { return g_fp_parts_last; }
extern "C" __declspec(dllexport) void ffxvr_motion_log(int frames) { g_motion_log_left = frames; }
static void motion_log_tick() {
    if (g_motion_log_left <= 0) return;
    g_motion_log_left--;
    float best[3] = { 0, 0, 0 }; float bd = 1e30f;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (int i = 0; i < g_world_report_n; ++i) { const float dx = g_world_report[i][0] - g_fp_pos[0], dz = g_world_report[i][2] - g_fp_pos[2]; const float d = dx*dx + dz*dz; if (d < bd) { bd = d; memcpy(best, g_world_report[i], sizeof(best)); } }
    }
    float hy = 0.f;
    { const float fwd[3] = { 0, 0, -1 }; float d[3]; quat_rotate(g_xr_raw_q, fwd, d); hy = atan2f(-d[0], -d[2]); }
    char ex[240] = "";   // build 7 (6.4): managed's main-thread post-join snapshot of the followed actor, render space
    if (g_motion_cols_fresh) {
        const float* m = g_motion_cols;
        snprintf(ex, sizeof(ex), " | snap drawn (%.2f %.2f %.2f) present drawn (%.2f %.2f %.2f) snap logic (%.2f %.2f %.2f) exec %d snap age %d%s",
                 m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], (int)m[9], (int)m[10], m[11] != 0.f ? "" : " (no snapshot)");
        g_motion_cols_fresh = false;
    }
    diag("motion: frame %u dt %.1f ms actor (%.2f %.2f %.2f) rendered (%.2f %.2f %.2f) d %.1f | view yaw %.3f head yaw %.3f%s", g_frames, g_dt * 1000.f, g_fp_pos[0], g_fp_pos[1], g_fp_pos[2], best[0], best[1], best[2], sqrtf(bd), g_fp_yaw, hy, ex);
}

extern "C" __declspec(dllexport) void ffxvr_player_set(float x, float y, float z, int valid) {
    if (valid && g_fp_have_pos) {
        const float dx = x - g_fp_pos[0], dy = y - g_fp_pos[1], dz = z - g_fp_pos[2];
        const float d = sqrtf(dx*dx + dy*dy + dz*dz);
        if (d > 0.01f && d < 50.f) {                                                 // a logic step (a warp is not a step)
            g_fp_step = d; g_fp_still_frames = 0;
            const float h = sqrtf(dx*dx + dz*dz);
            if (h > 0.01f) { g_fp_move_dir[0] = dx / h; g_fp_move_dir[1] = 0.f; g_fp_move_dir[2] = dz / h; g_fp_move_dir_valid = true; }
        }
        else if (++g_fp_still_frames > 6) g_fp_step = 0.f;                          // stood still for a few frames
    }
    g_fp_pos[0] = x; g_fp_pos[1] = y; g_fp_pos[2] = z; g_fp_have_pos = valid != 0;
}
extern "C" __declspec(dllexport) void ffxvr_player_head_set(float x, float y, float z, int valid) {
    g_fp_have_head = valid != 0;
    if (!g_fp_have_head) { g_fp_head_s_valid = false; return; }
    // Stored and smoothed as the offset from the actor origin (player_set is called first).
    g_fp_head[0] = x - g_fp_pos[0]; g_fp_head[1] = y - g_fp_pos[1]; g_fp_head[2] = z - g_fp_pos[2];
    const float dx = g_fp_head[0] - g_fp_head_s[0], dy = g_fp_head[1] - g_fp_head_s[1], dz = g_fp_head[2] - g_fp_head_s[2];
    if (!g_fp_head_s_valid || dx*dx + dy*dy + dz*dz > 50.f * 50.f) { memcpy(g_fp_head_s, g_fp_head, sizeof(g_fp_head)); g_fp_head_s_valid = true; return; }   // first value, or a warp
    const float k = g_fp_head_smooth < 0.02f ? 0.02f : (g_fp_head_smooth > 1.f ? 1.f : g_fp_head_smooth);
    for (int i = 0; i < 3; ++i) g_fp_head_s[i] += (g_fp_head[i] - g_fp_head_s[i]) * k;
}
extern "C" __declspec(dllexport) void ffxvr_fp_head_set(int anchor, float up, float smooth, float fwd) { g_fp_head_anchor = anchor; g_fp_head_up = up; g_fp_head_smooth = smooth; g_fp_head_fwd = fwd; }
extern "C" __declspec(dllexport) int  ffxvr_fp_hidden() { return static_cast<int>(g_fp_hidden_draws); }
extern "C" __declspec(dllexport) void ffxvr_noimg_set(int mode) { g_noimg_mode = mode; }
extern "C" __declspec(dllexport) uint32_t ffxvr_noimg_draws() { return g_noimg_draws; }
extern "C" __declspec(dllexport) void ffxvr_bisect_set(int mode, int a, int b) { g_bisect_mode = mode; g_bisect_a = a; g_bisect_b = b; }
extern "C" __declspec(dllexport) void ffxvr_hist_set(int mode) { g_hist_mode = mode; }
extern "C" __declspec(dllexport) uint32_t ffxvr_hist_draws() { return g_hist_draws; }
/// Around a history-blend draw (both eyes): blending off, so the buffer holds this frame only.
extern "C" __declspec(dllexport) void ffxvr_blend_off() {
    if (g_ctx == nullptr || g_nb_active) return;
    if (g_opaque_blend == nullptr && g_device) { D3D11_BLEND_DESC bd = {}; bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL; g_device->CreateBlendState(&bd, &g_opaque_blend); }
    if (g_opaque_blend == nullptr) return;
    g_ctx->OMGetBlendState(&g_nb_saved, g_nb_saved_bf, &g_nb_saved_mask);
    g_ctx->OMSetBlendState(g_opaque_blend, g_nb_saved_bf, g_nb_saved_mask);
    g_nb_active = true;
}
extern "C" __declspec(dllexport) void ffxvr_blend_restore() {
    if (g_ctx == nullptr || !g_nb_active) return;
    g_ctx->OMSetBlendState(g_nb_saved, g_nb_saved_bf, g_nb_saved_mask);
    if (g_nb_saved) { g_nb_saved->Release(); g_nb_saved = nullptr; }
    g_nb_active = false;
}
extern "C" __declspec(dllexport) uint32_t ffxvr_scene_draws() { return g_scene_draws_last; }
extern "C" __declspec(dllexport) void ffxvr_ui_set(int on, float dist, float width) { g_ui_enabled = on != 0; g_ui_dist = dist; g_ui_width = width; }
extern "C" __declspec(dllexport) int  ffxvr_ui_draws() { return static_cast<int>(g_ui_draws_report); }
extern "C" __declspec(dllexport) void ffxvr_markers_3d(int on) { g_markers_3d = on != 0; }
/// Re-use the game's pointer sprites at the markers (needs the pointer hiding to capture them); returns sprites drawn since the last call.
extern "C" __declspec(dllexport) unsigned ffxvr_markers_game_art(int on) { g_ptr_reuse = on != 0; unsigned n = g_ptr_drawn; g_ptr_drawn = 0; return n; }
extern "C" __declspec(dllexport) void ffxvr_dump_seq(int n) { g_seq_left = n; g_seq_index = 0; g_e2_begin_fail_prev = g_e2_begin_fail; }
extern "C" __declspec(dllexport) void ffxvr_shutdown() {
    if (g_wd_stop) { SetEvent(g_wd_stop); if (g_wd_thread) WaitForSingleObject(g_wd_thread, 200); }   // build 7: no stall capture during exit
    if (g_xr_want) { xr_stop(); leave_xr_state(); }
}
/// The overlay's low-resolution line: 1 and the text while the headset is on and the game renders below its per-eye size.
extern "C" __declspec(dllexport) int ffxvr_lowres_status(char* buf, int n) {
    if (buf == nullptr || n <= 0) return 0;
    buf[0] = 0;
    if (!g_xr_want || !g_lowres) return 0;
    if (g_lowres_bw <= 1920 && g_lowres_bh <= 1080)   // the typical fallback: the monitor off, the game on Virtual Desktop's 1080p virtual display
        snprintf(buf, static_cast<size_t>(n), "Game renders at %ux%u, below the headset's per-eye %ux%u. Was the monitor off or asleep at launch (the game then only gets Virtual Desktop's 1080p display), or is the game's resolution set this low?",
                 g_lowres_bw, g_lowres_bh, g_lowres_ew, g_lowres_eh);
    else
        snprintf(buf, static_cast<size_t>(n), "Game renders at %ux%u, below the headset's per-eye %ux%u. For full sharpness raise the game's resolution (tools\\game-settings.ps1 -Width ... -Height ...) or lower Virtual Desktop's quality preset.",
                 g_lowres_bw, g_lowres_bh, g_lowres_ew, g_lowres_eh);
    return 1;
}
extern "C" __declspec(dllexport) void ffxvr_xr_status(char* buf, int n) {
    if (buf == nullptr || n <= 0) return;
    const float fwd[3] = { 0, 0, -1 }; float d[3]; quat_rotate(g_xr_q, fwd, d);
    if (!g_xr_want && (!g_cam_valid || g_backbuffer == nullptr)) { snprintf(buf, static_cast<size_t>(n), "waiting for the game to render a 3D frame before the headset can start"); return; }
    const ULONGLONG now = GetTickCount64();
    if (!g_xr_want && g_xr_retry_at > now) { snprintf(buf, static_cast<size_t>(n), "%s - retrying in %u s", xr_status(), (unsigned)((g_xr_retry_at - now + 999) / 1000)); return; }
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
static ID3D11Texture2D* ensure_copy(ID3D11Texture2D*& tex, ID3D11RenderTargetView*& rtv, const D3D11_TEXTURE2D_DESC& like) {
    if (tex) {
        D3D11_TEXTURE2D_DESC cd; tex->GetDesc(&cd);
        if (cd.Width != like.Width || cd.Height != like.Height || cd.Format != like.Format) { g_plain_srvs_forget(tex); if (rtv) rtv->Release(); rtv = nullptr; tex->Release(); tex = nullptr; }
    }
    if (tex == nullptr) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = like.Width; td.Height = like.Height; td.MipLevels = 1; td.ArraySize = 1; td.Format = like.Format;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(g_device->CreateTexture2D(&td, nullptr, &tex))) { tex = nullptr; return nullptr; }
        if (FAILED(g_device->CreateRenderTargetView(tex, nullptr, &rtv))) rtv = nullptr;
    }
    return tex;
}
/// Once per frame: a jump in the authored camera is a cut. Re-latch the seat
/// heading, recentre on the head, optionally blink.
static void detect_cut() {
    if (!g_cam_valid) return;
    float v[12]; { std::lock_guard<std::mutex> lock(g_mutex); memcpy(v, g_cam, sizeof(v)); }
    const float R[9] = { v[0], v[1], v[2],  v[4], v[5], v[6],  v[8], v[9], v[10] };
    const float t[3] = { v[3], v[7], v[11] };
    float pos[3]; for (int i = 0; i < 3; ++i) pos[i] = -(R[0*3+i]*t[0] + R[1*3+i]*t[1] + R[2*3+i]*t[2]);
    const float fwd[3] = { -R[6], -R[7], -R[8] };
    if (g_cam_prev_valid) {
        const float dp[3] = { pos[0]-g_cam_prev_pos[0], pos[1]-g_cam_prev_pos[1], pos[2]-g_cam_prev_pos[2] };
        const float dist = sqrtf(dp[0]*dp[0] + dp[1]*dp[1] + dp[2]*dp[2]);
        const float dot = fmaxf(-1.f, fminf(1.f, fwd[0]*g_cam_prev_fwd[0] + fwd[1]*g_cam_prev_fwd[1] + fwd[2]*g_cam_prev_fwd[2]));
        const float ang = acosf(dot) * 57.2958f;
        if ((dist > 40.f || ang > 25.f) && g_fp_mode == 0) {
            g_cut_count++;
            g_seat_latched = false;
            if (g_cut_recentre && !g_screen3d_now && !g_xr_screen_now) g_xr_recenter_pending = true;   // a recentre also moves the screen: not on screen cuts
            if (g_cut_blink) g_blink_frames = 6;
            if (g_cut_count <= 20) diag("cut %u: camera moved %.0f units, turned %.0f deg", g_cut_count, dist, ang);
        }
    }
    memcpy(g_cam_prev_pos, pos, sizeof(pos)); memcpy(g_cam_prev_fwd, fwd, sizeof(fwd)); g_cam_prev_valid = true;
}

// The game renders below the headset's per-eye size (the monitor off at launch: exclusive fullscreen only gets the
// active display's modes, and Virtual Desktop's virtual display offers 1080p). Checked once per frame with the
// headset on; logged once per change; shown by the overlay through ffxvr_lowres_status.
static void lowres_check() {
    if (g_backbuffer == nullptr || !xr_active()) { g_lowres = false; return; }
    D3D11_TEXTURE2D_DESC bd; g_backbuffer->GetDesc(&bd);
    unsigned ew = 0, eh = 0; xr_eye_size(&ew, &eh);
    if (ew == 0 || eh == 0) { g_lowres = false; return; }
    const bool low = bd.Width < ew || bd.Height < eh;
    if (bd.Width != g_lowres_bw || bd.Height != g_lowres_bh || ew != g_lowres_ew || eh != g_lowres_eh) {
        if (low || g_lowres) diag("resolution: the game renders at %ux%u, the headset's per-eye image is %ux%u: %s", bd.Width, bd.Height, ew, eh, low ? "BELOW (monitor off at launch, or the game's resolution set lower)" : "ok");
        g_lowres_bw = bd.Width; g_lowres_bh = bd.Height; g_lowres_ew = ew; g_lowres_eh = eh;
    }
    g_lowres = low;
}

static void xr_tick() {
    if (!g_xr_want) return;
    lowres_check();
    detect_cut();
    turn_tick();
    if (!g_stereo || !g_ovr_enabled) {           // the headset needs the override and both eyes, whatever the overlay says
        static bool said = false;
        if (!said) { said = true; diag("xr: forcing override + eye 2 on (were %d/%d)", g_ovr_enabled ? 1 : 0, g_stereo ? 1 : 0); }
        g_stereo = true; g_ovr_enabled = true;
    }
    // Eye 1: the backbuffer copied into a sampleable texture; eye 2: its twin.
    // When the frame had a UI pass, the eyes are the scene resolved before
    // it, and the UI goes on its own panel.
    ID3D11Texture2D* e1 = nullptr;
    ID3D11Texture2D* e2 = nullptr;
    XrUiPanel ui = { nullptr, g_ui_dist, g_ui_width, nullptr };
    const bool ui_frame = (g_ui_state == UI_DONE && g_ui_pass_frame + 1 == g_frames && g_eye1_final && g_eye2_final && g_ui_tex);   // g_frames was bumped at the top of this hook
    // The ImGui layer: what Fahrenheit drew after our hook last Present.
    g_imgui_draws_last = g_imgui_draws_frame; g_imgui_draws_frame = 0;
    if (g_imgui_layer && g_backbuffer) {
        D3D11_TEXTURE2D_DESC bd; g_backbuffer->GetDesc(&bd);
        ensure_copy(g_imgui_tex, g_imgui_rtv, bd);
        if (g_imgui_tex && g_imgui_draws_last > 0 && (g_imgui_force || g_imgui_draws_last >= g_imgui_min_draws)) ui.tex2 = g_imgui_tex;
    }
    g_ui_state = UI_IDLE; g_ui_last_frame_had_pass = ui_frame;
    g_ui_early = false; g_ui_early_keep = false; g_ui_early_report = g_ui_early_draws; g_ui_early_draws = 0;
    if (ui_frame) {
        e1 = g_eye1_final; e2 = g_eye2_final; ui.tex = g_ui_tex;
        if (g_markers_3d) draw_markers_both(g_eye1_final, g_eye1_final_rtv, g_eye2_final, g_eye2_final_rtv);
    }
    else if (g_backbuffer) {
        D3D11_TEXTURE2D_DESC bd; g_backbuffer->GetDesc(&bd);
        { auto it = g_twins.find(g_backbuffer); if (it != g_twins.end()) e2 = it->second.tex; }
        if (ensure_copy(g_bb_copy, g_bb_copy_rtv, bd)) { InEye2 guard; g_ctx->CopyResource(g_bb_copy, g_backbuffer); e1 = g_bb_copy; }
    }
    const float* P = &g_cam[16];
    const float tx = (P[0] != 0.f) ? 1.f / fabsf(P[0]) : 1.f;
    const float ty = (P[5] != 0.f) ? 1.f / fabsf(P[5]) : 0.5625f;
    // The field of view the frame just finished was rendered with.
    XrFovTan used[2];
    if (g_xr_fov_applied) { used[0] = g_xr_fov_used[0]; used[1] = g_xr_fov_used[1]; }
    else { used[0] = used[1] = XrFovTan{ -tx, tx, ty, -ty }; }
    XrFrameOut out = {};
    g_xr_fov_applied = false;
    auto clear_imgui = [&]() { if (g_imgui_rtv) { InEye2 guard; const float clear[4] = { 0.f, 0.f, 0.f, 0.f }; g_ctx->ClearRenderTargetView(g_imgui_rtv, clear); } };
    if (g_xr_drop_image) e1 = nullptr;              // Debug test: as if this frame had no eye image
    // R15: no early return without an eye image. xr_frame still runs (events, wait, begin, locate) and re-sends the
    // last frame for up to 30 frames; the session never stalls because one Present had nothing to show.
    if (g_screen3d_now) g_blink_frames = 0;         // the screen shows the game's cuts as cuts
    if (g_blink_frames > 0) {                       // a cut: a few black frames
        g_blink_frames--;
        if (g_bb_copy_rtv && e1 == g_bb_copy) { InEye2 guard; const float black[4] = { 0, 0, 0, 1 }; g_ctx->ClearRenderTargetView(g_bb_copy_rtv, black); e2 = g_bb_copy; }
    }
    if (e1 && g_seq_left > 0) {
        wchar_t n1[64], n2[64];
        _snwprintf_s(n1, _TRUNCATE, L"seq_%02d_eye1.bmp", g_seq_index); _snwprintf_s(n2, _TRUNCATE, L"seq_%02d_eye2.bmp", g_seq_index);
        dump_bmp(e1, n1); if (e2) dump_bmp(e2, n2);
        diag("seq %02d: frame %u, eye-2 draws %u, eye-2 begin failures %u, uploads so far %u", g_seq_index, g_frames, g_eye2_draws, g_e2_begin_fail - g_e2_begin_fail_prev, g_e2_uploads);
        g_e2_begin_fail_prev = g_e2_begin_fail;
        g_seq_index++; g_seq_left--;
    }
    // 2D frame? No real perspective camera was uploaded. Two frames of
    // hysteresis before switching to the screen; back to 3D at once.
    // The title screen and menus upload a dummy camera whose projection is
    // near-2D (|P[0]| <= 0.2), so only real cameras count. A draw-count
    // threshold of 20 was wrong: a hut interior is a 10-draw scene pass.
    const bool backdrop = (g_bg_uploads_frame > 0 && g_backdrop_mode == 1);
    const bool frame2d = (g_cam3d_uploads_frame == 0 || g_scene_live_draws < 3 || backdrop);
    if (frame2d) g_frames_without_cam++; else g_frames_without_cam = 0;
    const bool is2d = g_frames_without_cam >= 2;
    const bool screen_next = (g_xr_screen_mode == 1) || (g_xr_screen_mode == 0 && is2d);
    if (screen_next != g_xr_screen_now) {
        static int logged = 0;
        if (logged++ < 400) {
            D3D11_TEXTURE2D_DESC sd = {}; if (g_scene_live_tex) g_scene_live_tex->GetDesc(&sd);
            diag("mode -> %s at frame %u: stereo %d override %d, camera uploads %u (real %u, backdrop %u), scene draws %u into %ux%u fmt %u samples %u, eye-2 draws %u, begin failures %u",
                 screen_next ? "SCREEN" : "3D", g_frames, g_stereo ? 1 : 0, g_ovr_enabled ? 1 : 0, g_cam_uploads_frame, g_cam3d_uploads_frame, g_bg_uploads_frame, g_scene_live_draws, sd.Width, sd.Height,
                 static_cast<unsigned>(sd.Format), sd.SampleDesc.Count, g_eye2_draws, g_e2_begin_fail);
        }
    }
    g_backdrop_room = g_bg_uploads_frame > 0;
    g_cam_uploads_frame = 0; g_cam3d_uploads_frame = 0; g_bg_uploads_frame = 0;
    g_xr_screen_now = screen_next;
    g_ovr_suspend = screen_next;              // a frame shown flat is rendered as the game renders it: no head pose, no eye offset, no FOV
    {   // the stereo screen: 3D frames that would otherwise be shown in 3D (the frame just finished decides for the next)
        const bool s3 = g_screen3d && !screen_next && !g_backdrop_room;
        if (s3 != g_screen3d_now) diag("mode -> %s at frame %u (stereo screen: eye separation %.1f, screen plane at %.0f)", s3 ? "STEREO SCREEN" : "3D (headset view)", g_frames, g_screen3d_sep, g_screen3d_conv);
        g_screen3d_now = s3;
    }
    // The stick remap angle is refreshed by the override; whenever first person is not in effect it must be
    // zero, or a stale angle from outside keeps rotating the stick (a hut after a 150-degree doorway: "no control").
    if (g_ovr_suspend || g_backdrop_room || g_fp_mode == 0 || !g_xr_active) g_move_delta = 0.f;
    XrScreen screen = {};
    screen.on = g_xr_screen_now || g_screen3d_now; screen.width = g_xr_screen_width;
    screen.stereo = g_screen3d_now && e2 != nullptr; screen.eye2 = screen.stereo ? e2 : nullptr;
    memcpy(screen.q, g_xr_screen_q, sizeof(screen.q)); memcpy(screen.p, g_xr_screen_p, sizeof(screen.p));
    {
        // The runtime composites on the game's own device context inside
        // these calls (Virtual Desktop binds its 2496x2688 eye textures and
        // draws): none of that must be seen by our hooks, or its targets get
        // twinned, its draws re-issued and its texture mistaken for the
        // backbuffer -- which hung the GPU.
        InEye2 guard;
        LARGE_INTEGER ta, tb; QueryPerformanceCounter(&ta);
        xr_frame(e1, g_stereo ? e2 : nullptr, used, &screen, &ui, &out);
        QueryPerformanceCounter(&tb); if (g_qpf.QuadPart) g_xr_ms_this_frame = (tb.QuadPart - ta.QuadPart) * 1000.0 / g_qpf.QuadPart;
    }
    xr_apply_poses(out);
    motion_log_tick();
    clear_imgui();   // submitted above; the ImGui draws that follow this hook start clean
    xr_input_get(&g_vrpad); if (g_vrpad.ready) { g_vrpad_frames++; g_vrpad_packet++; }
    if (xr_take_space_change()) { g_xr_recenter_pending = true; g_seat_latched = false; g_fp_yaw_latched = false; diag("xr: recentre (headset gesture)"); }   // same as Home
    gesture_tick();
    // What the next frame renders with.
    if (g_xr_active && g_xr_use_fov) { g_xr_fov_used[0] = g_xr_fov[0]; g_xr_fov_used[1] = g_xr_fov[1]; }
    else { g_xr_fov_used[0] = g_xr_fov_used[1] = XrFovTan{ -tx, tx, ty, -ty }; }
    const ULONGLONG now_ms = GetTickCount64();
    if (!xr_active()) {   // the session went away (the runtime stopped it): back to flat
        leave_xr_state();
        if (now_ms - g_xr_started_at < k_xr_short_life_ms) {   // died at once: back off as for a failed start
            g_xr_fails++; g_xr_retry_at = now_ms + (g_xr_fails < 3 ? 2000 : 5000);
            if (g_xr_fails <= 2 || g_xr_fails % 60 == 0) diag("xr: session ended %.1f s after starting; counted as failed attempt %u", (now_ms - g_xr_started_at) / 1000.0, g_xr_fails);
        }
    }
    else if (g_xr_fails && now_ms - g_xr_started_at >= k_xr_short_life_ms) { g_xr_fails = 0; g_xr_retry_at = 0; }
}


/// Reads one hook counter by index, so the overlay can show whether a specific
/// draw path fires at all rather than only the aggregate.
extern "C" __declspec(dllexport) int ffxvr_counter(int i) {
    if (i < 0 || i >= C_COUNT) return -1;
    return static_cast<int>(g_counters[i]);
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
