# Contributing

Fixes and features for the runtime, the bus, input, the timed loop and the
tools are welcome. Anything specific to one game belongs in that game's repo; the
toolkit gets the general mechanism (a hook, a flag), the game keeps the address.

## What never goes in a PR

- ROMs, dumps, or anything extracted from one.
- Output of `tools/autogen.py`, disassembly listings, or symbol maps generated
  from a ROM. The tool ships; its output stays on your machine.
- Proprietary SDK code or leaked material.

## Where your code comes from

This project is MIT. Contributions must be your own work or under an
MIT-compatible licence, and you should say where anything came from. The usual
way this goes wrong is porting a fix seen in a GPL or non-commercial emulator:
bsnes, higan, Mesen (GPL) or snes9x (non-commercial). That relicenses the code by
accident and is very hard to undo. Running those emulators as a black box to
compare output is fine; reading their source to write ours is not. Hardware
documentation (fullsnes, the SNESdev wiki) is fine.

## AI-assisted contributions

Welcome, provided a human understood and verified the change.

## Before you open a PR

1. Build (README, "Building from source") and run `examples/minimal` against a ROM you own.
2. If the change affects games, say which titles you checked (mk, mariopaint,
   stuntrace) and how: a conformance count before and after, or headless captures.
3. State whether default behaviour changes for every title or is opt-in.
