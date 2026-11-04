// SPDX-License-Identifier: MIT
// ffxvr_cpu.h -- cpu-lean (4 Nov 2026): pure helpers for the per-frame CPU timings (the cpu: line) and for range_readable's
// region cache. No Windows, no D3D, no logging: ffxvr_native.cpp feeds them, a unit test (scratch) drives them with a fake
// memory map. The page rule here is range_readable's, unchanged; ffxvr_native.cpp static_asserts the constants against
// <windows.h>. vq-lean (4 Nov 2026): ffxvr_safe_read's per-frame run cache (RunCache, range_check_fast).
#pragma once
#include <cmath>
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

// -- vq-lean: ffxvr_safe_read's per-frame run cache (ini fast_reads, default 1) ---------------------------------------
// VirtualQuery scans the page table from the queried page to the end of the run of pages with the same attributes, so its
// cost grows with the run: about 1 us in small regions, 100-160 us in the game's large committed blocks (the owner's battle
// windows: 70+ queries, 6-9 ms a frame). These keep every run a query answered readable for the rest of the frame (any type:
// heap, mapped, a module, the exe). Only ffxvr_safe_read consults them, and it copies out of a run that is not FFX.exe's own
// image with ReadProcessMemory: the kernel's copy fails instead of faulting if the run was freed, decommitted or made
// no-access since its query, so a stale entry can make a read fail but never fault. range_readable (whose callers read with
// plain loads) never answers from here. Render thread only; cleared at every Present, after a failed copy, and after 50 ms.

/// The runs [lo, hi) VirtualQuery answered readable this frame; img: inside FFX.exe's image (allocation base = the exe's
/// module base, type MEM_IMAGE), which is never unloaded or decommitted, so a plain copy stays safe there as before.
template <int N>
struct RunCache {
    uintptr_t lo[N] = {}, hi[N] = {}; bool img[N] = {}; int next = 0;
    void clear() { for (int i = 0; i < N; ++i) { lo[i] = hi[i] = 0; img[i] = false; } next = 0; }
    /// The entry holding address q, or -1.
    int find(uintptr_t q) const {
        for (int i = 0; i < N; ++i) if (hi[i] > lo[i] && q >= lo[i] && q < hi[i]) return i;
        return -1;
    }
    /// Keeps [l, h): nothing if an entry already holds it (only its image flag can drop, the safe way); entries it holds are
    /// dropped (a lower query of the same run returns a longer answer with the same end); else a free slot, else the oldest
    /// (round robin).
    void add(uintptr_t l, uintptr_t h, bool im) {
        if (h <= l) return;
        for (int i = 0; i < N; ++i) if (hi[i] > lo[i] && lo[i] <= l && h <= hi[i]) { img[i] = img[i] && im; return; }
        int slot = -1;
        for (int i = 0; i < N; ++i)
            if (hi[i] > lo[i] && l <= lo[i] && hi[i] <= h) { lo[i] = hi[i] = 0; img[i] = false; if (slot < 0) slot = i; }
        if (slot < 0) for (int i = 0; i < N; ++i) if (hi[i] <= lo[i]) { slot = i; break; }
        if (slot < 0) { slot = next; next = (next + 1) % N; }
        lo[slot] = l; hi[slot] = h; img[slot] = im;
    }
};

/// The allocations whose first query this frame started above their base: VirtualQuery's answer starts at the queried page,
/// so reads lower in the same block (an object list walked downwards, a random order) would each query again. The block's
/// [base, lo) is known to belong to that allocation (lo: where the kept answer starts), so the next miss there queries once
/// from the base instead, at most once per allocation per frame.
template <int P>
struct AllocSeen {
    uintptr_t base[P] = {}, lo[P] = {}; uint8_t state[P] = {}; int next = 0;   // state 1: noted; 2: its base already queried
    void clear() { for (int i = 0; i < P; ++i) { base[i] = lo[i] = 0; state[i] = 0; } next = 0; }
    /// A query at lo (> ab) answered readable in the allocation at ab.
    void note(uintptr_t ab, uintptr_t l) {
        if (ab == 0 || ab >= l) return;
        for (int i = 0; i < P; ++i) if (state[i] != 0 && base[i] == ab) return;
        base[next] = ab; lo[next] = l; state[next] = 1; next = (next + 1) % P;
    }
    /// A miss at q in [base, lo) of a noted allocation not probed yet: true and its base (the caller queries there).
    bool take_probe(uintptr_t q, uintptr_t& ab) {
        for (int i = 0; i < P; ++i)
            if (state[i] == 1 && q >= base[i] && q < lo[i]) { state[i] = 2; ab = base[i]; return true; }
        return false;
    }
};

struct FastStats { uint32_t queries = 0, hits = 0, probes = 0; };   // queries include the probes; hits: ranges answered with no query
enum : int { k_fast_no = 0, k_fast_image = 1, k_fast_other = 2 };

/// ffxvr_safe_read's walk with fast_reads: the same page rule as range_check (every page of [a, a + len) committed and
/// readable when last queried), each address answered from a run kept this frame or by a query whose readable answer is kept.
/// Returns k_fast_no (not readable), k_fast_image (every byte in runs of FFX.exe's image: a plain copy is safe) or
/// k_fast_other (the copy must be the kernel's).
template <class Query, int N, int P>
int range_check_fast(uintptr_t a, size_t len, Query&& query, RunCache<N>& c, AllocSeen<P>& seen, uintptr_t image_base, FastStats* st) {
    if (a == 0 || len == 0 || a < 0x10000 || a + len < a) return k_fast_no;
    const uintptr_t e = a + len;
    bool all_img = true, queried = false;
    uintptr_t q = a;
    while (q < e) {
        const int i = c.find(q);
        if (i >= 0) { all_img = all_img && c.img[i]; q = c.hi[i]; continue; }
        queried = true;
        uintptr_t ab = 0;
        if (seen.take_probe(q, ab)) {   // below a kept run of a noted allocation: one query from its base first
            const Mbi b = query(ab);
            if (st) { st->queries++; st->probes++; }
            const uintptr_t be = b.base + b.size;
            if (mbi_readable(b) && b.base == ab && be > ab) {
                const bool bim = image_base != 0 && b.type == k_mem_image && b.alloc_base == image_base;
                c.add(b.base, be, bim);
                if (q < be) { all_img = all_img && bim; q = be; continue; }   // its run holds q: answered
            }
        }
        const Mbi m = query(q);
        if (st) st->queries++;
        if (!mbi_readable(m)) return k_fast_no;
        const uintptr_t end = m.base + m.size;
        if (end <= q || m.base > q) return k_fast_no;   // (a broken answer; VirtualQuery never gives one)
        const bool im = image_base != 0 && m.type == k_mem_image && m.alloc_base == image_base;
        c.add(m.base, end, im);
        all_img = all_img && im;
        if (!im) seen.note(m.alloc_base, m.base);
        q = end;
    }
    if (!queried && st) st->hits++;
    return all_img ? k_fast_image : k_fast_other;
}

/// ffxvr_safe_read with fast_reads on (render thread): the kept runs, their age bound, the copy chosen by the walk's answer.
/// plain(): the copy as before (memcpy under __try), used only when every byte lies in the exe's image; kernel(): the
/// kernel's copy (ReadProcessMemory), true when every byte was copied. A failed kernel copy means the map changed under a
/// kept run: everything kept is dropped, so the next reads query afresh.
template <int N, int P>
struct FastReader {
    RunCache<N> runs; AllocSeen<P> seen; FastStats st;
    int64_t t0 = 0; bool armed = false;   // when the kept runs were started (the first read after a clear)
    uint32_t copies = 0, fails = 0;       // kernel copies, of them failed (this frame; the caller rolls them)
    void clear() { runs.clear(); seen.clear(); armed = false; }
    /// 1: copied; 0: not readable or the copy failed. now / max_age in the caller's clock (QPC ticks).
    template <class Query, class Plain, class Kernel>
    int read(uintptr_t a, size_t len, int64_t now, int64_t max_age, uintptr_t image_base, Query&& query, Plain&& plain, Kernel&& kernel) {
        if (!armed || now - t0 > max_age || now < t0) { clear(); armed = true; t0 = now; }
        const int k = range_check_fast(a, len, query, runs, seen, image_base, &st);
        if (k == k_fast_no) return 0;
        if (k == k_fast_image) return plain() ? 1 : 0;
        copies++;
        if (kernel()) return 1;
        fails++;
        clear();
        return 0;
    }
};

// -- grid-timer: the game's main loop (SKS_WinMainLoop 0x62EE50) over one cpu: line window --------------------------------
/// The limiter's period for one frame in ms: the game's divider ([0xC30E88], 1 or 2) x K ([0xB412D8], 1 / 59.94 s; FFX-60fps
/// writes it for its rate). 0 = not a frame period (the expected interval is then unknown and nothing counts as late).
inline double limiter_period_ms(int32_t divider, double k_s) {
    if (divider < 1 || divider > 8 || !(k_s > 1.0 / 400.0) || !(k_s < 1.0 / 20.0)) return 0.0;
    return static_cast<double>(divider) * k_s * 1000.0;
}

/// The main thread's frames and the limiter's Sleep calls between them, summed over one window. Filled on the game's main
/// thread (under the native side's lock), swapped out by the render thread with the cpu: line. Pure arithmetic.
struct Limiter {
    static constexpr double k_gap_ms = 250.0;   // a longer interval is a load or a stall, not pacing: counted, kept out of the stats
    static constexpr double k_slot_ms = 8.333;  // one 120 Hz headset slot
    uint32_t frames = 0;                        // frame starts with an interval (the first after a gap or the probe's start has none)
    uint32_t gaps = 0;                          // intervals over k_gap_ms
    uint32_t known = 0, late1 = 0, late2 = 0, late_slot = 0, early2 = 0;   // intervals with a known period; late by > 1, > 2 ms, > a slot; early by > 2 ms
    uint32_t no_sleep = 0;                      // frames that started without a Sleep since the previous frame's end (already due)
    double iv_sum = 0.0, iv_sq = 0.0, want_sum = 0.0; float iv_max = 0.f, iv_min = 0.f;
    Acc work;                                   // 0x62E340's run time (the frame on the main thread, its waits included)
    uint32_t sleeps = 0, sleep0 = 0, over1 = 0, over2 = 0;   // Sleep calls, of them Sleep(0); took more than asked + 1 / + 2 ms (asked > 0)
    double ask_sum = 0.0, took_sum = 0.0, over_sum = 0.0; float took_max = 0.f, over_max = 0.f; uint32_t ask_max = 0;

    /// A frame starts: iv_ms since the previous start (< 0: none), want_ms the period the limiter waited for (0: unknown),
    /// slept: a Sleep ran since the previous frame ended.
    void frame_start(double iv_ms, double want_ms, bool slept) {
        if (!slept) no_sleep++;
        if (!(iv_ms >= 0.0)) return;
        if (iv_ms > k_gap_ms) { gaps++; return; }
        const float f = static_cast<float>(iv_ms);
        if (frames == 0 || f < iv_min) iv_min = f;
        if (f > iv_max) iv_max = f;
        frames++; iv_sum += iv_ms; iv_sq += iv_ms * iv_ms;
        if (want_ms > 0.0) {
            known++; want_sum += want_ms;
            const double d = iv_ms - want_ms;
            if (d > 1.0) late1++;
            if (d > 2.0) late2++;
            if (d > k_slot_ms) late_slot++;
            if (d < -2.0) early2++;
        }
    }
    void frame_end(double work_ms) { work.add(work_ms); }
    /// One of the limiter's Sleep calls: asked (the game's argument), took (measured around the call).
    void sleep(uint32_t ask_ms, double took_ms) {
        if (!(took_ms >= 0.0)) took_ms = 0.0;
        sleeps++;
        if (ask_ms == 0) sleep0++;
        if (ask_ms > ask_max) ask_max = ask_ms;
        ask_sum += ask_ms; took_sum += took_ms;
        if (took_ms > took_max) took_max = static_cast<float>(took_ms);
        if (ask_ms > 0) {
            const double o = took_ms - static_cast<double>(ask_ms);
            over_sum += o > 0.0 ? o : 0.0;
            if (o > over_max) over_max = static_cast<float>(o);
            if (o > 1.0) over1++;
            if (o > 2.0) over2++;
        }
    }
    double iv_avg() const { return frames ? iv_sum / frames : 0.0; }
    double iv_sd() const {
        if (frames < 2) return 0.0;
        const double m = iv_sum / frames, v = iv_sq / frames - m * m;
        return v > 0.0 ? std::sqrt(v) : 0.0;
    }
    double want_avg() const { return known ? want_sum / known : 0.0; }
    /// Average oversleep over the calls that asked for at least 1 ms.
    double over_avg() const { const uint32_t n = sleeps - sleep0; return n ? over_sum / n : 0.0; }
};

}  // namespace cpu
