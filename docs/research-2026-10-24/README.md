# Battle cursors and FFX.exe deep dive (24 Oct 2026)

A research round run by a team of agents on a Ghidra decompile of FFX.exe (patch of 1 Oct 2026) plus
Fahrenheit's game definitions (commit ecb6702). Static analysis only; nothing here was confirmed in game yet.

- `SYNTHESIS.md` - start here: the diagnosis, the recommended battle-cursor architecture, the staged plan,
  other findings ranked, the proposed build order, and what to log in the diagnostic build.
- `cursor-current-state.md` - how the mod handled battle cursors at build 6, and where it fought itself.
- `exe-battle-ui.md` - battle UI internals (target/turn pointers, shared projection, CTB, damage numbers).
- `exe-engine.md` - engine internals (threads, cameras, render loop, loading, UI submit, FMV, input).
- `exe-verify.md` - an independent check of the claims a design rests on (its corrections override).
- `BOARD.md` - the team's shared question/answer board, as it was at the end.

The new EXE facts are merged into `docs/exe-analysis.md` (sections 16 onward).
