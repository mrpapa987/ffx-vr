// SPDX-License-Identifier: MIT
//
// FFX VR - native helper.
//
// Stage one is pure diagnostics. The game uploads its view and projection
// matrices to the GPU through D3D11 constant buffers; if we can identify which
// buffer and which 64-byte slot holds them, we have the camera without needing
// any of PhyreEngine's internals.
//
// Method: patch ID3D11DeviceContext's vtable entries for Map and Unmap, keep the
// most recent contents of every constant buffer, and write them all to a file
// when F9 is pressed. Take one dump standing still, move, take another, and diff:
// the matrix that changed in a camera-like way is the one we want.
//
// Vtable patching rather than a hooking library keeps this dependency-free, and
// the vtable lives in d3d11.dll so patching it covers every context instance.

#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

#pragma comment(lib, "d3d11.lib")

namespace {

// ID3D11DeviceContext vtable indices.
//   0-2  IUnknown
//   3-6  ID3D11DeviceChild
//   7+   ID3D11DeviceContext
constexpr int VT_MAP   = 14;
constexpr int VT_UNMAP = 15;

using PFN_Map = HRESULT (STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
    D3D11_MAPPED_SUBRESOURCE*);

using PFN_Unmap = void (STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11Resource*, UINT);

PFN_Map   g_orig_map   = nullptr;
PFN_Unmap g_orig_unmap = nullptr;

struct CbRecord {
    UINT                 size    = 0;
    uint64_t             updates = 0;
    std::vector<uint8_t> data;
};

std::mutex                              g_mutex;
std::map<ID3D11Buffer*, CbRecord>       g_buffers;      // every constant buffer seen
std::map<void*, std::pair<ID3D11Buffer*, UINT>> g_in_flight;  // pData -> (buffer, size)

bool     g_installed  = false;
int      g_dump_index = 0;
bool     g_f9_was_down = false;
wchar_t  g_dll_dir[MAX_PATH] = {};

// Returns the buffer and its byte size if the resource is a constant buffer.
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
    buf->Release();   // QueryInterface addref'd; we only use the pointer as an identity key

    if ((desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) == 0) return false;

    *out_buf  = buf;
    *out_size = desc.ByteWidth;
    return true;
}

// Writes every recorded constant buffer to a numbered file beside this DLL.
// run-ffx.ps1 collects *.txt from the game folder automatically.
void dump_all() {
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\ffxvr_cbdump_%02d.txt", g_dll_dir, g_dump_index++);

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || f == nullptr) return;

    std::lock_guard<std::mutex> lock(g_mutex);

    fprintf(f, "FFX VR constant buffer dump #%d\n", g_dump_index - 1);
    fprintf(f, "distinct constant buffers seen: %zu\n", g_buffers.size());
    fprintf(f, "\nEach buffer is printed as 4-float rows. A 4x4 matrix occupies four\n");
    fprintf(f, "consecutive rows (64 bytes). Diff two dumps taken from different\n");
    fprintf(f, "camera positions to find the view matrix.\n");

    for (const auto& [buf, rec] : g_buffers) {
        fprintf(f, "\n================================================================\n");
        fprintf(f, "buffer %p   size %u bytes (%u floats)   updates %llu\n",
                static_cast<void*>(buf), rec.size, rec.size / 4,
                static_cast<unsigned long long>(rec.updates));
        fprintf(f, "----------------------------------------------------------------\n");

        const size_t floats = rec.data.size() / sizeof(float);
        const float* fp = reinterpret_cast<const float*>(rec.data.data());

        for (size_t i = 0; i < floats; i += 4) {
            fprintf(f, "  [%04zu] ", i * 4);   // byte offset
            for (size_t j = 0; j < 4 && (i + j) < floats; ++j) {
                fprintf(f, "%14.6f ", fp[i + j]);
            }
            if ((i % 16) == 12) fprintf(f, "   <- end of a 64-byte block");
            fprintf(f, "\n");
        }
    }

    fclose(f);
}

void poll_hotkey() {
    const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (down && !g_f9_was_down) dump_all();
    g_f9_was_down = down;
}

HRESULT STDMETHODCALLTYPE hk_Map(
    ID3D11DeviceContext*      ctx,
    ID3D11Resource*           res,
    UINT                      subresource,
    D3D11_MAP                 map_type,
    UINT                      flags,
    D3D11_MAPPED_SUBRESOURCE* mapped) {

    const HRESULT hr = g_orig_map(ctx, res, subresource, map_type, flags, mapped);
    if (FAILED(hr) || mapped == nullptr || mapped->pData == nullptr) return hr;

    ID3D11Buffer* buf  = nullptr;
    UINT          size = 0;
    if (as_constant_buffer(res, &buf, &size)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_in_flight[mapped->pData] = { buf, size };
    }

    return hr;
}

void STDMETHODCALLTYPE hk_Unmap(
    ID3D11DeviceContext* ctx,
    ID3D11Resource*      res,
    UINT                 subresource) {

    // Snapshot the data the game just wrote, before the pointer goes away.
    ID3D11Buffer* buf  = nullptr;
    UINT          size = 0;
    if (as_constant_buffer(res, &buf, &size)) {
        std::lock_guard<std::mutex> lock(g_mutex);

        for (auto it = g_in_flight.begin(); it != g_in_flight.end(); ++it) {
            if (it->second.first != buf) continue;

            CbRecord& rec = g_buffers[buf];
            rec.size    = size;
            rec.updates += 1;
            rec.data.resize(size);
            memcpy(rec.data.data(), it->first, size);

            g_in_flight.erase(it);
            break;
        }
    }

    g_orig_unmap(ctx, res, subresource);
    poll_hotkey();
}

bool patch_vtable_entry(void** vtable, int index, void* replacement, void** out_original) {
    DWORD old_protect = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old_protect))
        return false;

    *out_original  = vtable[index];
    vtable[index]  = replacement;

    VirtualProtect(&vtable[index], sizeof(void*), old_protect, &old_protect);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) int ffxvr_init() {
    if (g_installed) return 0;

    // A throwaway device is enough: the vtable lives in d3d11.dll and is shared
    // by every context, including the one the game already created.
    ID3D11Device*        device  = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL    level   = D3D_FEATURE_LEVEL_11_0;

    const HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &device, &level, &context);

    if (FAILED(hr) || context == nullptr) {
        if (device) device->Release();
        return -1;
    }

    void** vtable = *reinterpret_cast<void***>(context);

    const bool ok_map   = patch_vtable_entry(vtable, VT_MAP,   reinterpret_cast<void*>(&hk_Map),   reinterpret_cast<void**>(&g_orig_map));
    const bool ok_unmap = patch_vtable_entry(vtable, VT_UNMAP, reinterpret_cast<void*>(&hk_Unmap), reinterpret_cast<void**>(&g_orig_unmap));

    context->Release();
    device->Release();

    if (!ok_map || !ok_unmap) return -2;

    g_installed = true;
    return 0;
}

extern "C" __declspec(dllexport) int ffxvr_buffer_count() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return static_cast<int>(g_buffers.size());
}

extern "C" __declspec(dllexport) int ffxvr_dump_count() {
    return g_dump_index;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);

        // Remember our own directory so dumps land beside the DLL.
        GetModuleFileNameW(module, g_dll_dir, MAX_PATH);
        wchar_t* slash = wcsrchr(g_dll_dir, L'\\');
        if (slash) *slash = L'\0';
    }
    return TRUE;
}
