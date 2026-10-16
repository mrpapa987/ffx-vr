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
    VT_UPDATESUBRESOURCE    = 48,
};

enum CounterId {
    C_VSSETCB = 0, C_DRAWINDEXED, C_DRAW, C_MAP, C_UNMAP, C_UPDATESUB,
    C_CREATEBUF, C_CB_CAPTURED, C_COUNT
};

const char* const kCounterName[C_COUNT] = {
    "VSSetConstantBuffers", "DrawIndexed", "Draw", "Map", "Unmap",
    "UpdateSubresource", "CreateBuffer (const, w/ data)", "constant buffer captures"
};

volatile LONG g_counters[C_COUNT] = {};

inline void bump(CounterId id) { InterlockedIncrement(&g_counters[id]); }

// ---------------------------------------------------------------- originals

using PFN_VSSetConstantBuffers = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
using PFN_DrawIndexed          = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using PFN_Draw                 = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using PFN_Map                  = HRESULT (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using PFN_Unmap                = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using PFN_UpdateSubresource    = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
using PFN_CreateBuffer         = HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);

PFN_VSSetConstantBuffers g_orig_vssetcb        = nullptr;
PFN_DrawIndexed          g_orig_drawindexed    = nullptr;
PFN_Draw                 g_orig_draw           = nullptr;
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
uint32_t           g_frame_writes  = 0;       // writes seen in the frame just finished

// Which buffers actually get bound to the vertex shader during a captured
// frame. The game binds far more often than it writes, so this census shows
// whether the bound buffers are ones we ever saw filled.
std::map<ID3D11Buffer*, int> g_bound;

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

void record_buffer(ID3D11Buffer* buf, UINT size, const void* src, const char* via) {
    if (size == 0 || size > 65536 || src == nullptr) return;

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
void heartbeat(bool force) {
    const DWORD now = GetTickCount();
    if (!force && (now - g_last_beat) < 5000) return;
    g_last_beat = now;

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
    fprintf(f, "writes recorded: %zu (limit %zu) - one full frame\n\n", g_capture.size(), kCaptureLimit);
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

        // Only print writes big enough to hold a matrix; skip the noise.
        if (n < 16) continue;

        fprintf(f, "\n--- seq %u   buffer %p   %u bytes   via %s ---\n",
                w.seq, static_cast<void*>(w.buf), w.size, w.via);

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

bool patch(void** vtable, int index, void* replacement, void** out_original) {
    DWORD old = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old)) return false;
    *out_original = vtable[index];
    vtable[index] = replacement;
    VirtualProtect(&vtable[index], sizeof(void*), old, &old);
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

    void** vt = *reinterpret_cast<void***>(context);
    if (vt == nullptr || IsBadReadPtr(vt, sizeof(void*) * 64)) return -3;

    bool ok = true;
    ok &= patch(vt, VT_VSSETCONSTANTBUFFERS, reinterpret_cast<void*>(&hk_VSSetConstantBuffers), reinterpret_cast<void**>(&g_orig_vssetcb));
    ok &= patch(vt, VT_DRAWINDEXED,          reinterpret_cast<void*>(&hk_DrawIndexed),          reinterpret_cast<void**>(&g_orig_drawindexed));
    ok &= patch(vt, VT_DRAW,                 reinterpret_cast<void*>(&hk_Draw),                 reinterpret_cast<void**>(&g_orig_draw));
    ok &= patch(vt, VT_MAP,                  reinterpret_cast<void*>(&hk_Map),                  reinterpret_cast<void**>(&g_orig_map));
    ok &= patch(vt, VT_UNMAP,                reinterpret_cast<void*>(&hk_Unmap),                reinterpret_cast<void**>(&g_orig_unmap));
    ok &= patch(vt, VT_UPDATESUBRESOURCE,    reinterpret_cast<void*>(&hk_UpdateSubresource),    reinterpret_cast<void**>(&g_orig_updatesub));

    // Patch CreateBuffer on the device: ID3D11Device vtable index 3.
    if (game_device != nullptr) {
        ID3D11Device* dev = reinterpret_cast<ID3D11Device*>(game_device);
        void** dvt = *reinterpret_cast<void***>(dev);
        if (dvt != nullptr && !IsBadReadPtr(dvt, sizeof(void*) * 8)) {
            patch(dvt, 3, reinterpret_cast<void*>(&hk_CreateBuffer),
                  reinterpret_cast<void**>(&g_orig_createbuffer));
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

        if (g_capturing) {               // a full frame has now elapsed
            g_capturing   = false;
            dump_now      = true;
            g_frame_writes = static_cast<uint32_t>(g_capture.size());
        }
        else if (g_arm_next) {
            g_arm_next  = false;
            g_capture.clear();
            g_bound.clear();
            g_seq       = 0;
            g_capturing = true;
        }
    }
    if (dump_now) dump_capture();
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
