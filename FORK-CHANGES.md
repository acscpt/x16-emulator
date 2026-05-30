# Fork changes

A list of changes in this fork, recorded incrementally in the
[Keep a Changelog](https://keepachangelog.com/) format. It complements
[`RELEASES.md`](./RELEASES.md), the upstream log organized per named release
and updated en masse.

Newest entries first.

## [Unreleased]

### Added

- **`-debugstdio`: a headless, scriptable REPL frontend for the debugger.** Runs the debugger over stdin/stdout instead of the SDL overlay, so the emulator can be driven by a script, a test harness, or a terminal user. Mutually exclusive with `-debug`.
  - Line-based wire protocol: `RDY` / `ERR <msg>` command replies and asynchronous `* BRK <reason> <bank> <addr>` / `* RES` events, with a `x16db > ` prompt resynced to emulator state on each turn.
  - Command parity with the SDL F12 debugger: register/memory/VRAM/stack/zero-page inspection and edits, disassembly, fill, find, single-step, step-over, continue, reset, and bail.
  - Stateful view cursor: `m` / `d` / `v` dumps with `+`/`-` nudges, a pinned view bank (`b view`), and an optional status header (`hdr` / `st`).
  - Up to 16 simultaneous breakpoints: `sbp` (add), `cbp <bank> <addr>` / `cbp *` (clear one / all), `lbp` (list), and `tb` (toggle at the cursor).
  - Interactive terminal line editor when stdin is a TTY: own echo, backspace, `Enter`, up/down command history, left/right cursor movement, `Delete`, and mid-line editing. Asynchronous events redraw the prompt without clobbering in-progress input. `Ctrl-C` exits cleanly and restores the terminal.
  - User guide and command reference: [`docs/debug_repl.md`](./docs/debug_repl.md) and [`docs/debug_repl_commands.md`](./docs/debug_repl_commands.md).
  - Runs on Linux, macOS, and Windows.
  - Python test harnesses under `testbench/`: `x16dbg_smoke.py` (the pipe protocol) and `x16dbg_pty.py` (the TTY line-editor and signal paths).

- **Memory watchpoints (data breakpoints).** Stop the running CPU when it writes a watched memory address or range, reporting the writing instruction's program counter and the value written; the direct tool for finding what corrupts a memory location. Up to 16, managed by slot id over `-debugstdio`: `swp` (arm), `cwp` (clear one or all), `lwp` (list), and `wp <id> on|off` (enable/disable), with a `* WP <id> w <bank>:<addr>=<val> pc=<bank>:<pc>` event on each hit. Write-only for now; read watchpoints and conditions follow.

### Changed

- Debugger internals refactored into an SDL-free core (`debugger_core.{c,h}`) behind a frontend vtable (`dbg_frontend_t`), so the SDL and stdio frontends share one state machine, breakpoint table, and view-cursor state.

- `-debugstdio` suppresses `-echo` / `-trace` / `-log`, which would otherwise collide with the REPL protocol on stdout.

- `video`: added `video_is_alive()`; the debugger gates `video_update()` on it.

### Fixed

- `-debugstdio` pipe protocol: an asynchronous event (`* BRK` / `* WP` / `* RES`) that fires while the host is sitting at a prompt now starts on its own line. The prompt carries no trailing newline, so a fast breakpoint or watchpoint hit could previously glue onto it (`x16db > * BRK ...`) and a parsing host would miss the event. A TTY already redrew the prompt line; a pipe now emits a leading newline for the same reason.

- Windows build: static-link winpthread for `midi.c`'s mutexes. It previously linked only transitively via FluidSynth, so `-DENABLE_FLUIDSYNTH=OFF` failed to link.

- `video_win32.c`: build under mingw-w64 headers that predate the Windows 11 DWM rounded-corner constants (fall back to literal values).

- `midi.c`: include `<time.h>` for `time()`, which MinGW does not pull in transitively.

### Tooling

- markdownlint configuration (`.markdownlint.jsonc`).
