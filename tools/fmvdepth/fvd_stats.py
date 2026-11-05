#!/usr/bin/env python3
"""fvd_stats.py - flicker and normalisation figures of converted .fvd files (read-only; no game, no model, no GPU).

SPDX-License-Identifier: MIT (this script; part of the FFX VR repository). It only reads your .fvd files and prints
numbers; nothing is written. On Windows run it with the converter's venv (XPRESS decoding needs cabinet.dll):
    %LOCALAPPDATA%\\ffxvr\\fmvdepth-venv\\Scripts\\python.exe tools\\fmvdepth\\fvd_stats.py <...>\\fmvdepth\\opn_us.fvd [more .fvd] [--shots]

Per file, over all shots (the cut flags in the index), one frame at a time:
  noise     median over pixels of |n[t] - (n[t-1] + n[t+1]) / 2| (8-bit levels; a second difference: blind to steady motion),
            averaged over the frames whose neighbours are in the same shot
  pump      mean |median n[t] - median n[t-1]| within shots (levels): the whole-frame breathing of the per-frame normalisation
  p98 jump  mean |p98[t] / p98[t-1] - 1| within shots (the model's raw range change; what the converter's EMA turns into pumping)
  stride-22 the pump series' spectral power at the model's window stride (22 frames) +-1 bin, against the mean bin
            (a peak means the windows' alignment steps show)
  clipped   the mean share of pixels at 255 / at 0 (the converter's p2 / p98 clips 2 % at each end)
  KB        the mean payload size
  --shots   one line per shot too: frames, the raw range (mean hi - lo of the index) and the noise; if the model's raw noise is
            constant, the noise in levels grows as the raw range shrinks (flat shots get their noise stretched)
Design notes: research audit "FMV-to-3D pipeline" section 3 (the metrics) and "FMV relief screen" (round 7)."""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fvd  # noqa: E402


def stats(path):
    r = fvd.FvdReader(path)
    try:
        n = len(r)
        ents = [r.entry(i) for i in range(n)]
        cut = np.array([bool(e.flags & fvd.FF_CUT) or i == 0 for i, e in enumerate(ents)], bool)
        p98 = np.array([e.p98 for e in ents], np.float64)
        size = np.array([e.size for e in ents], np.float64)
        prev2 = prev1 = None
        noise, med, c255, c0 = [], np.empty(n), np.empty(n), np.empty(n)
        nz = np.full(n, np.nan)
        for i in range(n):
            f = r.frame(i).astype(np.int16)
            med[i] = np.median(f); c255[i] = (f == 255).mean(); c0[i] = (f == 0).mean()
            if prev2 is not None and not cut[i] and not cut[i - 1]:
                noise.append(float(np.median(np.abs(prev1 - (prev2 + f) / 2.0)))); nz[i] = noise[-1]
            prev2, prev1 = prev1, f
            if cut[i]:
                prev2, prev1 = None, f
    finally:
        r.close()
    same = ~cut[1:]
    pump = np.abs(np.diff(med))[same]
    pj = np.abs(p98[1:] / np.maximum(p98[:-1], 1e-9) - 1)[same]
    s = np.abs(np.diff(med)) * same
    w22 = float('nan')
    if len(s) >= 88:
        P = np.abs(np.fft.rfft(s - s.mean())) ** 2
        k = int(round(len(s) / 22.0))
        w22 = P[max(1, k - 1):k + 2].mean() / max(P[1:].mean(), 1e-12)
    lo = np.array([e.lo for e in ents]); hi = np.array([e.hi for e in ents])
    starts = list(np.where(cut)[0]) + [n]
    shots = [(a, b, float(np.mean(hi[a:b] - lo[a:b])), float(np.nanmean(nz[a:b])) if np.isfinite(nz[a:b]).any() else float('nan'))
             for a, b in zip(starts[:-1], starts[1:])]
    return dict(shot_list=shots, frames=n, shots=int(cut.sum()), noise=float(np.mean(noise)) if noise else float('nan'),
                pump=float(pump.mean()) if len(pump) else 0.0, p98_jump=float(pj.mean()) if len(pj) else 0.0, w22=w22,
                clip255=float(c255.mean()) if n else 0.0, clip0=float(c0.mean()) if n else 0.0, KB=float(size.mean()) / 1024 if n else 0.0)


def main(argv):
    per_shot = '--shots' in argv
    files = [a for a in argv if a != '--shots']
    if not files:
        print(__doc__)
        return 2
    for p in files:
        try:
            s = stats(p)
        except (OSError, fvd.FvdError) as e:
            print(f'{os.path.basename(p)}: cannot read: {e}')
            continue
        print(f'{os.path.basename(p)}: {s["frames"]} frames, {s["shots"]} shots | noise {s["noise"]:.2f} lv | pump {s["pump"]:.2f} lv/frame | '
              f'p98 jump {100 * s["p98_jump"]:.2f} %/frame | stride-22 power x{s["w22"]:.1f} | clipped 255 {100 * s["clip255"]:.1f} % / '
              f'0 {100 * s["clip0"]:.1f} % | {s["KB"]:.1f} KB/frame')
        if per_shot:
            for a, b, rg, nzv in s['shot_list']:
                print(f'    shot {a:6d}-{b - 1:6d} ({b - a:5d} frames): raw range {rg:8.4f}, noise {nzv:.2f} lv')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
