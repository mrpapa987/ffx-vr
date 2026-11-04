// SPDX-License-Identifier: MIT
// ffxvr_cpu.h -- cpu-lean (4 Nov 2026): pure helpers for the per-frame CPU timings (the cpu: line) and for range_readable's
// region cache. No Windows, no D3D, no logging: ffxvr_native.cpp feeds them, a unit test (scratch) drives them with a fake
// memory map. The page rule here is range_readable's, unchanged; ffxvr_native.cpp static_asserts the constants against
// <windows.h>.
#pragma once
#include <cstddef>
#include <cstdint>

namespace cpu {

/// One quantity over the cpu: line's window (from one perf: line to the next): sum, max, count. Negative or NaN = 0.
struct Acc {
    double sum = 0.0; float mx = 0.f; uint32_t n = 0;
    void add(double v) { if (!(v >= 0.0)) v = 0.0; sum += v; if (v > mx) mx = static_cast<float>(v); n++; }
    double avg() const { return n ? sum / n : 0.0; }
};

// -- the page rule (range_readable's) ---------------------------------------------------------------------------------
constexpr uint32_t k_mem_commit = 0x1000, k_mem_image = 0x1000000;
constexpr uint32_t k_page_noaccess = 0x01, k_page_readonly = 0x02, k_page_readwrite = 0x04, k_page_writecopy = 0x08,
                   k_page_execute_read = 0x20, k_page_execute_readwrite = 0x40, k_page_execute_writecopy = 0x80, k_page_guard = 0x100;

/// One VirtualQuery answer (ok = false: the call failed).
struct Mbi { bool ok = false; uintptr_t base = 0, size = 0, alloc_base = 0; uint32_t state = 0, protect = 0, type = 0; };

/// Committed, not a guard or no-access page, and one of the readable protections (range_readable's test, verbatim).
inline bool mbi_readable(const Mbi& m) {
    if (!m.ok || m.state != k_mem_commit) return false;
    if (m.protect & (k_page_guard | k_page_noaccess)) return false;
    const uint32_t p = m.protect & 0xFF;
    return p == k_page_readonly || p == k_page_readwrite || p == k_page_execute_read || p == k_page_execute_readwrite
        || p == k_page_writecopy || p == k_page_execute_writecopy;
}

/// range_readable's region cache: the last N page runs VirtualQuery reported readable that lie in FFX.exe's own image (type
/// MEM_IMAGE, allocation base = the exe's module base). Render thread only, cleared at every Present (ffxvr_begin_frame); an
/// entry can outlive a frame (reads before that call, a long ring-wait stall), which is harmless: the safety is the filter,
/// not the clear. Only the exe's image is kept because it is the one kind of region that cannot be freed
/// mid-frame: the exe is never unloaded and its sections are never decommitted; the protection changes the mod and Fahrenheit
/// make there (code patches) go between readable protections. Heap, mapped and other modules' regions (the game's magic_*.dll
/// unload mid-session) are never cached: those keep the query on every read, as before.
template <int N>
struct RegionCache {
    uintptr_t lo[N] = {}, hi[N] = {}; int next = 0;
    void clear() { for (int i = 0; i < N; ++i) lo[i] = hi[i] = 0; next = 0; }
    /// [a, a + len) inside one cached run.
    bool covers(uintptr_t a, size_t len) const {
        const uintptr_t e = a + len;
        if (e < a) return false;
        for (int i = 0; i < N; ++i) if (hi[i] > lo[i] && a >= lo[i] && e <= hi[i]) return true;
        return false;
    }
    void add(uintptr_t l, uintptr_t h) {
        if (h <= l) return;
        for (int i = 0; i < N; ++i) if (lo[i] == l && hi[i] == h) return;
        lo[next] = l; hi[next] = h; next = (next + 1) % N;
    }
};

struct RangeStats { uint32_t queries = 0, hits = 0; };

/// range_readable's walk: every page of [a, a + len) committed and readable. query(addr) is VirtualQuery. With a cache
/// (render thread), a range inside a cached run is answered without a query, and every readable run of the exe's image the
/// walk sees is added. Without one it is the original loop exactly.
template <class Query, int N>
bool range_check(uintptr_t a, size_t len, Query&& query, RegionCache<N>* cache, uintptr_t image_base, RangeStats* st) {
    if (a == 0 || len == 0 || a < 0x10000 || a + len < a) return false;
    if (cache && cache->covers(a, len)) { if (st) st->hits++; return true; }
    uintptr_t q = a;
    while (q < a + len) {
        const Mbi m = query(q);
        if (st) st->queries++;
        if (!mbi_readable(m)) return false;
        const uintptr_t end = m.base + m.size;
        if (end <= q) return false;   // (a broken answer would loop forever; VirtualQuery never gives one)
        if (cache && image_base != 0 && m.type == k_mem_image && m.alloc_base == image_base) cache->add(m.base, end);
        q = end;
    }
    return true;
}

}  // namespace cpu
