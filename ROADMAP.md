# Roadmap

## Next

- **Conformance harness for the toolkit itself.** Today conformance lives in the
  game repos (Mario Paint's `tools/conformance.py`, built on this repo's
  lockstep validation). The toolkit needs its own fixed corpus: per-opcode
  `gen_ops.h` checks against LakeSnes's interpreter, runnable without a ROM, with
  a pass count in the README and a regression gate in CI.
- **CI on push** (public repo): build the library and example on Windows and Linux.
- **netlab recipe** upstream (`projects/snesrecomp.env`, mingw builder).
- **autogen.py coverage**: the routines it refuses (one instruction reached at
  two widths, stack tricks, `TXS`/`XCE`) need generator support.
- **Super FX in the documented API.** The GSU lives in the LakeSnes fork; the
  bus entry points Stunt Race FX uses should be documented here.

## Deferred

- A Quick-start `Setup.cmd` belongs to the game repos; the toolkit is consumed
  as a submodule.

## Out of scope

- Shipping game data, generated code, or a ROM-specific build.
