#!/usr/bin/env python3
"""x16dbg_smoke.py -- exercise every command of the -debugstdio protocol.

A small smoke test for the stdio debugger REPL. Spawns x16emu under SDL's
dummy video driver, pipes commands through stdin, and checks responses.

Locates the emulator and ROM via env vars or a small search path:
  X16EMU_PATH   path to x16emu binary    (default: ./build/x16emu, ../build/x16emu, ./x16emu, ../x16emu)
  X16ROM_PATH   path to rom.bin          (default: ~/.local/share/x16emu/rom.bin, ./rom.bin, ../rom.bin)

Exits 0 if all checks pass, 1 otherwise.

Not a replacement for an external typed Python client -- that lives in its
own project. This script is a regression baseline + a reference example of
how to drive the protocol.
"""

import os
import re
import select
import subprocess
import sys
import time

PROMPT = b"x16db > "
HEADER_RE = re.compile(r"^[1-4]: \[")


def find_emulator():
	p = os.environ.get("X16EMU_PATH")
	if p and os.path.exists(p):
		return os.path.abspath(p)
	for cand in ("build/x16emu", "../build/x16emu", "x16emu", "../x16emu"):
		if os.path.exists(cand):
			return os.path.abspath(cand)
	raise SystemExit("Cannot find x16emu. Set X16EMU_PATH or build the emulator.")


def find_rom():
	p = os.environ.get("X16ROM_PATH")
	if p and os.path.exists(p):
		return os.path.abspath(p)
	for cand in (
		os.path.expanduser("~/.local/share/x16emu/rom.bin"),
		"rom.bin",
		"../rom.bin",
	):
		if os.path.exists(cand):
			return os.path.abspath(cand)
	raise SystemExit("Cannot find rom.bin. Set X16ROM_PATH.")


class X16dbg:
	"""Minimal client. Synchronous send / read-until-RDY-or-ERR; events read separately."""

	def __init__(self, emulator, rom):
		env = os.environ.copy()
		env["SDL_VIDEODRIVER"] = "dummy"
		env["SDL_AUDIODRIVER"] = "dummy"
		self.proc = subprocess.Popen(
			[emulator, "-rom", rom, "-debugstdio", "-warp"],
			stdin=subprocess.PIPE,
			stdout=subprocess.PIPE,
			stderr=subprocess.PIPE,
			env=env,
			bufsize=0,
		)
		self.last_header = []
		# The emulator emits a banner on stdout then a "> " prompt. Read
		# everything up through that initial prompt -- it's our handshake.
		self._await_prompt()

	def _await_prompt(self, timeout=2.0):
		"""Read banner + initial prompt and discard."""
		self._read_until_prompt_bytes(timeout)

	def __enter__(self):
		return self

	def __exit__(self, *_):
		self.close()

	def close(self):
		if self.proc.poll() is None:
			try:
				self.proc.stdin.write(b"quit\n")
				self.proc.stdin.flush()
			except OSError:
				pass
		try:
			self.proc.wait(timeout=2)
		except subprocess.TimeoutExpired:
			self.proc.kill()
			self.proc.wait()

	def _read_until_prompt_bytes(self, timeout=2.0):
		"""Read stdout bytes until they end with PROMPT.

		The prompt has no trailing newline so we can't use readline. Read
		chunks until the buffer ends with the marker, then return the
		body (everything before the prompt) as a decoded string.
		"""
		buf = b""
		deadline = time.time() + timeout
		while not buf.endswith(PROMPT):
			remaining = deadline - time.time()
			if remaining <= 0:
				raise TimeoutError(f"no prompt; buf={buf!r}")
			ready, _, _ = select.select(
				[self.proc.stdout], [], [], min(0.05, remaining)
			)
			if not ready:
				continue
			chunk = os.read(self.proc.stdout.fileno(), 4096)
			if not chunk:
				raise EOFError(f"emulator EOF before prompt; buf={buf!r}")
			buf += chunk
		return buf[: -len(PROMPT)].decode("ascii", errors="replace")

	def send(self, line):
		self.proc.stdin.write((line + "\n").encode("ascii"))
		self.proc.stdin.flush()

	def _parse_response(self, body):
		"""Split a prompt-terminated body into (data, events, header, response).

		Header lines (`^[1-4]: \\[…\\]`) are separated from data lines so
		assertions on command output don't trip on the header that
		precedes every prompt.
		"""
		data, events, header, response = [], [], [], None
		for ln in body.split("\n"):
			ln = ln.rstrip("\r")
			if ln == "":
				continue
			if ln == "RDY":
				response = "RDY"
			elif ln.startswith("ERR"):
				response = ln
			elif ln.startswith("* "):
				events.append(ln)
			elif HEADER_RE.match(ln):
				header.append(ln)
			else:
				data.append(ln)
		return data, events, header, response

	def cmd(self, line, timeout=2.0):
		"""Send a command and read until the next prompt.

		Returns (data_lines, events). Header lines are silently absorbed
		(self.last_header keeps the most recent set for header-specific
		tests). Raises AssertionError on ERR.
		"""
		self.send(line)
		body = self._read_until_prompt_bytes(timeout)
		data, events, header, response = self._parse_response(body)
		self.last_header = header
		if response is None:
			raise AssertionError(
				f"{line!r}: prompt without RDY/ERR; data={data} events={events}"
			)
		if response.startswith("ERR"):
			raise AssertionError(f"{line!r}: {response}")
		return data, events

	def wait_prompt(self, timeout=2.0):
		"""Wait for the next prompt without sending anything.

		Useful after `cnt` to passively observe async events (e.g. a BP
		hit during free running). Returns (data, events).
		"""
		body = self._read_until_prompt_bytes(timeout)
		data, events, header, _ = self._parse_response(body)
		self.last_header = header
		return data, events


passed = 0
failed = 0


def check(label, cond, detail=""):
	global passed, failed
	if cond:
		passed += 1
		print(f"  OK   {label}")
	else:
		failed += 1
		suffix = (": " + str(detail)) if detail else ""
		print(f"  FAIL {label}{suffix}")


def cnt_until_event(d, prefix, timeout=5.0):
	"""Resume the CPU and collect events until one starts with `prefix`.

	At warp speed a breakpoint or watchpoint can fire and emit its event in
	the same read as `cnt`'s own prompt, so the event may arrive with the
	`cnt` response or asynchronously on the next prompt. Handle both, and
	return without raising on timeout so the caller's check fails cleanly.
	"""
	_, events = d.cmd("cnt")
	if not any(e.startswith(prefix) for e in events):
		try:
			_, extra = d.wait_prompt(timeout=timeout)
			events += extra
		except TimeoutError:
			pass
	return events


def arm(d, code):
	"""Stop the CPU, write a short routine at $0500, and point the PC at it.

	`brk` first so `srg pc` always runs while stopped: setting the PC while
	the CPU is still running races with instruction fetch and is ignored.
	"""
	d.cmd("brk")
	d.cmd("wmm 00 0500 " + code)
	d.cmd("srg pc 0500")


def main():
	emu = find_emulator()
	rom = find_rom()
	print(f"emulator: {emu}")
	print(f"rom:      {rom}")
	print()

	with X16dbg(emu, rom) as d:
		print("--- session ---")
		data, _ = d.cmd("ver")
		check(
			"ver returns proto=N",
			len(data) == 1 and re.match(r"^proto=\d+$", data[0]),
			data,
		)

		# Force a break so we have a known mode for the rest of the script.
		_, events = d.cmd("brk")
		check(
			"brk triggers * BRK USER",
			any(e.startswith("* BRK USER ") for e in events),
			events,
		)

		data, _ = d.cmd("mod")
		check("mod returns mode=stop", any("mode=stop" in l for l in data), data)

		print()
		print("--- registers ---")
		data, _ = d.cmd("reg")
		line = data[0] if data else ""
		check("reg returns one line", len(data) == 1, data)
		check("reg has mode= field", "mode=" in line, line)
		check("reg has pc= field", "pc=" in line, line)
		check("reg has ram= / rom= fields", "ram=" in line and "rom=" in line, line)

		d.cmd("srg a 42")
		data, _ = d.cmd("reg")
		check("srg a 42 then reg shows a=42", " a=42 " in data[0], data[0])

		print()
		print("--- memory (RAM) ---")
		data, _ = d.cmd("mem 00 0400 10")
		check(
			"mem 16 bytes -> one hex+ASCII line",
			len(data) == 1
			and re.match(r"^0400:(?: [0-9a-f]{2}){16}  .{16}$", data[0]),
			data,
		)

		d.cmd("wmm 00 0400 de ad be ef")
		data, _ = d.cmd("mem 00 0400 04")
		check("wmm round-trip (hex column)", data[0].startswith("0400: de ad be ef"), data)

		d.cmd("fil 00 0500 5a 8")
		data, _ = d.cmd("mem 00 0500 08")
		check(
			"fil fills 8 bytes (hex column)",
			data[0].startswith("0500: 5a 5a 5a 5a 5a 5a 5a 5a"),
			data,
		)

		d.cmd("wmm 00 0600 ca fe ba be")
		data, _ = d.cmd("find 00 0000 1000 ca fe")
		check(
			"find locates the pattern",
			"0600" in data,
			data,
		)

		data, _ = d.cmd("find 00 0000 1000 ff ff ff ff ff ff ff ff")
		check("find returns no rows when not found", data == [], data)

		print()
		print("--- VRAM ---")
		d.cmd("vmw 0 11 22 33")
		data, _ = d.cmd("vmr 0 4")
		check(
			"vmw round-trip (3 bytes set + one prior)",
			len(data) == 1 and data[0].startswith("00000: 11 22 33 "),
			data,
		)

		print()
		print("--- disassembly ---")
		d.cmd("wmm 00 0500 ea")  # NOP
		data, _ = d.cmd("dis 00 0500 1")
		check(
			"dis decodes NOP",
			len(data) == 1 and "nop" in data[0].lower(),
			data,
		)

		print()
		print("--- snapshot panels ---")
		data, _ = d.cmd("zpr")
		check(
			"zpr returns 16 R-register lines",
			len(data) == 16 and all(re.match(r"R\s*\d+\s+[0-9a-f]{4}", l) for l in data),
			data,
		)

		data, _ = d.cmd("vrg")
		check(
			"vrg returns one line with VERA fields",
			len(data) == 1 and "addr0=" in data[0] and "accum=" in data[0],
			data,
		)

		data, _ = d.cmd("clk")
		check(
			"clk returns clocks=N",
			len(data) == 1 and re.match(r"^clocks=\d+$", data[0]),
			data,
		)

		data, _ = d.cmd("stk 4")
		check(
			"stk 4 returns 4 lines",
			len(data) == 4 and all(re.match(r"^[0-9a-f]{4}: [0-9a-f]{2}$", l) for l in data),
			data,
		)

		print()
		print("--- breakpoints ---")
		d.cmd("sbp 00 abcd")
		data, _ = d.cmd("lbp")
		check("lbp returns the BP we set", data == ["00: abcd"], data)

		d.cmd("cbp 00 abcd")
		data, _ = d.cmd("lbp")
		check("after cbp, lbp returns no breakpoints", data == [], data)

		print()
		print("--- multiple breakpoints ---")
		d.cmd("sbp 00 1111")
		d.cmd("sbp 00 2222")
		d.cmd("sbp 00 3333")
		data, _ = d.cmd("lbp")
		check("lbp lists all set breakpoints in order",
		      data == ["00: 1111", "00: 2222", "00: 3333"], data)
		d.cmd("sbp 00 2222")  # duplicate
		data, _ = d.cmd("lbp")
		check("re-adding a breakpoint is idempotent",
		      data == ["00: 1111", "00: 2222", "00: 3333"], data)
		d.cmd("cbp 00 2222")  # remove the middle one
		data, _ = d.cmd("lbp")
		check("cbp removes one without disturbing the rest",
		      data == ["00: 1111", "00: 3333"], data)
		try:
			d.cmd("cbp 00 9999")
			check("cbp on a missing breakpoint errors", False, "no ERR")
		except AssertionError:
			check("cbp on a missing breakpoint errors", True)
		d.cmd("cbp *")  # clear all
		data, _ = d.cmd("lbp")
		check("cbp * clears every breakpoint", data == [], data)

		print()
		print("--- execution control ---")
		_, events = d.cmd("cnt")
		check("cnt emits * RES", events == ["* RES"], events)

		time.sleep(0.05)  # let CPU execute a bit
		_, events = d.cmd("brk")
		check(
			"brk after cnt emits * BRK USER",
			any(e.startswith("* BRK USER ") for e in events),
			events,
		)

		_, events = d.cmd("stp")
		check(
			"stp emits * BRK STEP",
			any(e.startswith("* BRK STEP ") for e in events),
			events,
		)

		_, events = d.cmd("sov")
		if events == ["* RES"]:
			# sov-on-JSR: the called routine is now running, step-BP set at
			# the return address. Wait for the eventual step-complete event.
			_, extra = d.wait_prompt(timeout=2.0)
			check(
				"sov-JSR completes via async * BRK STEP",
				any(e.startswith("* BRK STEP ") for e in extra),
				extra,
			)
		else:
			# sov on a non-call instruction: behaves like stp.
			check(
				"sov non-JSR emits * BRK STEP",
				any(e.startswith("* BRK STEP ") for e in events),
				events,
			)

		# Single-letter aliases mirror the canonical 3-letter names.
		_, events = d.cmd("s")          # `s` aliases `stp`
		check(
			"alias 's' steps like 'stp'",
			any(e.startswith("* BRK STEP ") for e in events),
			events,
		)
		_, events = d.cmd("c")          # `c` aliases `cnt`
		check("alias 'c' continues like 'cnt'", events == ["* RES"], events)
		time.sleep(0.02)
		_, events = d.cmd("brk")
		check("brk after c", any(e.startswith("* BRK USER ") for e in events), events)
		# `r` is now the SDL-equivalent set-register command; `reg` is the
		# explicit dump verb.
		d.cmd("r a 7e")
		data, _ = d.cmd("reg")
		check("r <name> <hex> set the register", "a=7e" in data[0], data)

		d.cmd("rst")
		# rst by itself doesn't change mode; we should still be in STOP.
		data, _ = d.cmd("mod")
		check("after rst, still in STOP", any("mode=stop" in l for l in data), data)

		print()
		print("--- watchpoints ---")
		d.cmd("brk")
		d.cmd("cwp *")
		data, _ = d.cmd("swp 00 0070")
		check("swp echoes the assigned id", data == ["wp 0 set"], data)
		data, _ = d.cmd("lwp")
		check("lwp lists the watchpoint", data == ["0: w 00:0070 hits=0"], data)
		d.cmd("swp 00 0080 008f")
		data, _ = d.cmd("lwp")
		check("second watchpoint gets id 1, range shown",
		      data == ["0: w 00:0070 hits=0", "1: w 00:0080-008f hits=0"], data)
		d.cmd("cwp 0")
		data, _ = d.cmd("lwp")
		check("cwp frees a slot without renumbering the rest",
		      data == ["1: w 00:0080-008f hits=0"], data)
		d.cmd("wp 1 off")
		data, _ = d.cmd("lwp")
		check("wp off marks it disabled", data == ["1: w 00:0080-008f hits=0 off"], data)
		d.cmd("wp 1 on")
		d.cmd("swp r 00 0070")
		data, _ = d.cmd("lwp")
		check("read watchpoint accepted and lists as r",
		      "0: r 00:0070 hits=0" in data, data)
		d.cmd("cwp 0")
		try:
			d.cmd("swp 00 0090 if a ==")
			check("malformed watchpoint condition rejected", False, "no ERR")
		except AssertionError:
			check("malformed watchpoint condition rejected", True)
		try:
			d.cmd("cwp 9")
			check("cwp on a missing watchpoint errors", False, "no ERR")
		except AssertionError:
			check("cwp on a missing watchpoint errors", True)
		d.cmd("cwp *")
		data, _ = d.cmd("lwp")
		check("cwp * clears every watchpoint", data == [], data)
		# Fire: inject a routine that writes a known ZP location, point the
		# CPU at it, and run. Deterministic regardless of machine state or
		# host speed -- the watched write happens within two instructions.
		arm(d, "a9 aa 85 70")              # LDA #$aa ; STA $70
		d.cmd("swp 00 0070")
		events = cnt_until_event(d, "* WP 0 w 00:0070=aa")
		check("write watchpoint fires with * WP (id, addr, value, pc)",
		      any(e.startswith("* WP 0 w 00:0070=aa") and "pc=00:0502" in e for e in events),
		      events)
		data, _ = d.cmd("lwp")
		check("watchpoint hit count incremented",
		      data == ["0: w 00:0070 hits=1"], data)
		d.cmd("cwp *")

		print()
		print("--- conditional watchpoints + breakpoints ---")
		# Deterministic routines that stop on their own (no free-running loops
		# or timing): `LDA #$aa ; STA $70` makes the watched write / reaches the
		# breakpoint, and the `... ; STP` variant runs on to a clean STP stop
		# when the condition does not fire.

		# Watchpoint, condition true -> fires.
		arm(d, "a9 aa 85 70")
		d.cmd("swp 00 0070 if val == $aa")
		data, _ = d.cmd("lwp")
		check("lwp shows the watchpoint condition",
		      data == ["0: w 00:0070 hits=0 if val == $aa"], data)
		events = cnt_until_event(d, "* WP 0 w 00:0070=aa")
		check("watchpoint fires when condition is true",
		      any(e.startswith("* WP 0 w 00:0070=aa") for e in events), events)
		d.cmd("cwp *")

		# Watchpoint, condition false -> the write happens but does not fire;
		# the routine runs on to STP.
		arm(d, "a9 aa 85 70 db")
		d.cmd("swp 00 0070 if val == $bb")
		events = cnt_until_event(d, "* BRK STP")
		check("watchpoint does not fire when condition is false",
		      any(e.startswith("* BRK STP") for e in events)
		      and not any(e.startswith("* WP") for e in events), events)
		data, _ = d.cmd("lwp")
		check("false-condition watchpoint has zero hits",
		      data == ["0: w 00:0070 hits=0 if val == $bb"], data)
		d.cmd("cwp *")

		# Conditional breakpoint, condition true -> fires.
		arm(d, "a9 aa 85 70")
		d.cmd("sbp 00 0502 if a == $aa")
		data, _ = d.cmd("lbp")
		check("lbp shows the breakpoint condition",
		      data == ["00: 0502  if a == $aa"], data)
		events = cnt_until_event(d, "* BRK BREAKPOINT 00 0502")
		check("conditional breakpoint fires when condition is true",
		      any(e.startswith("* BRK BREAKPOINT 00 0502") for e in events), events)
		d.cmd("cbp *")

		# Conditional breakpoint, condition false -> does not break; STP stops.
		arm(d, "a9 aa 85 70 db")
		d.cmd("sbp 00 0502 if a == $05")
		events = cnt_until_event(d, "* BRK STP")
		check("conditional breakpoint does not fire when condition is false",
		      any(e.startswith("* BRK STP") for e in events)
		      and not any(e.startswith("* BRK BREAKPOINT") for e in events), events)
		d.cmd("cbp *")

		print()
		print("--- read watchpoints ---")
		# `LDA $0550` reads a watched, otherwise-cold address; the access type
		# is r and the value is the byte read. $0550 is reported at pc 0500
		# because the load is the first instruction after the resume.

		# Read watchpoint fires on a load.
		arm(d, "ad 50 05 db")          # LDA $0550 ; STP
		d.cmd("wmm 00 0550 cc")        # known value at the watched address
		d.cmd("swp r 00 0550")
		events = cnt_until_event(d, "* WP 0 r 00:0550=cc")
		check("read watchpoint fires with * WP (access type r)",
		      any(e.startswith("* WP 0 r 00:0550=cc") and "pc=00:0500" in e for e in events),
		      events)
		d.cmd("cwp *")

		# Read watchpoint, condition false -> does not fire; routine reaches STP.
		arm(d, "ad 50 05 db")
		d.cmd("wmm 00 0550 cc")
		d.cmd("swp r 00 0550 if val == $bb")
		events = cnt_until_event(d, "* BRK STP")
		check("read watchpoint does not fire when condition is false",
		      any(e.startswith("* BRK STP") for e in events)
		      and not any(e.startswith("* WP") for e in events), events)
		d.cmd("cwp *")

		# The is_read / is_write operands reflect the access under test.
		arm(d, "ad 50 05 db")
		d.cmd("wmm 00 0550 cc")
		d.cmd("swp r 00 0550 if is_read && !is_write")
		events = cnt_until_event(d, "* WP 0 r 00:0550=cc")
		check("read watchpoint condition can test is_read / is_write",
		      any(e.startswith("* WP 0 r 00:0550=cc") for e in events), events)
		d.cmd("cwp *")

		print()
		print("--- SDL-equivalent stateful commands ---")
		# `m a300` sets the data cursor; bare `m` re-dumps from it.
		data, _ = d.cmd("m a300")
		check("m <addr> dumps 16 rows", len(data) == 16, len(data))
		check("m <addr> first row is at addr", data[0].startswith("a300:"), data[0])
		data, _ = d.cmd("m")
		check("bare m re-dumps at cursor", data[0].startswith("a300:"), data[0])
		# `m <bank>:<addr>` sets x16bank too.
		d.cmd("m 5:a000")
		data, _ = d.cmd("st")
		check("m <bank>:<addr> sets view_bank", any("view_bank 5" in l for l in data), data)
		# `d <addr>` sets disasm cursor; `home` resyncs to regs.pc.
		d.cmd("d 1000")
		data, _ = d.cmd("st")
		check("d <addr> sets view_pc", any("view_pc   00:1000" in l for l in data), data)
		d.cmd("home")
		data, _ = d.cmd("st")
		check("home resyncs view_pc to regs.pc", not any("view_pc   00:1000" in l for l in data), data)
		# `tb` toggles BP at view_pc; verify with lbp.
		d.cmd("tb")
		data, _ = d.cmd("lbp")
		check("tb sets BP at view_pc", len(data) == 1, data)
		# tb adds alongside an explicit sbp rather than overwriting it.
		d.cmd("sbp 00 abcd")
		data, _ = d.cmd("lbp")
		check("tb breakpoint coexists with an sbp breakpoint",
		      len(data) == 2 and "00: abcd" in data, data)
		d.cmd("cbp 00 abcd")
		d.cmd("tb")
		data, _ = d.cmd("lbp")
		check("tb clears BP when set", data == [], data)

		print()
		print("--- view-cursor nudge ---")
		d.cmd("m 1000")
		data, _ = d.cmd("m +")
		check("m + advances data cursor by 0x100", data[0].startswith("1100:"), data[0])
		data, _ = d.cmd("m +80")
		check("m +<off> advances by explicit offset", data[0].startswith("1180:"), data[0])
		data, _ = d.cmd("m -")
		check("m - retreats data cursor by 0x100", data[0].startswith("1080:"), data[0])
		d.cmd("d 2000")
		data, _ = d.cmd("d +")
		check("d + advances disasm cursor by 0x10", data[0].startswith("00:2010:"), data[0])
		d.cmd("b view follow")
		data, _ = d.cmd("st")
		check("b view follow resets to -1", any("view_bank -1" in l for l in data), data)
		d.cmd("b view 3")
		d.cmd("b view +")
		data, _ = d.cmd("st")
		check("b view + nudges by 1", any("view_bank 4" in l for l in data), data)
		d.cmd("b view follow")  # reset for the next section

		print()
		print("--- header + hdr/st ---")
		# By default the header emits 3 lines (cpu/aux/view) plus a 4th (bp)
		# only when one is set. The parser absorbs them into d.last_header.
		d.cmd("mod")
		check("header default emits cpu/aux/view (3 lines)",
		      len(d.last_header) == 3, d.last_header)
		check("line 1 prefix cpu", d.last_header[0].startswith("1: ["), d.last_header[0])
		check("line 2 prefix aux", d.last_header[1].startswith("2: ["), d.last_header[1])
		check("line 3 prefix view", d.last_header[2].startswith("3: ["), d.last_header[2])
		# Set a BP -> 4th line appears.
		d.cmd("sbp 00 abcd")
		d.cmd("mod")
		check("line 4 emits when BP set",
		      len(d.last_header) == 4 and d.last_header[3].startswith("4: [ bp="),
		      d.last_header)
		d.cmd("cbp 00 abcd")
		# hdr off suppresses all lines.
		d.cmd("hdr off")
		d.cmd("mod")
		check("hdr off suppresses header", d.last_header == [], d.last_header)
		# hdr cpu on enables only line 1.
		d.cmd("hdr cpu on")
		d.cmd("mod")
		check("hdr cpu on enables line 1 only",
		      len(d.last_header) == 1 and d.last_header[0].startswith("1: ["),
		      d.last_header)
		# Restore default.
		d.cmd("hdr on")
		# st returns a multi-line snapshot.
		data, _ = d.cmd("st")
		check("st returns mode/view/regs/clk/bp", any("mode" in l for l in data)
		                                       and any("view_pc" in l for l in data)
		                                       and any("clk" in l for l in data)
		                                       and any("bp" in l for l in data),
		      data)

	# bail exit code test runs in a separate process -- the X16dbg context
	# manager always quits cleanly, so we can't trigger bail from within it.
	print()
	print("--- bail exit code ---")
	env = os.environ.copy()
	env["SDL_VIDEODRIVER"] = "dummy"
	env["SDL_AUDIODRIVER"] = "dummy"
	bail_result = subprocess.run(
		[emu, "-rom", rom, "-debugstdio", "-warp"],
		input=b"brk\nbail\n",
		env=env,
		capture_output=True,
		timeout=3,
	)
	check("bail exits with code 1", bail_result.returncode == 1, bail_result.returncode)

	print()
	print(f"{passed} passed, {failed} failed")
	return 0 if failed == 0 else 1


if __name__ == "__main__":
	sys.exit(main())
