# Contributors

Thank you to everyone who has contributed code, fixes, testing, or a hard-won
debugging insight. This file is the canonical record of who did what. The tools
and published work snesrecomp builds on are credited in the README's
[Credits](README.md#credits).

If you've contributed and aren't listed (or a line is wrong), open a PR against
this file. We want every name right.

---

## Maintainer

### Ned Heller ([@sp00nznet](https://github.com/sp00nznet))
Project creator and maintainer. The runtime, bus and hardware glue, the tools,
and the game projects that built them.

---

## Contributors

### [@Zaxaerith](https://github.com/Zaxaerith)
**65C816 status width side effects** (#8): `cpu_set_p()` let REP/SEP/PLP leave
16-bit index values in 8-bit mode and clear the fixed M/X bits in emulation
mode. It now forces M/X and a page-$01 stack while E is set, and clears the X/Y
high bytes whenever the index width is 8 bits. Also added the ROM-free CPU test
project (`tests/`, 40 scenarios).

**Generated functions start at their real entry** (#9): `autogen.py` emitted a
function's blocks in address order, so an entry that branched back to a lower
shared block began in that block instead and could return before running the
entry's own code. Generated functions now open with a `goto` to the entry.
Added `tools/test_autogen.py`, which compiles and runs synthetic cases against
the real `gen_ops.h`.

Both fixes were split out of sp00nznet/mk#1 at review.
