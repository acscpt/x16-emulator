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

	def __enter__(self):
		return self

	def __exit__(self, *_):
		self.close()

	def close(self):
		if self.proc.poll() is None:
			try:
				self.proc.stdin.write(b"qit\n")
				self.proc.stdin.flush()
			except OSError:
				pass
		try:
			self.proc.wait(timeout=2)
		except subprocess.TimeoutExpired:
			self.proc.kill()
			self.proc.wait()

	def _readline(self, timeout):
		ready, _, _ = select.select([self.proc.stdout], [], [], timeout)
		if not ready:
			raise TimeoutError("timed out reading stdout")
		return self.proc.stdout.readline().decode("ascii").rstrip("\r\n")

	def send(self, line):
		self.proc.stdin.write((line + "\n").encode("ascii"))
		self.proc.stdin.flush()

	def cmd(self, line, timeout=2.0):
		"""Send a command. Returns (data_lines, events_during_response).

		Raises AssertionError on ERR, TimeoutError if no RDY/ERR within `timeout`.
		"""
		self.send(line)
		data = []
		events = []
		deadline = time.time() + timeout
		while True:
			remaining = deadline - time.time()
			if remaining <= 0:
				raise TimeoutError(f"no RDY/ERR for {line!r}, got data={data}")
			ln = self._readline(remaining)
			if ln == "RDY":
				return data, events
			if ln.startswith("ERR"):
				raise AssertionError(f"{line!r}: {ln}")
			if ln.startswith("* "):
				events.append(ln)
			else:
				data.append(ln)

	def wait_event(self, timeout=2.0):
		"""Read until a `* …` line arrives."""
		deadline = time.time() + timeout
		while True:
			remaining = deadline - time.time()
			if remaining <= 0:
				raise TimeoutError("no event arrived")
			ln = self._readline(remaining)
			if ln.startswith("* "):
				return ln
			# Discard non-event lines (shouldn't normally appear here).


passed = 0
failed = 0


def check(label, cond, detail=""):
	global passed, failed
	if cond:
		passed += 1
		print(f"  OK   {label}")
	else:
		failed += 1
		print(f"  FAIL {label}{(': ' + detail) if detail else ''}")


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
		d.cmd("brk")
		ev = d.wait_event(timeout=2)
		check("brk triggers * BRK USER", ev.startswith("* BRK USER "), ev)

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
			"mem 16 bytes -> one formatted line",
			len(data) == 1 and re.match(r"^0400:( [0-9a-f]{2}){16}$", data[0]),
			data,
		)

		d.cmd("wmm 00 0400 de ad be ef")
		data, _ = d.cmd("mem 00 0400 04")
		check("wmm round-trip", data == ["0400: de ad be ef"], data)

		d.cmd("fil 00 0500 5a 8")
		data, _ = d.cmd("mem 00 0500 08")
		check("fil fills 8 bytes", data == ["0500: 5a 5a 5a 5a 5a 5a 5a 5a"], data)

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
		check("lbp returns the BP we set", data == ["00 abcd"], data)

		d.cmd("cbp 00 abcd")
		data, _ = d.cmd("lbp")
		check("after cbp, lbp returns no breakpoints", data == [], data)

		print()
		print("--- execution control ---")
		d.cmd("cnt")
		ev = d.wait_event(timeout=2)
		check("cnt emits * RES", ev == "* RES", ev)

		time.sleep(0.05)  # let CPU execute a bit
		d.cmd("brk")
		ev = d.wait_event(timeout=2)
		check("brk after cnt emits * BRK USER", ev.startswith("* BRK USER "), ev)

		_, _ = d.cmd("stp")
		ev = d.wait_event(timeout=2)
		check("stp emits * BRK STEP", ev.startswith("* BRK STEP "), ev)

		_, _ = d.cmd("sov")
		ev = d.wait_event(timeout=2)
		check("sov emits * BRK STEP", ev.startswith("* BRK STEP "), ev)

		d.cmd("rst")
		# rst by itself doesn't change mode; we should still be in STOP.
		data, _ = d.cmd("mod")
		check("after rst, still in STOP", any("mode=stop" in l for l in data), data)

	print()
	print(f"{passed} passed, {failed} failed")
	return 0 if failed == 0 else 1


if __name__ == "__main__":
	sys.exit(main())
