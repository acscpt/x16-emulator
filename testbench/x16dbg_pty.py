#!/usr/bin/env python3
"""x16dbg_pty.py -- exercise the -debugstdio terminal (tty) code paths.

x16dbg_smoke.py drives the debugger through a pipe, where isatty() is false
and the interactive line-editor / async-redraw paths are switched off. This
script attaches x16emu's stdin/stdout to a pseudo-terminal so isatty() is
true, and covers the behavior the pipe test cannot reach:

  1. `brk` typed while the CPU runs breaks into the debugger, repeatably.
     (Regression: an earlier SIGINT-handler break called dbg_break() from
     signal context and wedged the REPL after the first break+resume.)
  2. An async breakpoint hit while sitting at a prompt with half-typed input
     erases the line, prints the event under a fresh prompt, and restores the
     in-progress input -- in order, under a single prompt.
  3. SIGINT (Ctrl-C) exits cleanly: the process dies from the signal after
     the handler restores the terminal (it does not control the emulator --
     `brk` does).

A pty is unavoidable here: the redraw paths are gated on isatty(), so a pipe
would switch them off. Everything else is plain subprocess -- the pty slave
is just handed to Popen as stdin/stdout, and Ctrl-C is delivered with
send_signal() rather than a controlling-terminal ^C.

POSIX-only (needs pty, which on a pipe-less platform is unavailable). Exits 0
if all checks pass, 1 otherwise, and exits 0 with a notice where there is no
pty. Reuses x16dbg_smoke's emulator/ROM discovery (same X16EMU_PATH /
X16ROM_PATH env vars).
"""

import os
import select
import signal
import subprocess
import sys
import time

try:
	import pty
except ImportError:
	print("x16dbg_pty: no pty support on this platform -- skipping")
	sys.exit(0)

from x16dbg_smoke import find_emulator, find_rom

PROMPT = b"x16db > "
CLEAR  = b"\r\x1b[K"   # term_clear_line(): CR + erase-to-end-of-line

passed = 0
failed = 0


def check(label, cond, detail=""):
	global passed, failed
	if cond:
		passed += 1
		print(f"  OK   {label}")
	else:
		failed += 1
		suffix = (": " + repr(detail)) if detail else ""
		print(f"  FAIL {label}{suffix}")


class PtyDbg:
	"""Run x16emu -debugstdio with its stdio attached to a pseudo-terminal."""

	def __init__(self, emulator, rom):
		# openpty gives a (master, slave) pair; the slave behaves like a real
		# terminal, so isatty() is true for the child. We drive the master.
		self.master, slave = pty.openpty()
		env = os.environ.copy()
		env["SDL_VIDEODRIVER"] = "dummy"
		env["SDL_AUDIODRIVER"] = "dummy"
		env["TERM"] = "xterm"
		self.proc = subprocess.Popen(
			[emulator, "-rom", rom, "-debugstdio", "-warp"],
			stdin=slave,
			stdout=slave,
			stderr=slave,
			env=env,
			close_fds=True,
		)
		os.close(slave)  # the child holds its own copy now

	def write(self, data):
		os.write(self.master, data)

	def drain(self, quiet=0.3, cap=3.0):
		"""Read until `quiet` seconds pass with no new bytes (or `cap` total)."""
		buf = b""
		start = time.time()
		deadline = start + quiet
		while time.time() < deadline and time.time() - start < cap:
			if select.select([self.master], [], [], 0.05)[0]:
				try:
					chunk = os.read(self.master, 65536)
				except OSError:
					break
				if not chunk:
					break
				buf += chunk
				deadline = time.time() + quiet
		return buf

	def interrupt(self):
		self.proc.send_signal(signal.SIGINT)

	def wait(self, timeout=2.0):
		"""Return the exit code (negative == killed by that signal), or None."""
		try:
			return self.proc.wait(timeout=timeout)
		except subprocess.TimeoutExpired:
			return None

	def close(self):
		if self.proc.poll() is None:
			self.proc.kill()
			self.proc.wait()
		try:
			os.close(self.master)
		except OSError:
			pass


def main():
	emu = find_emulator()
	rom = find_rom()
	print(f"emulator: {emu}")
	print(f"rom:      {rom}")
	print()

	d = PtyDbg(emu, rom)
	try:
		check("banner + initial prompt", PROMPT in d.drain(), "no prompt")

		# 1. brk breaks repeatably with no wedge. -debugstdio starts in RUN,
		# so the CPU is running and the prompt accepts commands meanwhile.
		print("--- brk / resume (no wedge) ---")
		for i in range(1, 4):
			d.write(b"brk\n")
			ev = d.drain()
			check(f"brk #{i} -> * BRK USER", b"* BRK USER" in ev, ev[-120:])
			d.write(b"c\n")
			check(f"resume #{i} -> * RES", b"* RES" in d.drain(), "no RES")

		# 2. Async breakpoint redraw preserves in-progress input.
		# `tb` sets a breakpoint at the view cursor (== current PC after brk)
		# WITH the correct memory bank, so it actually fires -- `sbp` leaves
		# the bank unset and never matches in hit_bp(). Sending "c\nreg" in
		# one write resumes the CPU and leaves "reg" as half-typed input; the
		# CPU re-hits the breakpoint a few cycles later.
		print()
		print("--- async breakpoint redraw ---")
		d.write(b"brk\n"); d.drain()
		d.write(b"tb\n");  d.drain()
		d.write(b"c\nreg")
		ev = d.drain(quiet=0.4, cap=3.0)
		check("redraw erases the prompt line", CLEAR in ev, ev[-160:])
		check("async event is * BRK BREAKPOINT", b"* BRK BREAKPOINT" in ev, ev[-160:])
		tail = ev.split(CLEAR)[-1]
		check("redraw emits a single prompt", tail.count(PROMPT) == 1, tail)
		check("in-progress input restored after prompt, in order",
		      ev.rstrip().endswith(PROMPT + b"reg"), ev[-80:])
		d.write(b"\n")  # run the restored `reg` -- confirms it is real input
		out = d.drain()
		check("restored buffer executed as `reg`",
		      b"mode=" in out and b"pc=" in out, out[:160])

		# 3. Ctrl-C (SIGINT) exits cleanly: terminal restored, dies from SIGINT.
		print()
		print("--- ctrl-c clean exit ---")
		d.interrupt()
		rc = d.wait(timeout=2.0)
		check("ctrl-c terminates the process", rc is not None, "still alive")
		check("process died from SIGINT", rc == -signal.SIGINT, rc)
	finally:
		d.close()

	print()
	print(f"{passed} passed, {failed} failed")
	return 0 if failed == 0 else 1


if __name__ == "__main__":
	sys.exit(main())
