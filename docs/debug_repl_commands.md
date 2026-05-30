# X16 emulator stdio REPL command reference

This is a per-command reference for everything available at the `x16db >` prompt. Each entry covers the purpose of the command, its syntax, a short example, any known caveats, and the other commands that go alongside it. The wider concepts the commands operate on (machine states, the view cursor, the header) are described in [the main REPL doc](debug_repl.md), and reading that first will make the entries below easier to follow.

## Conventions

- **Numbers are hex.** Every numeric argument (addresses, banks, byte values, counts, offsets) is parsed as hexadecimal without prefix. `c010`, not `$c010` or `0xc010`. Output values are also lowercase hex without prefix. The one exception is inside a [condition expression](#conditions), which follows C conventions: decimal by default, hex only with a `$` or `0x` prefix.

- **Banks are one byte.** X16 RAM has 256 banks (`00`-`ff`); ROM has 32 banks (`00`-`1f`). The CPU's currently-selected bank lives in `$00` (RAM) and `$01` (ROM); commands that take an explicit bank read or write the named bank regardless of which one the CPU has selected.

- **Addresses are CPU addresses.** Most read/write commands take a `<bank> <addr>` pair where `<addr>` is the CPU-visible 16-bit address. The bank value applies in the banked windows (`$A000-$BFFF` for RAM, `$C000-$FFFF` for ROM); for unbanked low RAM (`$0000-$9FFF`) the bank value is ignored.

- **VRAM addresses are 17-bit.** VRAM is a separate address space, `00000`-`1ffff`, and the VRAM-specific commands (`v`, `vmr`, `vmw`) take no bank argument.

- **Responses end with `RDY` or `ERR <message>`.** Data lines come before the terminator. Asynchronous events (`* BRK ...`, `* RES`, `* BP ...`, `* WP ...`) appear between commands, never inside one.

- **Case-sensitive.** Commands and register names are lowercase. Arguments are split on whitespace; the shell does not tokenize quoted strings.

## Index

| Command | Category | Description | SDL UI |
| --- | --- | --- | --- |
| [`brk`](#brk) | Execution | Force the CPU into STOP | F12 |
| [`cnt`](#cnt) / [`c`](#cnt) | Execution | Resume the CPU | F5 |
| [`stp`](#stp) / [`s`](#stp) | Execution | Single-step one instruction | F11 |
| [`sov`](#sov) / [`n`](#sov) | Execution | Step over `JSR` / `JSL` / `JML` | F10 |
| [`rst`](#rst) | Execution | Reset the CPU | F2 |
| [`m`](#m) | View cursor | Move data cursor and dump RAM | PgUp / PgDn |
| [`d`](#d) | View cursor | Move disasm cursor and disassemble | Shift+PgUp / Shift+PgDn |
| [`v`](#v) | View cursor | Move data cursor and dump VRAM | PgUp / PgDn (VRAM mode) |
| [`b`](#b) | View cursor | Set CPU bank, or view-bank override | KP+ / KP- (view bank) |
| [`r`](#r) | View cursor | Set a CPU register | click register, type new value |
| [`f`](#f) | View cursor | Fill RAM or VRAM (bypasses I/O) | - |
| [`home`](#home) | View cursor | Snap disasm cursor to CPU PC | F1 |
| [`tb`](#tb) | View cursor | Toggle breakpoint at disasm cursor | F9 |
| [`sbp`](#sbp) | Breakpoints | Add a user breakpoint, optionally [conditional](#conditions) | F9 at cursor |
| [`cbp`](#cbp) | Breakpoints | Clear one breakpoint, or all | F9 again at cursor |
| [`lbp`](#lbp) | Breakpoints | List active breakpoints | visible in panel |
| [`swp`](#swp) | Watchpoints | Add a write watchpoint, optionally [conditional](#conditions) | - |
| [`cwp`](#cwp) | Watchpoints | Clear one watchpoint, or all | - |
| [`lwp`](#lwp) | Watchpoints | List active watchpoints | - |
| [`wp`](#wp) | Watchpoints | Enable / disable a watchpoint | - |
| [`mem`](#mem) | Memory | Read RAM (stateless) | - |
| [`wmm`](#wmm) | Memory | Write RAM bytes (stateless) | - |
| [`fil`](#fil) | Memory | Fill RAM via the CPU write path | - |
| [`find`](#find) | Memory | Find a byte pattern in RAM | - |
| [`vmr`](#vmr) | Memory | Read VRAM | - |
| [`vmw`](#vmw) | Memory | Write VRAM bytes | - |
| [`dis`](#dis) | Memory | Disassemble (stateless) | - |
| [`reg`](#reg) | Inspection | Dump CPU registers | visible in panel |
| [`srg`](#srg) | Inspection | Set a CPU register (explicit form of `r`) | click register, type new value |
| [`stk`](#stk) | Inspection | Show the top of the 6502 stack | visible in panel |
| [`zpr`](#zpr) | Inspection | Show direct-page R0..R15 register pairs | - |
| [`vrg`](#vrg) | Inspection | VERA state snapshot | - |
| [`clk`](#clk) | Inspection | Clocks since last resume | visible in panel |
| [`hdr`](#hdr) | Header | Show or toggle header lines | - |
| [`st`](#st) | Header | Full state snapshot | always visible in panels |
| [`mod`](#mod) | Session | Current mode and PC | visible in panel |
| [`ver`](#ver) | Session | Protocol version | - |
| [`hlp`](#hlp) | Session | Inline help text | - |
| [`quit`](#quit) / [`qit`](#quit) | Session | Detach and exit 0 | close window |
| [`bail`](#bail) | Session | Detach and exit 1 | - |

## Execution control

---

### `brk`

**Purpose**

When the emulator launches without a startup breakpoint the CPU runs freely from reset; `brk` is the way to interrupt it. Typed at the prompt, it brings the CPU into STOP at the next instruction boundary and queues a `* BRK USER <bank> <addr>` asynchronous event reporting where the stop took effect. The next prompt comes back with the CPU stopped and the disasm cursor in the header pointing at the instruction the CPU is about to execute next.

The command is the manual equivalent of pressing F12 in the SDL UI.

**Syntax**

```text
brk
```

**Example**

```text
x16db > brk
RDY
* BRK USER 00 c012
```

**Notes**

If the CPU is already in STOP, `brk` is harmless apart from re-emitting the event.

**Associated commands**: [`cnt`](#cnt), [`stp`](#stp), [`sbp`](#sbp), [`rst`](#rst)

[^ Index](#index)

---

### `cnt`

**Purpose**

When the CPU is in STOP, whether at launch with a `-debugstdio <addr>` startup breakpoint, after a `brk`, or after a step, `cnt` resumes free-running execution. The prompt returns immediately, a `* RES` asynchronous event fires on the way out of STOP, and the CPU runs concurrently with any commands typed at the prompt afterwards. The CPU continues until something stops it again: a user breakpoint hit, the `STP` opcode (`$DB`), or another `brk` from the shell.

`c` is accepted as a short alias.

**Syntax**

```text
cnt
c
```

**Example**

```text
x16db > cnt
RDY
* RES
```

**Associated commands**: [`brk`](#brk), [`stp`](#stp), [`sbp`](#sbp)

[^ Index](#index)

---

### `stp`

**Purpose**

`stp` advances the CPU by exactly one instruction and returns to STOP. The emulator passes briefly through the STEP state for the duration of that one instruction; the prompt is held until STOP is reached, so the next prompt always comes back with the CPU already stopped at the new program counter. A `* BRK STEP <bank> <addr>` asynchronous event fires on the transition, and the disasm cursor in the header auto-snaps to the new PC so the very next instruction the CPU will execute appears at the top of the disassembly.

`stp` expects the CPU to be in STOP. Issued during RUN it still works, but the step latches onto whichever instruction the CPU happens to land on at the sample point, which is rarely what was wanted; precede with `brk` for predictable behaviour.

`s` is accepted as a short alias.

**Syntax**

```text
stp
s
```

**Example**

```text
x16db > stp
RDY
* BRK STEP 00 c013
```

**Associated commands**: [`sov`](#sov), [`brk`](#brk), [`cnt`](#cnt), [`home`](#home)

[^ Index](#index)

---

### `sov`

**Purpose**

`sov` ("step over") is the smarter cousin of [`stp`](#stp) for subroutine calls. When the instruction at the program counter is `JSR`, `JSL`, or `JML`, `sov` sets an internal breakpoint at the return address, resumes the CPU (firing `* RES`), and waits for the call to return; the next event is `* BRK BREAKPOINT <bank> <addr>` from the return point. For any other instruction `sov` is identical to `stp`: one instruction executes and `* BRK STEP <bank> <addr>` fires immediately.

`n` is accepted as a short alias.

**Syntax**

```text
sov
n
```

**Example** (instruction at PC is a call)

```text
x16db > sov
RDY
* RES
* BRK BREAKPOINT 00 c016
```

**Example** (instruction at PC is not a call)

```text
x16db > sov
RDY
* BRK STEP 00 c013
```

**Notes**

If the called subroutine never returns (infinite loop, never `RTS`), `sov` will not stop until something else (a user breakpoint, a `brk` from the shell, a host signal) intervenes.

**Associated commands**: [`stp`](#stp), [`brk`](#brk), [`cnt`](#cnt)

[^ Index](#index)

---

### `rst`

**Purpose**

`rst` resets the CPU and only the CPU. The registers are cleared, the program counter is reloaded from the reset vector, and a `* BRK USER <bank> <addr>` asynchronous event fires from the new PC; peripherals, VERA, and RAM are left untouched. The intent is to recover a wedged CPU, one that has jumped into garbage or executed the `STP` opcode (`$DB`), without losing the rest of the machine's state.

For a full machine reset, exit the emulator and relaunch.

**Syntax**

```text
rst
```

**Example**

```text
x16db > rst
RDY
* BRK USER 00 fff0
```

**Associated commands**: [`brk`](#brk), [`cnt`](#cnt)

[^ Index](#index)

## View cursor

The eight commands in this section operate on the *view cursor*, the shared piece of debugger state that records "where the user is looking" in memory and in the disassembly. See [The view cursor](debug_repl.md#the-view-cursor) in the main REPL doc for the conceptual model. The same commands and shortcuts exist in the SDL UI; the REPL forms mirror them.

---

### `m`

**Purpose**

`m` is the workhorse for browsing RAM. It moves the data cursor and dumps 16 rows (256 bytes) of memory at the new position, formatted as one row of address, sixteen hex bytes, and an ASCII column per line. Typed without arguments it re-emits the dump from the current cursor, useful after a step has changed the bytes underneath. `m +` and `m -` nudge the cursor forward or backward by `0x100` (the default step, matching PgUp / PgDn in the SDL UI); `m +<off>` and `m -<off>` use a custom offset. An explicit `<addr>` or `<bank>:<addr>` jumps the cursor outright, and the bank form additionally pins the view bank to `<bank>` until cleared with `b view follow`.

**Syntax**

```text
m
m <addr>
m <bank>:<addr>
m + | -
m +<off> | -<off>
```

- `<addr>` is a 16-bit CPU address.
- `<bank>` is the X16 RAM bank (`00`-`ff`) or ROM bank (`00`-`1f`); a literal `:` separates the bank from the address.

**Example**

```text
x16db > m 0200
   00:0200  20 04 c0 00  00 00 00 00  00 00 00 00  00 00 00 00  . ..............
   00:0210  ...
RDY
x16db > m +
   00:0300  ...
RDY
x16db > m 5:a000
   05:a000  ...
RDY
```

**Notes**

Non-printable bytes render as `.` in the ASCII column.

**Associated commands**: [`d`](#d), [`v`](#v), [`b`](#b), [`mem`](#mem)

[^ Index](#index)

---

### `d`

**Purpose**

`d` is the disassembly counterpart to [`m`](#m). It moves the disasm cursor and emits 16 instructions starting at the new position. Bare `d` re-emits from the current cursor, `d +` / `-` nudges (default step `0x10`, matching Shift+PgUp / Shift+PgDn in the SDL UI), and `d <bank>:<addr>` jumps to an absolute position. The disasm cursor is also the position that `tb` reads when toggling a breakpoint.

The cursor auto-snaps to the CPU's PC on every transition into STOP, so the dump after a `brk`, a breakpoint hit, or a step always begins at the next instruction the CPU will execute. To look at a different range of code, use `d <addr>`; [`home`](#home) brings the cursor back to the CPU's current PC.

**Syntax**

```text
d
d <addr>
d <bank>:<addr>
d + | -
d +<off> | -<off>
```

**Example**

```text
x16db > d c010
00:c010 ad 00 02     lda $0200
00:c013 85 03        sta $03
00:c015 ...
RDY
```

**Notes**

The disassembler respects the current CPU mode, so `-c816` builds emit 65C816 mnemonics correctly.

**Associated commands**: [`m`](#m), [`home`](#home), [`dis`](#dis), [`tb`](#tb)

[^ Index](#index)

---

### `v`

**Purpose**

`v` is the VRAM equivalent of [`m`](#m). It moves the data cursor into VRAM mode and dumps 16 rows of VRAM at the new address. VRAM is a separate 17-bit address space (`00000`-`1ffff`), so `v` takes no bank argument. The default nudge step is `0x200`.

Switching between `m` and `v` shares the same cursor address: `v 0` after `m 1000` jumps to VRAM `$00000`, not back to `$1000` in RAM. The header's mode field (`RAM` or `VRAM` on line 3) reflects which space the cursor is currently anchored to.

**Syntax**

```text
v
v <addr>
v + | -
v +<off> | -<off>
```

- `<addr>` is a 17-bit VRAM address (`00000`-`1ffff`).

**Example**

```text
x16db > v 0
   00000  10 20 30 40  ...
RDY
x16db > v +
   00200  ...
RDY
```

**Associated commands**: [`m`](#m), [`vmr`](#vmr), [`vmw`](#vmw)

[^ Index](#index)

---

### `b`

**Purpose**

`b` is the bank-control command and has three distinct forms. `b ram <bank>` and `b rom <bank>` write to the CPU's bank registers `$00` and `$01`, changing which RAM or ROM bank is mapped into the `$A000-$BFFF` and `$C000-$FFFF` windows of CPU address space; this affects the running program. `b view <bank>` is different: it sets the *view-bank override* on the data cursor, so subsequent `m` reads come from `<bank>` regardless of which bank the CPU has selected, and the CPU's bank registers are left alone. `b view +` and `b view -` nudge the override by 1; `b view follow` clears the override and returns the dump to following the CPU.

Use the view-bank override to inspect code or data in a bank the CPU is not currently executing from. See [The view-bank override](debug_repl.md#the-view-bank-override) for the wider story.

**Syntax**

```text
b ram <bank>
b rom <bank>
b view <bank>
b view + | -
b view follow
```

- `<bank>` is `00`-`ff` for RAM and view-bank, `00`-`1f` for ROM.

**Example**

```text
x16db > b view 5
RDY
x16db > m a000
   05:a000  ...
RDY
x16db > b view follow
RDY
```

**Associated commands**: [`m`](#m), [`d`](#d)

[^ Index](#index)

---

### `r`

**Purpose**

`r` sets a CPU register. It is the cursor-side equivalent of [`srg`](#srg): `r pc c100` and `srg pc c100` have the same effect, but `r` is the form that mirrors the SDL UI's register-edit shortcut. All registers are accessible: `pc`, `a`, `b`, `c`, `x`, `y`, `sp`, `p`, `k`, `db`, `dp`, `e`. On 65C02 builds the 65C816-only registers (`b`, `c`, `db`, `dp`, `e`) are still settable but have no effect on execution.

**Syntax**

```text
r <name> <hex>
```

- `<name>` is one of the register names above.
- `<hex>` is the new value; the width is implied by the register.

**Example**

```text
x16db > r pc c100
RDY
x16db > r a 42
RDY
```

**Notes**

Setting `pc` does not transition the machine state. To resume execution from the new PC, follow with [`cnt`](#cnt).

**Associated commands**: [`srg`](#srg), [`reg`](#reg)

[^ Index](#index)

---

### `f`

**Purpose**

`f` fills a range of memory with a given byte value, optionally stepping each successive byte by a constant. Unlike [`fil`](#fil), which routes its writes through the CPU's write path and so triggers any I/O side effects in the `$9F00-$9FFF` range, `f` writes directly to the underlying RAM or VRAM buffer. That makes `f` useful for laying down test patterns in memory without disturbing peripheral state.

The mode follows the data cursor: in RAM mode `f` writes to RAM (using the cursor's view bank, or the CPU's currently-selected bank when the override is set to follow); in VRAM mode it writes to VRAM. Switch modes by running [`m`](#m) or [`v`](#v) first.

**Syntax**

```text
f <addr> <val> [<count>] [<incr>]
```

- `<addr>` is the target start address (RAM: 24-bit linear; VRAM: 17-bit).
- `<val>` is the byte value written at the first address.
- `<count>` is the number of bytes to write (default `1`).
- `<incr>` is the per-byte increment added to `<val>` (default `1`); `0` fills the range with a constant value.

**Example**

```text
x16db > f 0200 00 100 1
RDY
x16db > m 0200
   00:0200  00 01 02 03  04 05 06 07  08 09 0a 0b  0c 0d 0e 0f  ................
   00:0210  10 11 12 ...
RDY
```

**Associated commands**: [`fil`](#fil), [`m`](#m), [`v`](#v), [`wmm`](#wmm), [`vmw`](#vmw)

[^ Index](#index)

---

### `home`

**Purpose**

`home` snaps the disasm cursor back to the CPU's current program counter and emits a fresh disassembly from there. It is the manual equivalent of the auto-snap that happens on every CPU transition into STOP: after wandering off with `d <addr>` while the CPU is stopped, `home` brings the view back to where the CPU actually is.

In the SDL UI this is bound to F1.

**Syntax**

```text
home
```

**Example**

```text
x16db > d c100
00:c100 ...
RDY
x16db > home
00:c012 ad 00 02     lda $0200
RDY
```

**Associated commands**: [`d`](#d), [`tb`](#tb)

[^ Index](#index)

---

### `tb`

**Purpose**

`tb` toggles the user breakpoint at the disasm cursor's current position. If no breakpoint is set at that bank and address, one is added; if one is already there, it is cleared. Either path emits the corresponding `* BP SET <bank>:<addr>` or `* BP CLEAR <bank>:<addr>` event in the response, so the change appears in the protocol stream. `tb` pairs naturally with [`d`](#d): move the cursor to the instruction of interest, hit `tb`, and the breakpoint lands without typing the address out.

**Syntax**

```text
tb
```

**Example**

```text
x16db > d c100
00:c100 ...
RDY
x16db > tb
* BP SET 00:c100
RDY
```

**Notes**

`tb` adds to the breakpoint table; toggling one on at a fresh cursor position leaves any breakpoints set elsewhere in place. Toggling at a position that already has a breakpoint clears just that one. The table holds up to 16 breakpoints; if it is full and the cursor position is not already set, `tb` responds with `ERR breakpoint table full`.

**Associated commands**: [`d`](#d), [`sbp`](#sbp), [`cbp`](#cbp), [`lbp`](#lbp)

[^ Index](#index)

## Breakpoints

The debugger holds up to 16 user breakpoints. The three commands in this category add one explicitly, clear one (or all) explicitly, and list those active. For adding and clearing a breakpoint at the disasm cursor's current position with a single keystroke, see [`tb`](#tb) in the View cursor section.

---

### `sbp`

**Purpose**

`sbp` adds a user breakpoint at an explicit bank and address. Once set, the CPU stops with `* BRK BREAKPOINT <bank> <addr>` the next time execution reaches that location, whether during a `cnt`-resumed run or as the result of a step. This is the form to use from a script or any context where the address is known up front; for a one-keystroke toggle at the disasm cursor instead, see [`tb`](#tb).

Each `sbp` adds to the table rather than replacing it; re-adding the same bank and address is a no-op. The table holds up to 16 breakpoints, and `sbp` responds with `ERR breakpoint table full` once it is exhausted.

An optional `if <cond>` clause attaches a [condition](#conditions): the breakpoint then stops the CPU only when `<cond>` is true at that address, and is otherwise transparent. See [Conditions](#conditions) for the expression grammar.

**Syntax**

```text
sbp <bank> <addr> [if <cond>]
```

- `<bank>` is the one-byte CPU bank.
- `<addr>` is the 16-bit CPU address.
- `if <cond>` is an optional [condition expression](#conditions); the breakpoint stops only when it evaluates non-zero.

**Example**

```text
x16db > sbp 00 c010
RDY
x16db > sbp 00 c04f if a == $ff
RDY
```

**Notes**

No asynchronous event is emitted for breakpoints set or cleared by `sbp` and `cbp`. Use [`tb`](#tb) at the disasm cursor if you want the `* BP SET` / `* BP CLEAR` events to appear in the protocol stream.

A malformed condition is rejected here, when the breakpoint is set, with `ERR <message>`; the breakpoint is not added.

**Associated commands**: [`cbp`](#cbp), [`lbp`](#lbp), [`tb`](#tb)

[^ Index](#index)

---

### `cbp`

**Purpose**

`cbp <bank> <addr>` removes the breakpoint at the given bank and address. If no breakpoint matches, the command refuses with `ERR no such breakpoint` and the table is unchanged. `cbp *` clears every breakpoint at once.

**Syntax**

```text
cbp <bank> <addr>
cbp *
```

**Example**

```text
x16db > sbp 00 c010
RDY
x16db > cbp 00 c010
RDY
x16db > cbp 00 c010
ERR no such breakpoint
```

**Notes**

To clear whatever is set at the disasm cursor without typing the address, use [`tb`](#tb) at that position. To wipe the whole table in one step, use `cbp *`.

**Associated commands**: [`sbp`](#sbp), [`lbp`](#lbp), [`tb`](#tb)

[^ Index](#index)

---

### `lbp`

**Purpose**

`lbp` lists the active breakpoints, one `<bank>: <addr>` line each (hex), in the order they were added, followed by `RDY`. A breakpoint with a [condition](#conditions) appends `  if <cond>` to its line. When none are set the response is just `RDY` with no data lines, so the line count is itself the breakpoint count.

**Syntax**

```text
lbp
```

**Example**

```text
x16db > sbp 00 c010
RDY
x16db > sbp 00 e000 if mem[$30] == $01
RDY
x16db > lbp
00: c010
00: e000  if mem[$30] == $01
RDY
x16db > cbp *
RDY
x16db > lbp
RDY
```

**Associated commands**: [`sbp`](#sbp), [`cbp`](#cbp), [`tb`](#tb)

[^ Index](#index)

## Watchpoints

A watchpoint, or data breakpoint, stops the CPU when the running program *accesses* a memory location, where a breakpoint stops when execution *reaches* one. Their headline use is tracking down memory corruption: when a byte is being clobbered and you do not know by whom, arm a write watchpoint on it, run, and every hit hands back the program counter of the instruction that wrote it. The culprit reveals itself instead of being guessed at.

The debugger holds up to 16 watchpoints, managed by slot id. They are currently write-only; read watchpoints arrive in a later build. A watchpoint can carry a [condition](#conditions) so it stops only on writes that matter. A hit fires an asynchronous `* WP <id> w <bank>:<addr>=<val> pc=<bank>:<pc>` event and returns the CPU to STOP at the instruction boundary just after the write, with the disasm cursor on the next instruction. The fields are the watchpoint id, the access type (`w`), the bank and address written and the byte value, and the bank and program counter of the instruction that made the write.

A watchpoint fires only on writes made by the running CPU. Debugger pokes ([`wmm`](#wmm), [`fil`](#fil)) are deliberately exempt, so inspecting or editing memory never trips your own watchpoints.

---

### `swp`

**Purpose**

`swp` arms a write watchpoint over a single address or an inclusive range. While the CPU runs, the next instruction that writes anywhere in the range stops it and emits the `* WP` event described above. This is the answer to "what is changing this byte?".

Each `swp` takes the lowest free slot and prints the assigned id (`wp N set`). The id is stable for the watchpoint's lifetime, so it keeps referring to the same watchpoint until you delete it. The table holds up to 16 watchpoints; `swp` responds with `ERR watchpoint table full` once it is exhausted. Read access (`r` / `rw`) is reserved for a later build and is rejected for now.

An optional `if <cond>` clause attaches a [condition](#conditions): the watchpoint then stops only on a write for which `<cond>` is true, and the `val`, `addr`, and `is_write` operands describe that write. See [Conditions](#conditions).

**Syntax**

```text
swp [w] <bank> <addr> [end] [if <cond>]
```

- `[w]` is the optional access type. Only `w` (write) is accepted in this build, and it is the default, so it can be omitted.
- `<bank>` is the X16 RAM/ROM bank for an address in the `$A000-$FFFF` window. Below that window the address is unbanked and the bank argument is conventionally `00`. A banked watchpoint matches only while the bank it names is the one currently mapped; an unbanked (zero-page / low-RAM) watchpoint always matches.
- `<addr>` is the 16-bit address. `[end]` is an optional inclusive range end; omit it to watch a single byte.
- `if <cond>` is an optional [condition expression](#conditions); the watchpoint stops only on a write where it evaluates non-zero.

**Example**

```text
x16db > swp 00 0070
wp 0 set
RDY
x16db > cnt
RDY
* RES
* WP 0 w 00:0070=aa pc=00:c1a3
```

The watchpoint at zero-page `$70` fires when the instruction at `$C1A3` writes `$AA` to it, and the CPU stops.

**Notes**

To watch a banked address, name the bank: `swp 02 a100` watches `$A100` only while RAM bank 2 is mapped. A range is handy for a small structure: `swp 00 0080 008f` covers sixteen bytes.

To stop only on a particular value, add a condition: `swp 00 0070 if val == $ff` ignores every write to `$70` except the one that stores `$FF`. A malformed condition is rejected here, when the watchpoint is set, with `ERR <message>`.

**Associated commands**: [`cwp`](#cwp), [`lwp`](#lwp), [`wp`](#wp)

[^ Index](#index)

---

### `cwp`

**Purpose**

`cwp <id>` removes the watchpoint in slot `<id>`. If the slot is empty the command refuses with `ERR no such watchpoint`. `cwp *` clears every watchpoint at once.

Removing a watchpoint frees its slot without renumbering the others, so the remaining ids are unchanged and the next `swp` reuses the lowest free slot. That slot stability is what lets an id you are holding always mean the same watchpoint.

**Syntax**

```text
cwp <id>
cwp *
```

**Example**

```text
x16db > lwp
0: w 00:0070 hits=0
1: w 00:0080-008f hits=0
x16db > cwp 0
RDY
x16db > lwp
1: w 00:0080-008f hits=0
```

After clearing slot 0, slot 1 keeps its id; it is not renumbered to 0.

**Notes**

`<id>` is the decimal slot number shown by [`lwp`](#lwp), not an address.

**Associated commands**: [`swp`](#swp), [`lwp`](#lwp), [`wp`](#wp)

[^ Index](#index)

---

### `lwp`

**Purpose**

`lwp` lists the active watchpoints, one per line, followed by `RDY`. Each line is `<id>: <access> <bank>:<addr>[-<end>] hits=<n>`, followed by ` if <cond>` when it carries a [condition](#conditions) and ` off` when it is disabled. The id is the slot number used by [`cwp`](#cwp) and [`wp`](#wp); `hits` is the running count of times the watchpoint has fired. When none are set the response is just `RDY`, so the line count is the watchpoint count.

**Syntax**

```text
lwp
```

**Example**

```text
x16db > swp 00 0070
wp 0 set
x16db > swp 00 0080 008f if val != $00
wp 1 set
x16db > wp 0 off
RDY
x16db > lwp
0: w 00:0070 hits=0 off
1: w 00:0080-008f hits=0 if val != $00
RDY
```

**Associated commands**: [`swp`](#swp), [`cwp`](#cwp), [`wp`](#wp)

[^ Index](#index)

---

### `wp`

**Purpose**

`wp <id> on` and `wp <id> off` enable or disable the watchpoint in slot `<id>` without removing it. A disabled watchpoint keeps its definition and hit count but does not fire, which is the natural way to bisect a problem: mute one watchpoint at a time rather than deleting and retyping it. `wp` refuses with `ERR no such watchpoint` if the slot is empty.

**Syntax**

```text
wp <id> on | off
```

**Example**

```text
x16db > wp 1 off
RDY
x16db > wp 1 on
RDY
```

**Associated commands**: [`swp`](#swp), [`cwp`](#cwp), [`lwp`](#lwp)

[^ Index](#index)

## Conditions

A breakpoint or watchpoint can carry a condition: an expression, written after the keyword `if`, that must evaluate to a non-zero value for a hit to stop the CPU. When the condition is false the candidate hit is ignored and the CPU runs on, the watchpoint's hit count unchanged. This turns "stop here" into "stop here when it matters": the pass through a loop where the index is finally zero, the one write that stores a sentinel, the call made only with a particular argument.

The expression is evaluated on the host at the moment of the candidate hit, against a snapshot of the machine's state. It never runs on the emulated CPU and never disturbs it; there are no guest cycles, memory writes, or side effects. A condition that does not parse is rejected when the breakpoint or watchpoint is set, with `ERR <message>`, so a live breakpoint always holds a valid condition.

### Operands

Each operand resolves to an integer read from the machine at the point of the hit.

| Operand | Meaning |
| --- | --- |
| `a` `x` `y` | Accumulator and index registers |
| `sp` | Stack pointer |
| `pc` | Program counter |
| `p` | Processor status byte |
| `n` `v` `z` `c` `i` `d` | Individual status flags, each `0` or `1` |
| `mem[<expr>]` | The byte of CPU memory at address `<expr>`, in the currently mapped bank |
| `addr` | The address being accessed (watchpoints) |
| `val` | The byte value being written (watchpoints) |
| `is_write` `is_read` | The access type, `0` or `1` (watchpoints) |

`addr`, `val`, `is_write`, and `is_read` describe the access that triggered a watchpoint. A breakpoint has no access behind it, so in a breakpoint condition they read as `0`. `is_read` is reserved for read watchpoints and is `0` for now. `mem[...]` reads the live byte at any address: `mem[$70]`, `mem[sp + 1]`.

### Operators

The usual C operators, with C precedence and associativity. Highest precedence first:

| Operators | Meaning |
| --- | --- |
| `-` `~` `!` (unary) | Negate, bitwise NOT, logical NOT |
| `*` `/` `%` | Multiply, divide, modulo |
| `+` `-` | Add, subtract |
| `<<` `>>` | Shift |
| `<` `<=` `>` `>=` | Relational |
| `==` `!=` | Equality |
| `&` | Bitwise AND |
| `^` | Bitwise XOR |
| `\|` | Bitwise OR |
| `&&` | Logical AND, short-circuit |
| `\|\|` | Logical OR, short-circuit |

Parentheses group as usual. The comparison and logical operators yield `0` or `1`, and the logical operators short-circuit: in `a && mem[a]` the right side is not read when `a` is zero.

### Values

Working values are signed 64-bit integers, so intermediate arithmetic does not wrap at a byte or a word and `val - $80 < 0` reads the way it looks. Operands still resolve at their natural width: a byte for `a`, the flags, `val`, and `mem[...]`; sixteen bits for `pc` and `sp`.

Literals are decimal (`42`) or hexadecimal with a `$` or `0x` prefix (`$ff`, `0xFF`). A bare `ff` is an identifier, not a number, and is rejected. Division or modulo by zero yields `0` rather than faulting.

### Examples

```text
sbp 00 c04f if a == $ff
sbp 00 c04f if x >= 8 && y == 0
sbp 00 e000 if mem[$30] == $01 && c
swp w 00 0070 if val != $00
swp w 00 0080 008f if is_write && addr == $0083
```

**Associated commands**: [`sbp`](#sbp), [`swp`](#swp), [`lbp`](#lbp), [`lwp`](#lwp)

[^ Index](#index)

## Memory access

The seven commands in this section read and write memory directly with explicit arguments, separate from the stateful [`m`](#m), [`d`](#d), [`v`](#v) family in the View cursor section. They are the right form for scripted operations or for inspections that should not disturb the view cursor's current position. The distinction between commands that bypass I/O (writes to the underlying RAM or VRAM buffer) and commands that go through the CPU's `write6502` path (and so trigger emulated peripheral side effects) is called out in the entries for [`wmm`](#wmm) and [`fil`](#fil).

---

### `mem`

**Purpose**

`mem` reads a contiguous block of CPU RAM and prints it in the same hex+ASCII row format as [`m`](#m), but without moving the view cursor. The output is one row per 16 bytes: address, sixteen hex bytes, and an ASCII column with non-printables shown as `.`. The byte count is capped at `0x1000` (4096) per call; for larger reads, loop from the calling side using the last returned address plus its row width.

**Syntax**

```text
mem <bank> <addr> <count>
```

- `<bank>` is the CPU bank.
- `<addr>` is the 16-bit start address.
- `<count>` is the number of bytes to read; maximum `1000` (hex).

**Example**

```text
x16db > mem 00 0200 20
   00:0200  20 04 c0 00  00 00 00 00  00 00 00 00  00 00 00 00  . ..............
   00:0210  00 00 00 00  00 00 00 00  00 00 00 00  00 00 00 00  ................
RDY
```

**Notes**

The output format matches `m` exactly, so dumps from either command can be diffed line-by-line.

**Associated commands**: [`m`](#m), [`wmm`](#wmm), [`vmr`](#vmr), [`dis`](#dis)

[^ Index](#index)

---

### `wmm`

**Purpose**

`wmm` writes one or more bytes to RAM at an explicit bank and address, directly to the underlying RAM array. The writes bypass the CPU's write path, so any address in the I/O region (`$9F00-$9FFF`) can be poked without triggering peripheral side effects. For writes that should go through the CPU's write path and be observed by I/O hardware, use [`fil`](#fil).

**Syntax**

```text
wmm <bank> <addr> <hex>...
```

- `<bank>` is the CPU bank.
- `<addr>` is the start address.
- `<hex>...` is one or more single-byte hex values, written consecutively starting at `<addr>`.

**Example**

```text
x16db > wmm 00 0200 de ad be ef
RDY
x16db > mem 00 0200 4
   00:0200  de ad be ef                                          ....
RDY
```

**Notes**

Each hex argument is one byte. To write a 16-bit value, write its two bytes separately in the appropriate order: `wmm 00 0200 ef be` writes `$BEEF` little-endian.

**Associated commands**: [`fil`](#fil), [`f`](#f), [`mem`](#mem), [`vmw`](#vmw)

[^ Index](#index)

---

### `fil`

**Purpose**

`fil` fills a range of RAM with a single byte value, routing each write through the CPU's `write6502` path. Writes into the I/O region (`$9F00-$9FFF`) reach the emulated peripherals just as if the CPU itself had executed an `STA` instruction at each address. Use `fil` when you want those side effects; for direct RAM-array writes that bypass I/O, use [`f`](#f) or [`wmm`](#wmm).

**Syntax**

```text
fil <bank> <addr> <val> [<count>]
```

- `<bank>` is the CPU bank.
- `<addr>` is the start address.
- `<val>` is the byte to write at each position.
- `<count>` is the number of bytes to write (default `1`).

**Example**

```text
x16db > fil 00 9f25 02
RDY
```

**Notes**

Because writes go through the CPU's path, side effects can occur: writing to VERA's data port advances its address pointer, writing to a banking register changes the CPU's bank selection, and so on. The command is useful precisely when those side effects are wanted, and confusing when they are not.

**Associated commands**: [`wmm`](#wmm), [`f`](#f), [`mem`](#mem)

[^ Index](#index)

---

### `find`

**Purpose**

`find` searches a contiguous range of RAM for a byte pattern. Each match prints one line carrying the 16-bit start address of the match; after all matches have been emitted (or none, when the pattern is absent), the command finishes with `RDY`. The search runs from `<start>` to `<start>+<len>-<patlen>` inclusive and reports overlapping matches separately. Patterns are 1 to 16 bytes.

**Syntax**

```text
find <bank> <start> <len> <byte>...
```

- `<bank>` is the CPU bank.
- `<start>` is the start of the search range.
- `<len>` is the length of the range in bytes.
- `<byte>...` is one or more pattern bytes.

**Example**

```text
x16db > find 00 0000 8000 de ad
0200
3000
RDY
```

**Notes**

Only matches within the named bank are reported. To search across multiple banks, repeat the call for each one.

**Associated commands**: [`mem`](#mem), [`m`](#m)

[^ Index](#index)

---

### `vmr`

**Purpose**

`vmr` reads a contiguous block of VRAM and prints it in the same hex+ASCII row format as [`v`](#v), but without moving the view cursor. The address column is 5 hex digits wide because VRAM is a 17-bit address space. The byte count is capped at `0x1000` per call.

**Syntax**

```text
vmr <addr> <count>
```

- `<addr>` is the 17-bit VRAM address (`00000`-`1ffff`).
- `<count>` is the number of bytes to read; maximum `1000` (hex).

**Example**

```text
x16db > vmr 0 20
   00000  10 20 30 40  50 60 70 80  90 a0 b0 c0  d0 e0 f0 00  . 0@P`p.........
   00010  ...
RDY
```

**Associated commands**: [`v`](#v), [`vmw`](#vmw), [`mem`](#mem)

[^ Index](#index)

---

### `vmw`

**Purpose**

`vmw` writes one or more bytes to VRAM at an explicit address, directly into the VRAM buffer. The writes bypass VERA's own data-port mechanism, so they do not advance VERA's address registers and do not interact with VERA's other state. That makes `vmw` useful for laying down sprites, tile patterns, or test bitmaps during debugging without having to drive VERA's I/O registers in sequence.

**Syntax**

```text
vmw <addr> <hex>...
```

- `<addr>` is the 17-bit VRAM address.
- `<hex>...` is one or more single-byte hex values, written consecutively.

**Example**

```text
x16db > vmw 00000 de ad be ef
RDY
x16db > vmr 0 4
   00000  de ad be ef                                          ....
RDY
```

**Associated commands**: [`vmr`](#vmr), [`v`](#v), [`wmm`](#wmm), [`f`](#f)

[^ Index](#index)

---

### `dis`

**Purpose**

`dis` disassembles `<count>` instructions starting at an explicit bank and address, without moving the disasm view cursor. This is the stateless counterpart to [`d`](#d), suitable for scripted operations or one-off inspections that should leave the cursor where it was. The count cap is `0x40` (64) instructions per call.

**Syntax**

```text
dis <bank> <addr> <count>
```

- `<bank>` is the CPU bank.
- `<addr>` is the 16-bit start address.
- `<count>` is the number of instructions to disassemble; maximum `40` (hex).

**Example**

```text
x16db > dis 00 c010 4
00:c010 ad 00 02     lda $0200
00:c013 85 03        sta $03
00:c015 4c 30 c0     jmp $c030
00:c018 ea           nop
RDY
```

**Associated commands**: [`d`](#d), [`mem`](#mem)

[^ Index](#index)

## Inspection

The six commands in this section report machine state at the moment of the call. The CPU registers, the top of the stack, the cc65 zero-page R0..R15 pairs, VERA's internal state, and the cycle counter are each reported on a single fixed-format line (or one line per item, for `stk` and `zpr`), so the output is easy to parse from a script.

---

### `reg`

**Purpose**

`reg` dumps the full CPU register state on a single line in fixed `key=value` form. The order and field names are stable across calls; the leading `mode=` field (`c02` or `c816`) tells the reader which subset of registers to interpret. The 65C816-only registers (`b`, `c`, `db`, `dp`, `e`) appear in the line on 65C02 builds too, but their values are not architecturally meaningful there. The currently-selected RAM and ROM banks (`ram=` and `rom=`) are included, so the dump captures everything needed to reconstruct the CPU's view of the memory map.

**Syntax**

```text
reg
```

**Example**

```text
x16db > reg
mode=c02 pc=c012 a=07 b=00 c=0007 x=0000 y=0000 sp=01fd p=34 k=00 db=00 dp=0000 e=1 ram=00 rom=00
RDY
```

**Notes**

The output is the single-line snapshot. The multi-line bracketed header that appears between prompts is generated separately and controlled by [`hdr`](#hdr). To set a register, see [`r`](#r) or [`srg`](#srg).

**Associated commands**: [`r`](#r), [`srg`](#srg), [`mod`](#mod), [`hdr`](#hdr)

[^ Index](#index)

---

### `srg`

**Purpose**

`srg` sets a single CPU register. It is the explicit-arguments form of [`r`](#r); the two have identical effect, and which one to use is a matter of preference: `srg` reads as "set register" and pairs naturally with `reg`, while `r` mirrors the SDL UI's register-edit shortcut. The full register set is settable on either: `pc`, `a`, `b`, `c`, `x`, `y`, `sp`, `p`, `k`, `db`, `dp`, `e`.

**Syntax**

```text
srg <name> <hex>
```

- `<name>` is one of the register names above.
- `<hex>` is the value to write; the width is implied by the register.

**Example**

```text
x16db > srg pc c100
RDY
x16db > srg a 42
RDY
```

**Notes**

Setting `pc` does not transition the machine state. To resume execution from the new PC, follow with [`cnt`](#cnt).

**Associated commands**: [`r`](#r), [`reg`](#reg)

[^ Index](#index)

---

### `stk`

**Purpose**

`stk` reports the top of the 6502 stack: the bytes most recently pushed by the CPU. The output is one line per byte in the form `<addr>: <value>`, starting one above the current stack pointer (the most recent push) and reading upward, wrapping at page boundaries the same way the CPU itself does. The default count is `10` (16 decimal); the maximum is `40` (64 decimal).

**Syntax**

```text
stk
stk <count>
```

- `<count>` is the number of bytes to report (hex), default `10`, maximum `40`.

**Example**

```text
x16db > stk 8
01fe: 12
01ff: 34
0100: 00
0101: 00
0102: ab
0103: cd
0104: ef
0105: 01
RDY
```

**Notes**

The same eight-byte preview appears as `stk=` on line 2 of the header. `stk` is for fetching a wider window than the header shows, or for fetching the values as a parseable list.

**Associated commands**: [`reg`](#reg), [`mem`](#mem), [`hdr`](#hdr)

[^ Index](#index)

---

### `zpr`

**Purpose**

`zpr` reports the zero-page R0..R15 register pairs as 16 lines of `R<n>  <hex>`. Each pseudo-register is two consecutive bytes of memory interpreted as a 16-bit little-endian word: `R0` is `DP+2`/`DP+3`, `R1` is `DP+4`/`DP+5`, and so on up to `R15` at `DP+0x20`/`DP+0x21`. On 65C02 builds the direct-page (`DP`) is always zero, so the addresses are `$02` through `$21`. These are the C-runtime pseudo-registers used by `cc65` and CMDR-DOS; `zpr` is a faster way to read them than running 16 separate `mem` calls.

**Syntax**

```text
zpr
```

**Example**

```text
x16db > zpr
R0  1234
R1  abcd
R2  0000
R3  0000
R4  0080
R5  0000
R6  0000
R7  0000
R8  0000
R9  0000
R10 0000
R11 0000
R12 0000
R13 0000
R14 0000
R15 0000
RDY
```

**Associated commands**: [`mem`](#mem), [`reg`](#reg)

[^ Index](#index)

---

### `vrg`

**Purpose**

`vrg` reports a snapshot of VERA's current state on a single line, in the same `key=value` form as [`reg`](#reg). The fields cover VERA's two address pointers, the two data port latches, the control register, the video mode, horizontal and vertical scaling, the FX engine's control and multiplier, the FX cache, and the FX accumulator. The field order is fixed.

**Syntax**

```text
vrg
```

**Example**

```text
x16db > vrg
addr0=0c000 addr1=00000 data0=ff data1=00 ctrl=00 video=01 hscale=80 vscale=80 fxctl=00 fxmul=00 cache=00000000 accum=00000000
RDY
```

**Notes**

This is VERA's internal state, not a memory dump of VRAM. To read VRAM contents, use [`vmr`](#vmr) (stateless) or [`v`](#v) (with the view cursor).

**Associated commands**: [`v`](#v), [`vmr`](#vmr)

[^ Index](#index)

---

### `clk`

**Purpose**

`clk` reports the number of CPU clock cycles elapsed since the last resume. The counter is reset on every transition from STOP into RUN (any `cnt` or `sov`-on-call), so the value reflects how long the CPU has been running on the current execution segment. In STOP, it reports the total length of the just-finished run.

**Syntax**

```text
clk
```

**Example**

```text
x16db > clk
clocks=14502
RDY
```

**Notes**

The same value is shown as `clk=` on line 2 of the header. `clk` exists for callers that want the count as a standalone parseable line rather than embedded in a header.

**Associated commands**: [`reg`](#reg), [`hdr`](#hdr)

[^ Index](#index)

## Header and status

The two commands here control and consolidate the live machine-state display. `hdr` configures which lines of the per-prompt header appear; `st` produces a one-shot dump of everything the header and the view cursor are tracking. See [The header](debug_repl.md#the-header) for what each header line contains and when it is emitted.

---

### `hdr`

**Purpose**

`hdr` controls which lines of the per-prompt header are visible, and reports the current setting when called with no arguments. The header has four lines (`cpu`, `aux`, `view`, `bp`) and each one can be toggled independently, all together, or addressed by line number (`1` through `4`). The bare `hdr` form reports the current state of all four lines, with each report-line formatted as a copy-paste-valid setter command, so the output can be saved and replayed to restore the header configuration later.

**Syntax**

```text
hdr
hdr on | off
hdr <name>|<num> on | off
```

- `<name>` is `cpu`, `aux`, `view`, or `bp`.
- `<num>` is `1`, `2`, `3`, or `4` (same line ordering as the names).

**Example** (report current state)

```text
x16db > hdr
hdr cpu  on
hdr aux  on
hdr view on
hdr bp   on
RDY
```

**Example** (suppress all header lines)

```text
x16db > hdr off
RDY
```

**Example** (turn off just the auxiliary line)

```text
x16db > hdr aux off
RDY
```

**Notes**

Turning off the `bp` line (line 4) is rarely needed because that line only appears when a breakpoint is active. The other three lines emit unconditionally unless suppressed.

**Associated commands**: [`st`](#st), [`reg`](#reg), [`clk`](#clk)

[^ Index](#index)

---

### `st`

**Purpose**

`st` emits a one-shot dump of the debugger's internal state: the current machine mode, every piece of the view cursor (`view_pc`, `view_data`, `view_bank`, `view_mode`), the CPU's actual program counter (`regs.pc`), the cycles-since-resume counter, and any active breakpoints (one `bp` row each, or `(none)`). It is a single command that collects what would otherwise need [`mod`](#mod), [`reg`](#reg), [`clk`](#clk), and [`lbp`](#lbp) in sequence, formatted with labeled rows rather than `key=value` lines so it reads as a snapshot rather than as a parseable record.

**Syntax**

```text
st
```

**Example**

```text
x16db > st
mode      stop
view_pc   00:c012  x16bank=-1
view_data 00:0200
view_bank -1
view_mode RAM
regs.pc   00:c012
clk       14502
bp        00:c010  x16bank=-1
RDY
```

**Notes**

`view_bank` of `-1` means "follow the CPU's currently-selected bank"; any other value is the pinned view-bank override. `bp` shows `(none)` when no breakpoint is set. `view_pc` and `regs.pc` are usually equal because the disasm cursor auto-snaps on every transition into STOP; they differ only after a manual `d <addr>` navigation away from the CPU's position.

**Associated commands**: [`mod`](#mod), [`reg`](#reg), [`clk`](#clk), [`lbp`](#lbp), [`hdr`](#hdr)

[^ Index](#index)

## Session

The five commands in this section relate to the debugger session itself: querying the current machine mode, the wire-protocol version, the inline help text, and closing the session with one of two exit codes.

---

### `mod`

**Purpose**

`mod` reports the current machine mode and program counter on a single line of the form `mode=<mode> pc=<bank>:<addr>`. The mode is one of `run`, `step`, or `stop`. The PC is `view_pc` in STOP (which equals `regs.pc` after the auto-snap that fires on every entry into STOP) and `regs.pc` sampled at the call site in RUN.

**Syntax**

```text
mod
```

**Example**

```text
x16db > mod
mode=stop pc=00:c012
RDY
```

**Associated commands**: [`reg`](#reg), [`st`](#st)

[^ Index](#index)

---

### `ver`

**Purpose**

`ver` reports the wire-protocol version of the debugger shell on a single line of the form `proto=<n>`. The number is a positive integer that increments only when a protocol-breaking change is made: a renamed command, a changed response format, or a removed feature. Additions that preserve compatibility (new commands, new optional arguments) do not bump it.

**Syntax**

```text
ver
```

**Example**

```text
x16db > ver
proto=1
RDY
```

**Associated commands**: [`hlp`](#hlp)

[^ Index](#index)

---

### `hlp`

**Purpose**

`hlp` prints a categorised list of every command available at the prompt, one line per command, followed by `RDY`. It is the inline counterpart of the present reference document. The aliases `h`, `?`, and `help` are accepted.

**Syntax**

```text
hlp
h
?
help
```

**Example** (truncated)

```text
x16db > hlp
execution control:
  brk                                 force-break into the debugger
  cnt | c                             continue (resume CPU)
  ...
RDY
```

**Notes**

The help text is generated from the same source as the dispatcher's command table, so it stays in step with the build.

**Associated commands**: [`ver`](#ver)

[^ Index](#index)

---

### `quit`

**Purpose**

`quit` ends the debugger session and the emulator process with exit code `0`. The shell sends `RDY` and then closes; any output that arrives after `RDY` is the emulator's normal shutdown noise. `qit` is accepted as a short alias.

For an exit with a different return code, see [`bail`](#bail).

**Syntax**

```text
quit
qit
```

**Example**

```text
x16db > quit
RDY
```

**Notes**

The emulator does not write any NVRAM, save game, or runtime state to disk before exiting; the session ends as if the process had received `SIGTERM`.

**Associated commands**: [`bail`](#bail)

[^ Index](#index)

---

### `bail`

**Purpose**

`bail` ends the session with exit code `1`. It is identical to [`quit`](#quit) in every other respect; the only difference is the value the process returns to its parent when it exits.

**Syntax**

```text
bail
```

**Example**

```text
x16db > bail
RDY
```

**Associated commands**: [`quit`](#quit)

[^ Index](#index)
