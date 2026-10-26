# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow SemVer once the first one is tagged (none yet).

## [Unreleased]

### Added
- `SNESRECOMP_PAD_SCRIPT="frame[-end]:BTN[+BTN] ..."`: scripted port-1 joypad for
  headless runs; when set it is the only input, so a run is reproducible (#10).
- `--headless` and `--record out.mp4` via `snesrecomp_parse_args()`: no window,
  renderer, audio device or pacing; `--record` pipes every frame to ffmpeg (#1).
- `recomp_input_set_mouse_cursor_addr()`: steers a game's own cursor onto the
  host pointer; `SNESRECOMP_MOUSE_SCRIPT` takes absolute `frame:@x:@y:btn` steps (#2).
- Lockstep validation: `recomp_timed_add_validate()` /
  `recomp_timed_validate_report()` run a recompiled function and the genuine
  routine from the same state and compare memory, registers and flags (#3).
- `tools/autogen.py` and `<snesrecomp/gen_ops.h>`: lift profiled 65816 routines
  to C against the runtime (#5).
- `NOTICE` and `LICENSES/` for LakeSnes, Dear ImGui and stb_image_write;
  `CHANGELOG.md`, `ROADMAP.md`, `CONTRIBUTING.md`, `docs/` (#6).

### Changed
- LakeSnes bumped to include LakeSnes#2 and LakeSnes#3 (#11): the Super FX
  (GSU-2) core is rewritten from fullsnes, and the DSP-1 is a clean-room
  rewrite replacing Snes9x-derived code (mitaudit#2). Super FX titles run for
  the first time (Stunt Race FX reaches a race). DSP-1 titles (Super Mario
  Kart) get exact math instead of the old tables, so the numbers differ by a
  unit or two and AI/physics drift from the old build after a few seconds.
- LakeSnes bumped to include LakeSnes#1 (#4).

### Fixed
- Super FX games hung: the old GSU core mis-decoded much of the opcode map
  (LakeSnes#2, picked up by #11).
- SNES Mouse reported Left and Right swapped: the packet sends Right first
  (LakeSnes#1, picked up by #4). Every title using `SNES_INPUT_MOUSE` sees it.
- `examples/minimal` did not link with SDL2main (no `SDL_main`): it now
  includes `<SDL.h>` (#6).
