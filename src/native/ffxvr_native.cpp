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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

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
    float tmp[9], Rn[9];
    mul3(Ry, R, tmp);
    mul3(Rx, tmp, Rn);

    // Move along the new camera's axes (rows of R'; forward is -row 2).
    for (int i = 0; i < 3; ++i)
        Pn[i] = P[i] + Rn[0*3+i]*g_ovr_move[0] + Rn[1*3+i]*g_ovr_move[1] - Rn[2*3+i]*g_ovr_move[2];

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
    unsigned mask = 0;

    memcpy(p + o_view, Vn, 64);  mask |= 1;
    memcpy(p + o_pos,  Pn, 12);  mask |= 4;
    if (size >= o_inv + 64) { memcpy(p + o_inv, In, 64);  mask |= 2; }

    float PV[16], PVn[16];
    mul4(Pj, V, PV);
    mul4(Pj, Vn, PVn);
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
static int replace_known_products(uint8_t* p, UINT size) {
    if (!g_have_PV) return 0;
    int n = 0;
    const UINT nblocks = size / 16;

    // Pass 1: bare proj*view.
    for (UINT off = 0; off + 64 <= size; off += 16) {
        if (same4(reinterpret_cast<const float*>(p + off), g_last_PV)) {
            memcpy(p + off, g_last_PVn, 64);
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
                if (!used) { mul4(g_last_PVn, X, PVnX); used = true; }
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

extern "C" __declspec(dllexport) void ffxvr_cam_mem_set(int mode, uintptr_t obj) {
    g_cam_mem_mode = mode;
    g_cam_mem_obj  = obj;
}
extern "C" __declspec(dllexport) int ffxvr_cam_mem_writes() { return static_cast<int>(g_cam_mem_writes); }

/// If the override is on and this write carries a camera, returns a patched
/// private copy of the bytes to upload instead; otherwise returns src.
static const void* maybe_override(const void* src, UINT size) {
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
    }
    else {
        if (replace_known_products(g_scratch.data(), size) == 0) return src;   // nothing of ours in here
        mask = 256;
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
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_stacks_live) { g_stacks_live = false; census_dump(); }
        if (g_stacks_arm)  { g_stacks_arm = false; g_stacks_live = true; }
    }
    if (g_ovr_enabled && (g_cam_mem_mode & 2)) { std::lock_guard<std::mutex> lock(g_mutex); write_game_camera(); }
    g_latched      = false;
    g_draw_in_frame = 0;
    g_have_PV       = false;
    g_slot0_report = g_slot0_seen;   // snapshot before the reset, so the UI sees it
    g_slot0_seen   = 0;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
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
extern "C" __declspec(dllexport) void ffxvr_on_draw(int which) {
    if (which >= 1 && which <= 4) bump(static_cast<CounterId>(which));
    const int idx = g_draw_in_frame++;
    if (g_latched || g_ctx == nullptr) return;

    ID3D11Buffer* b = nullptr;
    g_ctx->VSGetConstantBuffers(0, 1, &b);          // AddRefs
    if (b == nullptr) return;

    D3D11_BUFFER_DESC bd = {};
    b->GetDesc(&bd);
    // The camera lives in the ~1 KB per-object buffers (view at +64, inverse at
    // +320). The ~7 KB ones are skinned characters with a different layout.
    if (bd.ByteWidth < CAM_MIN_SIZE || bd.ByteWidth > 2048) { b->Release(); return; }

    if (g_slot0 != nullptr) g_slot0->Release();
    g_slot0             = b;                        // keep the reference
    g_latched           = true;
    g_draws_latched_at  = idx;
}

/// out[0] = floats changed since last frame, out[1] = buffer size, out[2] = draw
/// index that latched, out[3..10] = first changed byte offsets.
/// Called from the managed inline detour on UpdateSubresource, which the
/// runtime would not let us vtable-patch. Records (and scans) the write when
/// the target is a constant buffer being replaced whole.
extern "C" __declspec(dllexport) const void* ffxvr_on_updatesub(void* resource, const void* box, const void* src) {
    bump(C_UPDATESUB);
    if (resource == nullptr || src == nullptr || box != nullptr) return src;

    ID3D11Buffer* buf = nullptr; UINT size = 0;
    if (!as_constant_buffer(reinterpret_cast<ID3D11Resource*>(resource), &buf, &size)) return src;

    std::lock_guard<std::mutex> lock(g_mutex);
    record_buffer(buf, size, src, "UpdateSubresource");   // records the game's own camera
    return maybe_override(src, size);                       // uploads ours
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
