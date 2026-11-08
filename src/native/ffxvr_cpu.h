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
    /// lean2: every byte of [a, a + len) in kept runs (len > 0, no wrap).
    bool covers(uintptr_t a, size_t len) const {
        const uintptr_t e = a + len;
        if (len == 0 || e < a) return false;
        for (uintptr_t q = a; q < e;) { const int i = find(q); if (i < 0) return false; q = hi[i]; }
        return true;
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

// -- lean2 (4 Nov 2026): the working-set check (QueryWorkingSetEx) and the big-run hints ----------------------------------
// The run cache starts cold at every Present, so a read into one of the game's big committed blocks still costs one long
// scan a frame however stable the block is (the owner's virtual screen, 17:38: 4 queries a frame, 0.9 ms, ~225 us each).
// QueryWorkingSetEx answers per page (one page-table look-up each), not per run, so its cost does not grow with the block:
// Valid = 1 means the page is resident (a hardware-valid PTE), so a read of it cannot fault, and a guard page (an invalid PTE
// until its first touch) never reports Valid; Win32Protection is then the page's protection, held to range_readable's rule.
// A page that is not resident (paged out, never touched, guard, reserved, free) reports Valid = 0, which means "unknown": the
// walk asks VirtualQuery as before. So the check answers only "readable now" or "ask VirtualQuery"; it never refuses a read
// that VirtualQuery would allow, and it allows only what VirtualQuery would allow at that moment.
// BigHints: the address ranges where VirtualQuery's answers were long (>= 1 MB from the queried page), which get the
// working-set check first. A hint is a cost guess only, kept across frames and never trusted for safety: a stale one costs one
// extra call. (A cached run kept across frames, checked by one VirtualQuery of the allocation's first page, is no cheaper: that
// query scans the whole first run, which in a uniform block is the whole block; see the notes, lean2.)
constexpr uintptr_t k_page = 0x1000;
constexpr int k_ws_max_pages = 32;            // a longer read asks VirtualQuery (the actor table, 104,448 B, spans 26-27 pages)
constexpr uintptr_t k_big_run = 0x100000;     // a VirtualQuery answer this long (from the queried page) leaves a hint

/// One page's working-set attributes (the low 32 bits of PSAPI_WORKING_SET_EX_BLOCK, the same in 32- and 64-bit code): bit 0
/// Valid, bits 4..14 Win32Protection, bit 31 Bad. Resident, not bad, and range_readable's protection rule.
inline bool ws_readable(uint32_t f) {
    if ((f & 1u) == 0 || (f & 0x80000000u) != 0) return false;
    Mbi m; m.ok = true; m.state = k_mem_commit; m.protect = (f >> 4) & 0x7FFu;
    return mbi_readable(m);
}

/// What one ws_check did (review N5, SF1): answered = a working-set query was made and answered; any_valid = it reported at
/// least one page Valid.
struct WsInfo { bool answered = false, any_valid = false; };

/// [a, a + len): every page resident and readable now, by one working-set query ws(va, attrs, n) (QueryWorkingSetEx; true when
/// it answered, attrs[i] the i-th page's attributes). false = unknown: the caller asks VirtualQuery. The argument checks are
/// range_check's.
template <class Ws>
bool ws_check(uintptr_t a, size_t len, Ws&& ws, WsInfo* info = nullptr) {
    if (info) *info = WsInfo{};
    if (a == 0 || len == 0 || a < 0x10000 || a + len < a) return false;
    const uintptr_t p0 = a & ~(k_page - 1), pl = (a + len - 1) & ~(k_page - 1);
    const uintptr_t n = (pl - p0) / k_page + 1;
    if (n > static_cast<uintptr_t>(k_ws_max_pages)) return false;
    uintptr_t va[k_ws_max_pages]; uint32_t at[k_ws_max_pages];
    for (uintptr_t i = 0; i < n; ++i) { va[i] = p0 + i * k_page; at[i] = 0; }
    if (!ws(static_cast<const uintptr_t*>(va), static_cast<uint32_t*>(at), static_cast<int>(n))) return false;
    bool all = true, any = false;
    for (uintptr_t i = 0; i < n; ++i) { if (at[i] & 1u) any = true; if (!ws_readable(at[i])) all = false; }
    if (info) { info->answered = true; info->any_valid = any; }
    return all;
}

/// lean2 review SF1: the start-up self-test's verdict. Before any read relies on it, QueryWorkingSetEx is asked about a page
/// of our own: (1) committed PAGE_READWRITE and written (resident): it must answer Valid with PAGE_READWRITE; (2) the same
/// page made PAGE_READWRITE | PAGE_GUARD (never read): it must answer Valid = 0 or carry the guard bit. ok1 / ok2: the
/// calls answered; f1 / f2 the page's attributes. k_wst_pass, or why the check stays off for the session.
enum : int { k_wst_pass = 0, k_wst_call1, k_wst_not_resident, k_wst_prot, k_wst_call2, k_wst_guard_valid };
inline int ws_selftest_verdict(bool ok1, uint32_t f1, bool ok2, uint32_t f2) {
    if (!ok1) return k_wst_call1;
    if ((f1 & 1u) == 0) return k_wst_not_resident;
    if (((f1 >> 4) & 0x7FFu) != k_page_readwrite || !ws_readable(f1)) return k_wst_prot;
    if (!ok2) return k_wst_call2;
    if ((f2 & 1u) != 0 && ((f2 >> 4) & k_page_guard) == 0) return k_wst_guard_valid;
    return k_wst_pass;
}
/// fix-render review S3: two more self-test steps. (3) The test page made PAGE_NOACCESS must answer Valid = 0 or protection
/// PAGE_NOACCESS (else the whole check is off); (4) the exe's header page, after a read, must answer resident and readable (an
/// image page: else only the kept image runs are off, the heap's check stays).
enum : int { k_wst_call3 = 16, k_wst_noaccess_valid, k_wst_img_call, k_wst_img_not_readable };
inline int ws_selftest_noaccess(bool ok3, uint32_t f3) {
    if (!ok3) return k_wst_call3;
    if ((f3 & 1u) != 0 && ((f3 >> 4) & 0x7FFu) != k_page_noaccess) return k_wst_noaccess_valid;
    return k_wst_pass;
}
inline int ws_selftest_image(bool ok4, uint32_t f4) {
    if (!ok4) return k_wst_img_call;
    return ws_readable(f4) ? static_cast<int>(k_wst_pass) : static_cast<int>(k_wst_img_not_readable);
}

inline const char* ws_selftest_why(int v) {
    switch (v) {
    case k_wst_pass: return "passed";
    case k_wst_call1: return "the call failed on a resident page";
    case k_wst_not_resident: return "a written page reported not Valid";
    case k_wst_prot: return "a written PAGE_READWRITE page reported another protection";
    case k_wst_call2: return "the call failed on a guard page";
    case k_wst_guard_valid: return "a guard page reported Valid without the guard bit";
    case k_wst_call3: return "the call failed on a no-access page";
    case k_wst_noaccess_valid: return "a no-access page reported Valid with another protection";
    case k_wst_img_call: return "the call failed on the exe's header page";
    case k_wst_img_not_readable: return "the exe's header page (read just before) did not report resident and readable";
    default: return "?";
    }
}
/// lean2 review SF1 (the Wine case: TRUE with Valid = 0 for every page): this many answers in a row with no page Valid, each
/// for a range VirtualQuery then found readable, and the reader reports the check as useless (FastReader::ws_tripped).
constexpr uint32_t k_ws_zero_max = 64;

/// The ranges [lo, hi) where VirtualQuery answered with a long run (a cost hint, not a claim about the memory).
template <int H>
struct BigHints {
    uintptr_t lo[H] = {}, hi[H] = {}; int next = 0;
    void clear() { for (int i = 0; i < H; ++i) lo[i] = hi[i] = 0; next = 0; }
    bool has(uintptr_t q) const {
        for (int i = 0; i < H; ++i) if (hi[i] > lo[i] && q >= lo[i] && q < hi[i]) return true;
        return false;
    }
    int count() const { int n = 0; for (int i = 0; i < H; ++i) n += hi[i] > lo[i]; return n; }
    /// Nothing if an entry holds [l, h); an entry it holds grows to it; else a free slot, else the oldest (round robin).
    void add(uintptr_t l, uintptr_t h) {
        if (h <= l) return;
        for (int i = 0; i < H; ++i) if (hi[i] > lo[i] && lo[i] <= l && h <= hi[i]) return;
        for (int i = 0; i < H; ++i) if (hi[i] > lo[i] && l <= lo[i] && hi[i] <= h) { lo[i] = l; hi[i] = h; return; }
        for (int i = 0; i < H; ++i) if (hi[i] <= lo[i]) { lo[i] = l; hi[i] = h; return; }
        lo[next] = l; hi[next] = h; next = (next + 1) % H;
    }
};

/// lean2: the guarded reads that cost something (a query or a working-set check), by caller (the return address into the
/// caller of ffxvr_safe_read / range_readable): the cpu: line names the top ones. Diagnostic only, render thread.
struct SiteStat { uintptr_t ret = 0; uint32_t calls = 0, queries = 0, ws = 0; int64_t ticks = 0; uint32_t len = 0; uintptr_t addr = 0, alloc = 0, run = 0; };
template <int S>
struct SiteTable {
    SiteStat s[S]; SiteStat other;   // other: the calls of sites that found the table full
    void clear() { for (int i = 0; i < S; ++i) s[i] = SiteStat{}; other = SiteStat{}; }
    /// One call: q queries, w working-set checks, t ticks; the read [addr, +len); alloc / run: the last query's allocation base
    /// and run length (0: no query in this call: the previous values stay).
    void note(uintptr_t ret, uint32_t q, uint32_t w, int64_t t, uint32_t len, uintptr_t addr, uintptr_t alloc, uintptr_t run) {
        SiteStat* e = nullptr;
        for (int i = 0; i < S && e == nullptr; ++i) if (s[i].calls != 0 && s[i].ret == ret) e = &s[i];
        for (int i = 0; i < S && e == nullptr; ++i) if (s[i].calls == 0) { e = &s[i]; e->ret = ret; }
        if (e == nullptr) e = &other;
        e->calls++; e->queries += q; e->ws += w; e->ticks += t; e->len = len; e->addr = addr;
        if (run != 0) { e->alloc = alloc; e->run = run; }
    }
    /// Up to k entries by ticks, largest first; returns how many (the indices into s).
    int top(int* idx, int k) const {
        if (k <= 0) return 0;
        int n = 0;
        for (int i = 0; i < S; ++i) {
            if (s[i].calls == 0) continue;
            if (n >= k && s[i].ticks <= s[idx[k - 1]].ticks) continue;   // not larger than the smallest kept
            int j = n < k ? n++ : k - 1;                                  // a new place, or the smallest's
            while (j > 0 && s[idx[j - 1]].ticks < s[i].ticks) { idx[j] = idx[j - 1]; --j; }
            idx[j] = i;
        }
        return n;
    }
};

// -- fix-render (7 Nov 2026): FFX.exe's image runs kept across frames, each use proven by the working-set check ---------------
// The owner's s2 session paid ~0.4 ms a frame on two 4-byte reads into a 25 MB run of FFX.exe's image (0x0116B994 and
// 0x0117ED3C, one through range_readable, one through ffxvr_safe_read): both caches end with every Present, and each keeps the
// run from the QUERIED page (VirtualQuery's answer starts there), so the lower read missed the higher one's run and every
// frame scanned the 25 MB twice. ImageKeep holds the image runs any query answered (render thread, across frames) and
// learns a run from its section's start where it can (one probe per section, the PE section table); a run kept from an
// earlier frame is never trusted as it stands: a read in it is answered only when QueryWorkingSetEx says every page of the read
// is resident and readable NOW (lean2's ws rule: Valid, not bad, range_readable's protections; nothing uses it before its
// self-test passed), which a page freed, decommitted, made no-access or turned into a guard page since cannot pass. The pages
// proven are kept for the rest of the frame (the same lifetime as a query's answer), and the pages read through kept runs are
// remembered (hot) so the next frame proves them all in one call. Anything else (no working-set answer, a page not resident,
// a run dropped) takes the walk as before, which queries afresh. Off (fast_reads=0, the check off): never consulted.
constexpr int k_keep_hot = 32;     // pages proven in one call (k_ws_max_pages)
template <int K = 8, int S = 16>
struct ImageKeep {
    uintptr_t lo[K] = {}, hi[K] = {}; int next = 0;            // image runs answered readable (any frame)
    uintptr_t hot[k_keep_hot] = {}; int hot_n = 0, hot_next = 0; // pages read through kept runs (proven together at the next frame's first use)
    uintptr_t ok[k_keep_hot] = {}; int ok_n = 0, ok_next = 0;    // pages proven resident and readable this frame
    bool batched = false;                                        // this frame's first proof (with the hot pages) done
    uintptr_t sec_lo[S] = {}, sec_hi[S] = {}; bool sec_probed[S] = {}; int sec_n = 0;   // the exe's sections (page-aligned), and which were probed
    uint32_t proofs = 0, proof_pages = 0, answered = 0, refused = 0, probes = 0;       // statistics (the caller rolls them): refused = calls not answered
    uint32_t pages_no = 0;                                                             // review S3: pages a proof found not resident / not readable
    /// Everything but the sections (a failed copy, fast_reads or the check off): the next reads query afresh.
    void clear() { for (int i = 0; i < K; ++i) lo[i] = hi[i] = 0; next = 0; hot_n = hot_next = 0; new_frame(); for (int i = 0; i < S; ++i) sec_probed[i] = false; }
    /// A new frame (or the per-frame runs' age bound): the proofs expire with the frame's runs.
    void new_frame() { ok_n = ok_next = 0; batched = false; }
    void set_sections(const uintptr_t* l, const uintptr_t* h, int n) {
        sec_n = 0;
        for (int i = 0; i < n && sec_n < S; ++i) if (h[i] > l[i]) { sec_lo[sec_n] = l[i] & ~(k_page - 1); sec_hi[sec_n] = h[i]; sec_probed[sec_n] = false; sec_n++; }
    }
    int count() const { int n = 0; for (int i = 0; i < K; ++i) n += hi[i] > lo[i]; return n; }
    /// [a, a + len) inside one kept run (len > 0, no wrap).
    bool in_run(uintptr_t a, size_t len) const {
        const uintptr_t e = a + len;
        if (len == 0 || e < a) return false;
        for (int i = 0; i < K; ++i) if (hi[i] > lo[i] && a >= lo[i] && e <= hi[i]) return true;
        return false;
    }
    /// Keeps [l, h): nothing if a run holds it; runs it holds are dropped (a lower query of the same run: the longer answer);
    /// else a free slot, else the oldest.
    void add_run(uintptr_t l, uintptr_t h) {
        if (h <= l) return;
        for (int i = 0; i < K; ++i) if (hi[i] > lo[i] && lo[i] <= l && h <= hi[i]) return;
        int slot = -1;
        for (int i = 0; i < K; ++i) if (hi[i] > lo[i] && l <= lo[i] && hi[i] <= h) { lo[i] = hi[i] = 0; if (slot < 0) slot = i; }
        if (slot < 0) for (int i = 0; i < K; ++i) if (hi[i] <= lo[i]) { slot = i; break; }
        if (slot < 0) { slot = next; next = (next + 1) % K; }
        lo[slot] = l; hi[slot] = h;
    }
    /// Drops the runs holding q (a proof failed there: the next read queries).
    void drop_at(uintptr_t q) { for (int i = 0; i < K; ++i) if (hi[i] > lo[i] && q >= lo[i] && q < hi[i]) lo[i] = hi[i] = 0; }
    bool is_ok(uintptr_t pg) const { for (int i = 0; i < ok_n; ++i) if (ok[i] == pg) return true; return false; }
    bool ok_covers(uintptr_t a, size_t len) const {
        if (len == 0 || a + len < a) return false;
        for (uintptr_t pg = a & ~(k_page - 1); pg < a + len; pg += k_page) if (!is_ok(pg)) return false;
        return true;
    }
    void ok_add(uintptr_t pg) { if (is_ok(pg)) return; if (ok_n < k_keep_hot) ok[ok_n++] = pg; else { ok[ok_next] = pg; ok_next = (ok_next + 1) % k_keep_hot; } }
    void hot_add(uintptr_t pg) {
        for (int i = 0; i < hot_n; ++i) if (hot[i] == pg) return;
        if (hot_n < k_keep_hot) hot[hot_n++] = pg; else { hot[hot_next] = pg; hot_next = (hot_next + 1) % k_keep_hot; }
    }
    void hot_remove(uintptr_t pg) { for (int i = 0; i < hot_n; ++i) if (hot[i] == pg) { hot[i] = hot[--hot_n]; if (hot_next > hot_n) hot_next = 0; return; } }
    /// One working-set query for the read's pages (and, at the frame's first proof, the hot pages too): every page that answers
    /// resident and readable is kept as proven for the frame. True when every page of the read did.
    template <class Ws>
    bool prove(uintptr_t a, size_t len, Ws&& ws) {
        if (len == 0 || a < 0x10000 || a + len < a) return false;
        const uintptr_t p0 = a & ~(k_page - 1), pl = (a + len - 1) & ~(k_page - 1);
        const uintptr_t n_read = (pl - p0) / k_page + 1;
        if (n_read > static_cast<uintptr_t>(k_ws_max_pages)) return false;
        uintptr_t va[k_ws_max_pages]; uint32_t at[k_ws_max_pages]; int n = 0;
        for (uintptr_t i = 0; i < n_read; ++i) va[n++] = p0 + i * k_page;
        if (!batched) for (int i = 0; i < hot_n && n < k_ws_max_pages; ++i) if (hot[i] < p0 || hot[i] > pl) va[n++] = hot[i];
        for (int i = 0; i < n; ++i) at[i] = 0;
        proofs++; proof_pages += static_cast<uint32_t>(n);
        if (!ws(static_cast<const uintptr_t*>(va), static_cast<uint32_t*>(at), n)) { refused++; return false; }
        answered++;
        const bool was_batched = batched; batched = true;
        bool all = true;
        for (int i = 0; i < n; ++i) {
            const bool yes = ws_readable(at[i]);
            if (!yes) pages_no++;
            if (yes) ok_add(va[i]); else if (i >= static_cast<int>(n_read)) hot_remove(va[i]);
            if (i < static_cast<int>(n_read) && !yes) all = false;
        }
        (void)was_batched;
        if (all) for (uintptr_t i = 0; i < n_read; ++i) hot_add(p0 + i * k_page);
        return all;
    }
    /// A miss at q (in the exe's image): its section's page-aligned start, once per section until the next clear, when it lies
    /// below q's page (a query from there learns the run from its start, if the run starts there).
    bool take_probe(uintptr_t q, uintptr_t& start) {
        for (int i = 0; i < sec_n; ++i)
            if (q >= sec_lo[i] && q < sec_hi[i]) {
                if (sec_probed[i] || sec_lo[i] >= (q & ~(k_page - 1))) return false;
                sec_probed[i] = true; start = sec_lo[i]; probes++; return true;
            }
        return false;
    }
};

/// range_readable's walk on the render thread with fast_reads on (fix-render): range_check with the kept image runs. A read
/// inside a run kept from any frame is answered when the working-set check proves its pages (or they were proven this frame);
/// a failed proof drops the run and walks. The walk is range_check's, except that a miss inside the exe's image
/// [image_base, image_end) may first query its section's start (once a section), and every image run it sees is kept in both
/// caches. Same answers as range_check whenever the working-set answers are true.
template <class Query, class Ws, int N, int K, int S>
bool range_check_keep(uintptr_t a, size_t len, Query&& query, Ws&& ws, RegionCache<N>& cache, ImageKeep<K, S>& keep,
                      uintptr_t image_base, uintptr_t image_end, RangeStats* st) {
    if (a == 0 || len == 0 || a < 0x10000 || a + len < a) return false;
    if (cache.covers(a, len)) { if (st) st->hits++; return true; }
    if (image_base != 0 && keep.in_run(a, len)) {
        if (keep.ok_covers(a, len) || keep.prove(a, len, ws)) { if (st) st->hits++; return true; }
        keep.drop_at(a);
    }
    uintptr_t q = a;
    while (q < a + len) {
        uintptr_t s0 = 0;
        if (image_base != 0 && q >= image_base && q < image_end && keep.take_probe(q, s0)) {
            const Mbi b = query(s0);
            if (st) st->queries++;
            const uintptr_t be = b.base + b.size;
            if (mbi_readable(b) && b.base == s0 && be > q && b.type == k_mem_image && b.alloc_base == image_base) {
                cache.add(b.base, be); keep.add_run(b.base, be); q = be; continue;
            }
        }
        const Mbi m = query(q);
        if (st) st->queries++;
        if (!mbi_readable(m)) return false;
        const uintptr_t end = m.base + m.size;
        if (end <= q) return false;
        if (image_base != 0 && m.type == k_mem_image && m.alloc_base == image_base) { cache.add(m.base, end); keep.add_run(m.base, end); }
        q = end;
    }
    return true;
}

struct FastStats { uint32_t queries = 0, hits = 0, probes = 0, ws = 0, ws_yes = 0; };   // queries include the probes; hits: ranges answered with no query; ws: working-set checks, ws_yes: reads they answered
enum : int { k_fast_no = 0, k_fast_image = 1, k_fast_other = 2 };

/// ffxvr_safe_read's walk with fast_reads: the same page rule as range_check (every page of [a, a + len) committed and
/// readable when last queried), each address answered from a run kept this frame or by a query whose readable answer is kept.
/// Returns k_fast_no (not readable), k_fast_image (every byte in runs of FFX.exe's image: a plain copy is safe) or
/// k_fast_other (the copy must be the kernel's).
/// lean2: with hints, every long answer outside the exe's image (>= k_big_run from the queried page) is noted there as
/// [allocation base, end of the run) (a cost hint only; the answer itself is used exactly as before).
template <class Query, int N, int P, int H = 8, int K = 8, int S = 16>
int range_check_fast(uintptr_t a, size_t len, Query&& query, RunCache<N>& c, AllocSeen<P>& seen, uintptr_t image_base, FastStats* st, BigHints<H>* hints = nullptr,
                     ImageKeep<K, S>* keep = nullptr, uintptr_t image_end = 0) {
    if (a == 0 || len == 0 || a < 0x10000 || a + len < a) return k_fast_no;
    const uintptr_t e = a + len;
    bool all_img = true, queried = false;
    uintptr_t q = a;
    while (q < e) {
        const int i = c.find(q);
        if (i >= 0) { all_img = all_img && c.img[i]; q = c.hi[i]; continue; }
        queried = true;
        uintptr_t ab = 0, s0 = 0;
        if (keep && image_base != 0 && q >= image_base && q < image_end && keep->take_probe(q, s0)) {   // fix-render: the run from its section's start
            const Mbi b = query(s0);
            if (st) st->queries++;
            const uintptr_t be = b.base + b.size;
            if (mbi_readable(b) && b.base == s0 && be > q && b.type == k_mem_image && b.alloc_base == image_base) {
                c.add(b.base, be, true); keep->add_run(b.base, be); q = be; continue;
            }
        }
        if (seen.take_probe(q, ab)) {   // below a kept run of a noted allocation: one query from its base first
            const Mbi b = query(ab);
            if (st) { st->queries++; st->probes++; }
            const uintptr_t be = b.base + b.size;
            if (mbi_readable(b) && b.base == ab && be > ab) {
                const bool bim = image_base != 0 && b.type == k_mem_image && b.alloc_base == image_base;
                c.add(b.base, be, bim);
                if (bim && keep) keep->add_run(b.base, be);
                if (hints && !bim && be - b.base >= k_big_run) hints->add(b.base, be);
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
        if (im && keep) keep->add_run(m.base, end);   // fix-render: kept across frames (each later use proven by the working-set check)
        all_img = all_img && im;
        if (!im) seen.note(m.alloc_base, m.base);
        if (hints && !im && end - m.base >= k_big_run) hints->add(m.alloc_base != 0 && m.alloc_base <= m.base ? m.alloc_base : m.base, end);
        q = end;
    }
    if (!queried && st) st->hits++;
    return all_img ? k_fast_image : k_fast_other;
}

/// ffxvr_safe_read with fast_reads on (render thread): the kept runs, their age bound, the copy chosen by the walk's answer.
/// plain(): the copy as before (memcpy under __try), used only when every byte lies in the exe's image; kernel(): the
/// kernel's copy (ReadProcessMemory), true when every byte was copied. A failed kernel copy means the map changed under a
/// kept run: everything kept is dropped, so the next reads query afresh.
/// lean2: hints (kept across frames, cost only): a read that starts in a hinted range, touches nothing of the exe's image
/// [image_base, image_end) and is not all in kept runs gets the working-set check first; resident and readable, its pages are
/// kept for the rest of the frame (never as image: the copy out of them is always the kernel's) and the kernel copies it.
/// Otherwise (unknown, or no hint) the walk is the one above, unchanged.
template <int N, int P, int H = 8>
struct FastReader {
    RunCache<N> runs; AllocSeen<P> seen; FastStats st; BigHints<H> hints;
    int64_t t0 = 0; bool armed = false;   // when the kept runs were started (the first read after a clear)
    uint32_t copies = 0, fails = 0;       // kernel copies, of them failed (this frame; the caller rolls them)
    uint32_t ws_zero_run = 0; bool ws_tripped = false;   // review SF1: answers in a row with no page Valid where VirtualQuery found the range readable
    ImageKeep<8, 16>* keep = nullptr;     // fix-render: the exe's image runs kept across frames (null: vq-lean / lean2 exactly)
    uint32_t keep_hits = 0;               // reads answered through it (this frame; the caller rolls them)
    uint32_t keep_fails = 0;              // fails when the keep was last cleared: a failed kernel copy clears it before the next read
    /// The caller's per-frame roll, before it zeroes `fails`: a failure no read has seen yet clears the keep now.
    void keep_roll() { if (keep && fails != keep_fails) keep->clear(); keep_fails = 0; }
    void clear() { runs.clear(); seen.clear(); armed = false; }   // (not the hints: they are no claim about the memory)
    /// 1: copied; 0: not readable or the copy failed. now / max_age in the caller's clock (QPC ticks). ws: the working-set
    /// query (ws_check's); one that always returns false is the vq-lean reader exactly.
    template <class Query, class Ws, class Plain, class Kernel>
    int read(uintptr_t a, size_t len, int64_t now, int64_t max_age, uintptr_t image_base, uintptr_t image_end, Query&& query, Ws&& ws, Plain&& plain, Kernel&& kernel) {
        if (!armed || now - t0 > max_age || now < t0) { if (keep && armed) keep->new_frame(); clear(); armed = true; t0 = now; }   // (fix-render: the keep's proofs end with the runs' age bound; the caller ends them every frame)
        if (keep && fails != keep_fails) { keep->clear(); keep_fails = fails; }   // fix-render: the map changed under a kept run: start over
        if (keep && image_base != 0 && a >= image_base && a + len <= image_end && keep->in_run(a, len) && !runs.covers(a, len)) {
            if (keep->ok_covers(a, len) || keep->prove(a, len, ws)) {   // fix-render: resident and readable now, in the exe's image
                runs.add(a & ~(k_page - 1), ((a + len - 1) & ~(k_page - 1)) + k_page, true);
                st.hits++; keep_hits++;
                if (plain()) return 1;
                keep->drop_at(a);   // review N5: a failed plain copy through a proven page: the run is not trusted again
                return 0;
            }
            keep->drop_at(a);
        }
        WsInfo wi;
        if (hints.has(a) && !(a < image_end && a + len > image_base) && !runs.covers(a, len)) {
            const bool yes = ws_check(a, len, ws, &wi);
            if (wi.answered) st.ws++;   // review N5: answered calls only
            if (yes) {
                ws_zero_run = 0;
                st.ws_yes++;
                runs.add(a & ~(k_page - 1), ((a + len - 1) & ~(k_page - 1)) + k_page, false);
                copies++;
                if (kernel()) return 1;
                fails++;
                clear();
                return 0;
            }
        }
        const int k = range_check_fast(a, len, query, runs, seen, image_base, &st, &hints, keep, image_end);
        if (wi.answered) {   // review SF1: an answer with no page Valid for a range VirtualQuery finds readable counts towards ws_tripped
            if (wi.any_valid) ws_zero_run = 0;
            else if (k != k_fast_no && ++ws_zero_run >= k_ws_zero_max) ws_tripped = true;
        }
        if (k == k_fast_no) return 0;
        if (k == k_fast_image) return plain() ? 1 : 0;
        copies++;
        if (kernel()) return 1;
        fails++;
        clear();
        return 0;
    }
    /// vq-lean's call: no working-set query, no image end.
    template <class Query, class Plain, class Kernel>
    int read(uintptr_t a, size_t len, int64_t now, int64_t max_age, uintptr_t image_base, Query&& query, Plain&& plain, Kernel&& kernel) {
        return read(a, len, now, max_age, image_base, 0, query, [](const uintptr_t*, uint32_t*, int) { return false; }, plain, kernel);
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
