# X16 emulator stdio REPL debugger

## Introduction

The Commander X16 emulator includes an interactive debugger that can pause the running machine and inspect any aspect of it from the CPU  state, RAM and BRAM, to the VRAM and video chip to machine state such as registers, breakpoints and execution steps. Breakpoints can be set to stop the CPU at any address and single-step one instruction at a time.

The same debugger drives both the SDL graphical UI (`-debug`) and the stdio shell (`-debugstdio`).

The stdio shell is a basic interactive shell for which debug commands can be entered and machine state interrogated, with the following sections providing more detailed instructions.

- [Launching](#launching) - how to start the stdio REPL debugger.
- [Machine states](#machine-states) - the three states the emulator can be in.
- [The debugger shell](#the-debugger-shell) - command-response interactions and the per-prompt header.
- [The view cursor](#the-view-cursor) - the shared state that `m`, `d`, and `v` operate on.
- [Asynchronous events](#asynchronous-events) - the `*` lines that arrive between commands.
- [Examples](#examples) - worked transcripts of common flows.
- [Limitations](#limitations) - known caveats and gaps.
- [Command reference](debug_repl_commands.md) - per-command syntax, examples, and notes for everything available at the prompt.

## Launching

To launch the Commander X16 Emulator in stdio debugger mode use the `-debugstdio` flag along with the normal command line invocation:

```bash
x16emu -rom rom.bin -debugstdio
```

`-debugstdio` runs in headless mode so no user interface window will open and no audio device initialises.

An optional hex argument can be specified after `-debugstdio` to set a breakpoint at that address, the same way `-debug` accepts one:

```bash
x16emu -rom rom.bin -debugstdio c100
```

Notes:

- `-debugstdio` and `-debug` are _mutually exclusive_ and should not be specified together. A command line that asks for both is rejected at startup.

- `-echo`, `-trace`, and `-log K/S/V`, if specified together with `-debugstdio`, are ignored and a warning is written to stderr. These flags write to stdout and would interleave with the stdio debugger's output.

Once started the stdio debugger prints a banner, an information header, and then the debugger prompt `x16db >`.

```text
Commander X16 Emulator r50 (next), <git-rev>
(C)2019, 2023 Michael Steil et al.
All rights reserved. License: 2-clause BSD

1: [ 00:e000 a9 00     lda #$00         ] [ A=00 X=00 Y=00 S=01fd NV-BDIZC=00000100 ]
2: [ clk=0 stk=01fe:00 00 00 00 00 00 00 00 ]
3: [ view: pc=00:e000 d=00:0000 b=- RAM ]
x16db > 
```

## Machine states

The X16 emulator is always in one of three states:

| State | Description |
| --- | --- |
| **STOP** | The CPU is paused. Reached after `brk`, after a breakpoint is hit, after the CPU executes a `STP` opcode, or after a step completes. |
| **RUN** | The CPU is running freely. The debug shell still accepts commands; they are processed between CPU instructions. |
| **STEP** | A transient state entered by `stp` to single-step the CPU. One instruction executes, then the emulator drops to STOP and emits `* BRK STEP <bank>:<addr>`. The prompt is held during STEP, so the next prompt appears already in STOP. |

When started wtih the  `-debugstdio` flag, unless set explicicly with a break point, the machine will be in a RUN state, even when in the debugger shell.  The debugger shell is described later but, the current state is reported by the `mod` command from the debug shell. 

## The debugger shell

The shell is line-based ASCII. Commands are entered one at a time at the `x16db >` command prompt. The full set of commands, with syntax and examples, lives in the [command reference](debug_repl_commands.md); this section covers only how a command-response interaction looks.

On entering a command the debugger will respond with:

| Element | Meaning |
| --- | --- |
| `<data>` | A line of free-form data in response to the command. |
| `RDY` | The previous command finished without error. |
| `ERR <message>` | The previous command failed. |
| `* <event>` | An asynchronous event from the emulator (breakpoint hit, single-step completed, resume, and so on). |
| `N: [ ... ]` | A [header line](#the-header) showing the machine state. Which lines to display is configurable. |

and then `x16db >` indicating the emulator is ready for the next command.

### The header

Before every prompt the emulator emits a multi-line formatted header showing the current machine state.  As an example:

```text
1: [ 00:c012 85 01     sta $01          ] [ A=07 X=00 Y=00 S=01fd NV-BDIZC=00110100 ]
2: [ clk=2 stk=01fe:e6 2b 92 e8 04 43 e0 35 ]
3: [ view: pc=00:c012 d=00:0000 b=- RAM ]
4: [ bp=00:c010 ]
x16db > 
```

where each header line is:

1. The instruction at the current CPU program counter (PC) and register state:
   1. Bank, address, raw bytes, and disassembly.
   2. A, X, Y, S and the status flag bits. On 65C816 the layout widens to include B, C, D, DB, and E.

2. Cycles since the last resume (`clk=`) and the top eight stack bytes (`stk=<address>:<bytes>`).

3. The view cursor (see below): disasm position (`pc=`), data position (`d=`), pinned view bank (`b=`), and current mode (RAM or VRAM).

4. The active breakpoint addresses (`bp=<bank>:<addr> ...`, space-separated). Absent when no breakpoint is set.

Because the header is output at every prompt, stepping through code produces a running snapshot of the instruction stream and the registers. 

Header lines can be suppressed individually or together with the `hdr` command.

## The view cursor

The debugger remembers where the last navigation left off. That state, the *view cursor*, is what `m`, `d`, and `v` re-emit from when called with no arguments. It is independent of the CPU's own state: where the CPU's program counter is, and which banks the CPU has selected, are not the same thing as where the user is looking.

Four pieces of cursor state appear on line 3 of the header:

- **Disasm cursor** (`pc=` in the header). Where the next disassembly starts.
- **Data cursor** (`d=`). Where the next memory dump starts.
- **View bank** (`b=`). Which X16 RAM or ROM bank the data dump shows in the $A000–$FFFF window. A `-` means "follow the CPU's currently-selected bank".
- **Mode** (`RAM` or `VRAM`). Which address space the data cursor refers to.

### Moving the cursor

Each piece of the cursor has commands that set it absolutely and commands that nudge it relative to its current value:

| Command | Effect |
| --- | --- |
| `m <addr>` | Data cursor to absolute address (RAM mode). |
| `m <bank>:<addr>` | Data cursor + pin view bank. |
| `m +`, `m -`, `m +<off>`, `m -<off>` | Nudge data cursor (default step 0x100). |
| `v <addr>` | Data cursor to absolute address (VRAM mode). |
| `v +`, `v -`, `v +<off>`, `v -<off>` | Nudge data cursor in VRAM (default step 0x200). |
| `d <addr>` | Disasm cursor to absolute address. |
| `d +`, `d -`, `d +<off>`, `d -<off>` | Nudge disasm cursor (default step 0x10). |
| `b view <bank>` | Set the view bank explicitly. |
| `b view +`, `b view -` | Nudge the view bank by 1. |
| `b view follow` | Reset the view bank to follow the CPU. |
| `home` | Snap the disasm cursor to the CPU's current PC. |

`m`, `d`, and `v` re-emit a dump after moving the cursor. The `b view` commands change state without dumping; type `m` afterward to see the new view.

### Auto-snap on break

The disasm cursor is the only piece that moves on its own. Whenever the CPU transitions to STOP (a breakpoint fires, a step completes, or the user types `brk`), `pc=` is set to the CPU's current program counter. After a break, the next instruction the CPU is about to execute is almost always what the user wants to inspect.

The `home` command is the manual version of this snap, useful after navigating away with `d <addr>`.

### The view-bank override

The X16 maps a RAM bank or a ROM bank into the $A000–$FFFF window of CPU address space; the CPU's $00 and $01 registers select which. The view bank is an override for that mapping inside the data dump: `m 5:a000` reads bank 5 at $A000 regardless of which bank the CPU currently has selected, and the CPU's bank registers are not touched. Subsequent bare `m` commands continue to show bank 5 until `b view follow` resets to "follow CPU".

This is the typical reason for setting a view bank: peek at code or data in a bank the CPU is not currently executing from, without disturbing CPU state.

### Mode

The mode is which address space the data cursor refers to. `m` selects RAM; `v` selects VRAM. The cursor address itself is shared between modes: `v 0` after `m 1000` jumps to VRAM address $0, not back to $1000.

## Asynchronous events

Some state changes happen without a command being entered: a breakpoint fires, a running CPU executes a `STP` opcode, a step completes. The shell prints these on their own line, prefixed with `*`, between the previous response and the next prompt. They never interleave inside another command's output.

### `* BRK <reason> <bank> <addr>`

The emulator entered STOP. `<reason>` is one of:

| Reason | Triggered by |
| --- | --- |
| `USER` | `brk` command, or F12 in the SDL UI |
| `BREAKPOINT` | The active user breakpoint was hit |
| `STP` | The CPU executed the `STP` opcode (`$DB`) |
| `STEP` | A `stp` or `sov` step completed |

```text
* BRK USER 00 c010
* BRK BREAKPOINT 00 080d
* BRK STP 00 0203
* BRK STEP 00 c012
```

### `* RES`

The emulator left STOP for RUN. Fired by `cnt`, and by `sov` when the instruction stepped over is a call.

### `* BP SET <bank>:<addr>` / `* BP CLEAR <bank>:<addr>`

A breakpoint was toggled by `tb` (toggle at the view cursor's disasm position). `sbp` and `cbp` change breakpoint state silently; they print `RDY` only.

## Examples

The transcripts below sometimes include the per-prompt header lines and sometimes omit them for brevity. In a live session the header can be turned off with `hdr off`.

### Set a breakpoint, run, single-step

```text
x16db > sbp 00 c010
RDY
x16db > cnt
RDY
* BRK BREAKPOINT 00 c010
x16db > stp
RDY
* BRK STEP 00 c013
```

`sbp` arms a breakpoint at `00:c010`. `cnt` resumes the CPU and the prompt returns immediately. When the CPU reaches `c010` it stops on its own and the `* BRK BREAKPOINT` line fires. `stp` single-steps one instruction; the CPU lands on `c013` (the instruction at `c010` was three bytes wide). After each break, the disasm cursor auto-snaps to the new PC.

### Break a running program, inspect memory, step through it

The CPU has been running freely since launch. The header shows the CPU mid-execution; arm a breakpoint at `$C010`:

```text
1: [ 00:c024 a5 00     lda $00          ] [ A=03 X=00 Y=00 S=01fd NV-BDIZC=00110100 ]
2: [ clk=8472 stk=01fe:00 00 00 00 00 00 00 00 ]
3: [ view: pc=00:c024 d=00:0000 b=- RAM ]
x16db > sbp 00 c010
RDY
```

The prompt returns immediately and the CPU keeps running. The next prompt's header now shows the breakpoint on line 4. The CPU is still somewhere mid-program:

```text
1: [ 00:c084 c9 ff     cmp #$ff         ] [ A=05 X=00 Y=00 S=01fd NV-BDIZC=10110100 ]
2: [ clk=12331 stk=01fe:00 00 00 00 00 00 00 00 ]
3: [ view: pc=00:c024 d=00:0000 b=- RAM ]
4: [ bp=00:c010 ]
x16db > 
```

When the CPU reaches `$C010` it stops on its own. The async event fires, and the next header reflects the stopped state at `c010`; the disasm cursor (`pc=` on line 3) has auto-snapped to the new PC. Inspect a page of RAM at `$0200`:

```text
* BRK BREAKPOINT 00 c010
1: [ 00:c010 ad 00 02  lda $0200        ] [ A=05 X=00 Y=00 S=01fd NV-BDIZC=00110100 ]
2: [ clk=14502 stk=01fe:00 00 00 00 00 00 00 00 ]
3: [ view: pc=00:c010 d=00:0000 b=- RAM ]
4: [ bp=00:c010 ]
x16db > m 0200
   00:0200  20 04 c0 00  00 00 00 00  00 00 00 00  00 00 00 00  . ..............
   ...
RDY
```

The data cursor (`d=`) is now `00:0200`. Step one instruction:

```text
1: [ 00:c010 ad 00 02  lda $0200        ] [ A=05 X=00 Y=00 S=01fd NV-BDIZC=00110100 ]
2: [ clk=14502 stk=01fe:00 00 00 00 00 00 00 00 ]
3: [ view: pc=00:c010 d=00:0200 b=- RAM ]
4: [ bp=00:c010 ]
x16db > stp
RDY
* BRK STEP 00 c013
```

PC has moved from `c010` to `c013`; the `lda $0200` was three bytes wide. `A` is now `$20`, the byte the load fetched. Step again:

```text
1: [ 00:c013 85 03     sta $03          ] [ A=20 X=00 Y=00 S=01fd NV-BDIZC=00110100 ]
2: [ clk=14506 stk=01fe:00 00 00 00 00 00 00 00 ]
3: [ view: pc=00:c013 d=00:0200 b=- RAM ]
4: [ bp=00:c010 ]
x16db > stp
RDY
* BRK STEP 00 c015
```

### Peek at a banked RAM region without disturbing the CPU

```text
x16db > m 5:a000
   05:a000  00 01 02 03  04 05 06 07  08 09 0a 0b  0c 0d 0e 0f  ................
   ...
RDY
x16db > m +
   05:a100  ...
RDY
x16db > b view follow
RDY
x16db > m
   00:a100  ...
RDY
```

`m 5:a000` moves the data cursor to `$A000` and pins the view bank to `5`. Bare `m` and `m +` continue to read from bank 5 without ever writing to the CPU's bank registers (`$00` / `$01`). `b view follow` clears the override; the next bare `m` reads from whichever bank the running program has selected.

## Limitations

### Platform support

Linux, macOS, and Windows are implemented. WebAssembly compiles, but the `-debugstdio` frontend is a build-time no-op there because the browser has no stdin to read from. 

### CPU sampling during RUN

Inspection commands such as `reg`, `mem`, `stk`, and `vrg` work while the CPU is running, but the values they return are sampled at the instruction boundary the debugger's tick handler happens to land on. The samples are well-defined (the CPU is between instructions, not mid-execution) but slightly stale by the time they reach the prompt. For a precise snapshot, `brk` first and then read.

### The `STP` opcode halts the CPU

Executing the 65C02 `STP` instruction (opcode `$DB`, "stop the clock") halts the CPU. It breaks into the debugger with `* BRK STP <bank> <addr>` and leaves the program counter parked on the instruction. Because the CPU cannot advance past `STP` on its own, resuming with `cnt` re-executes it and breaks again, as does single-stepping (note: the debugger command `stp` is "step", unrelated to the CPU opcode `STP`). To continue, repoint the program counter past it (`r pc <addr>`) before `cnt`, or reset the CPU with `rst`.
