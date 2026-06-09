# Fork changes

A list of changes in this fork, recorded incrementally in the
[Keep a Changelog](https://keepachangelog.com/) format. It complements
[`RELEASES.md`](./RELEASES.md), the upstream log organized per named release
and updated en masse.

Newest entries first.

## [Unreleased]

### Fixed

- **`-debugstdio` froze VERA, hanging any VBL-paced program.** The mode forced `headless`, which also gated off the per-instruction VERA tick (`video_step`), so the VBL flag at `$9F27`, the raster counter, and the frame ISR never advanced. The CPU ran but anything pacing off VBL -- a very common 6502 graphics idiom, polled or IRQ-driven -- spun forever, and `scr` captured stale frames. VERA now ticks under `-debugstdio` while the mode stays genuinely headless (no SDL, no window); only display presentation (`video_update`) and audio remain gated on a real output. `-testbench` is unchanged.

## [acscpt.2] - 2026-06-06

### Added

- **Breakpoint enable/disable: `bp <bank> <addr> on|off`.** Mute a breakpoint without removing it, the same way `wp <id> on|off` mutes a watchpoint, so one breakpoint can be silenced at a time while bisecting. A disabled breakpoint keeps its definition and condition, does not stop the CPU, and still shows in `lbp` marked ` off`.

### Changed

- **`sbp` at an existing address now replaces that breakpoint in place** instead of being a no-op, so re-issuing `sbp` with a different `if` clause updates the condition (and re-issuing with none clears it). The entry keeps its position in the list.

- **`-debugstdio` protocol version is now `proto=2`** (was `proto=1`), reflecting the `bp` command, the `lbp` ` off` marker, and the `sbp` replace-on-readd behaviour.

### Fixed

- **Breakpoints in the `$A000-$FFFF` banked window (ROM and banked RAM) never fired.** `sbp` stored every breakpoint with `x16Bank = -1`, but the hit test compares against the live bank reported for the address, so a banked breakpoint could not match. `sbp` now stores the named bank for addresses at `$A000` and above (matching how `swp` and the SDL `tb` already work); low addresses stay unbanked. `bp on|off` keys the same way.

- Two startup-breakpoint structs in `main.c` were built uninitialised. This was harmless until the fork added a condition pointer to `struct breakpoint`; that pointer was then left as garbage that a later breakpoint hit or `cbp` could dereference. They are now zero-initialised.

## [acscpt.1] - 2026-05-31

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

- **Memory watchpoints (data breakpoints).** Stop the running CPU when it reads or writes a watched memory address or range, reporting the accessing instruction's program counter and the byte value; the direct tool for finding what writes or reads a memory location. Up to 16, managed by slot id over `-debugstdio`: `swp [r|w|rw]` (arm; default `w`), `cwp` (clear one or all), `lwp` (list), and `wp <id> on|off` (enable/disable), with a `* WP <id> <r|w> <bank>:<addr>=<val> pc=<bank>:<pc>` event on each hit. Debugger pokes and the debugger's own reads are exempt, so inspecting memory never trips a watchpoint.

- **Conditional breakpoints and watchpoints.** Attach an `if <expr>` clause to a breakpoint or watchpoint so it stops the CPU only when the expression is true at the point of the hit: `sbp 00 c04f if a == $ff`, `swp 00 0070 if val != $00 && is_write`. The condition is a C-style integer expression (arithmetic, bitwise, shift, comparison, and short-circuit logical operators, with C precedence) evaluated on the host against a snapshot of machine state, so it never perturbs the guest. Operands resolve to live registers (`a x y sp pc p`), flags (`n v z c i d`), memory (`mem[<expr>]`), and, for a watchpoint, the access under test (`addr val is_write`). The evaluator is a self-contained parser (`debugger_expr.{c,h}`) with its own unit test; see [`docs/debug_repl_commands.md#conditions`](./docs/debug_repl_commands.md#conditions).

- **`scr`: screenshot command over `-debugstdio`.** Writes a PNG of the current screen and prints the path: `scr /tmp/shot.png`, or bare `scr` for a timestamped file. The image is the composited VERA output (text, tiles, bitmap, sprites). Because `-debugstdio` runs headless, the per-scanline render that normally fills the framebuffer does not run, so `scr` composes a frame from current VERA state on demand; it reads VERA state only and does not perturb the running program. Reuses the existing screenshot path (the Cmd/Super+P key still works in a window), now factored so it is reachable from the REPL.

### Changed

- Debugger internals refactored into an SDL-free core (`debugger_core.{c,h}`) behind a frontend vtable (`dbg_frontend_t`), so the SDL and stdio frontends share one state machine, breakpoint table, and view-cursor state.

- `-debugstdio` suppresses `-echo` / `-trace` / `-log`, which would otherwise collide with the REPL protocol on stdout.

- `video`: added `video_is_alive()`; the debugger gates `video_update()` on it.

### Fixed

- Watchpoints attributed an access made by the first instruction after a resume (`cnt` / `stp`) to the previous, stopped program counter. The PC snapshot used for the `* WP` report was taken before the resume command was dispatched; it is now taken at the resume itself. Writes rarely land on the first instruction, so this surfaced mainly once read watchpoints made first-instruction accesses routine.

- `-debugstdio` pipe protocol: an asynchronous event (`* BRK` / `* WP` / `* RES`) that fires while the host is sitting at a prompt now starts on its own line. The prompt carries no trailing newline, so a fast breakpoint or watchpoint hit could previously glue onto it (`x16db > * BRK ...`) and a parsing host would miss the event. A TTY already redrew the prompt line; a pipe now emits a leading newline for the same reason.

- Windows build: static-link winpthread for `midi.c`'s mutexes. It previously linked only transitively via FluidSynth, so `-DENABLE_FLUIDSYNTH=OFF` failed to link.

- `video_win32.c`: build under mingw-w64 headers that predate the Windows 11 DWM rounded-corner constants (fall back to literal values).

- `midi.c`: include `<time.h>` for `time()`, which MinGW does not pull in transitively.

### Tooling

- Fork CI (`.github/workflows/fork-ci.yml`): builds and runs the debugger test suites (`test_debugger_expr`, `x16dbg_smoke`, `x16dbg_pty`) and upstream's `-testbench` core selftest on Linux, plus a mingw compile check on Windows that catches mingw-only issues. Self-contained: it fetches the ROM from a public x16-rom release asset, with no cross-repo artifact access. The selftest runs via `testbench/selftest_fork.py`, which marks the three tests broken on upstream-master itself (`test_stackpointer`, `test_rombank`, `test_rambank`) as expected failures, leaving upstream's `selftest.py` untouched. Upstream's `build.yml` is left untouched but disabled on the fork, since its ROM/docs artifact pulls from other repos cannot run there.

- Release workflow (`.github/workflows/release.yml`) and process doc (`RELEASING.md`). Pushing an `acscpt.<n>` tag builds Linux x86_64, Windows x86_64, and macOS x86_64/arm64 and publishes a GitHub Release. Packages are lean and ROM-free: emulator plus `makecart`, FluidSynth on, trace off, with notes pointing users to x16-rom for the ROM.

- markdownlint configuration (`.markdownlint.jsonc`).
