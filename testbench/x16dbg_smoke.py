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
		"""Split a prompt-terminated body into (data, events, response)."""
		data, events, response = [], [], None
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
			else:
				data.append(ln)
		return data, events, response

	def cmd(self, line, timeout=2.0):
		"""Send a command and read until the next prompt.

		Returns (data_lines, events). Raises AssertionError on ERR, or
		TimeoutError if no prompt within `timeout`.
		"""
		self.send(line)
		body = self._read_until_prompt_bytes(timeout)
		data, events, response = self._parse_response(body)
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
		data, events, _ = self._parse_response(body)
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
		check("lbp returns the BP we set", data == ["00 abcd"], data)

		d.cmd("cbp 00 abcd")
		data, _ = d.cmd("lbp")
		check("after cbp, lbp returns no breakpoints", data == [], data)

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
		data, _ = d.cmd("r")            # `r` aliases `reg`
		check("alias 'r' dumps registers", "mode=" in data[0], data)

		d.cmd("rst")
		# rst by itself doesn't change mode; we should still be in STOP.
		data, _ = d.cmd("mod")
		check("after rst, still in STOP", any("mode=stop" in l for l in data), data)

	# bail exit code test runs in a separate process — the X16dbg context
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
