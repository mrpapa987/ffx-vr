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
    g_latched      = false;
    g_draw_in_frame = 0;
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
extern "C" __declspec(dllexport) void ffxvr_on_updatesub(void* resource, const void* box, const void* src) {
    bump(C_UPDATESUB);
    if (resource == nullptr || src == nullptr || box != nullptr) return;

    ID3D11Buffer* buf = nullptr; UINT size = 0;
    if (as_constant_buffer(reinterpret_cast<ID3D11Resource*>(resource), &buf, &size)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        record_buffer(buf, size, src, "UpdateSubresource");
    }
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
