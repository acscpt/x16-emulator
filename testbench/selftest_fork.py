#!/usr/bin/env python3
"""Run upstream's selftest, tolerating the tests that are broken on
upstream-master itself, so the fork CI stays green on them.

Verified against upstream-master (no fork changes), with both the r49 release
ROM and the dev ROM: these three fail regardless of the fork or the ROM.

  test_stackpointer  -- selftest calls getStackPointer(), which testbench.py
                        does not implement (raises AttributeError).
  test_rombank       -- the ROM bank register ($01) does not read back the set
                        value over the -testbench path.
  test_rambank       -- same for the RAM bank register ($00).

They are marked expected failures here: the suite passes when exactly these
fail, a NEW failure in any other test still fails CI, and if upstream ever
fixes one it surfaces as an unexpected success (also a failure) so we drop it.
Upstream's selftest.py is left untouched.
"""
import unittest
from selftest import SelfTest

_KNOWN_BROKEN_UPSTREAM = ("test_stackpointer", "test_rombank", "test_rambank")

for _name in _KNOWN_BROKEN_UPSTREAM:
    setattr(SelfTest, _name, unittest.expectedFailure(getattr(SelfTest, _name)))

if __name__ == "__main__":
    unittest.main()
