# Runtime flags and diagnostics

What a game built on snesrecomp accepts at run time. Code: `src/snesrecomp.c`
(flags, capture), `src/input.c` (mouse script), `src/recomp_interp.c` (timed
loop, validation, profiling).

## Command line

A game passes `argc/argv` through `snesrecomp_parse_args()` before its own
parsing; it removes the flags it handles and returns the new `argc`
(`examples/minimal/main.c` shows it).

| Flag | Effect |
|---|---|
| `--headless` | No window, renderer or audio device, no 60 Hz pacing. Screenshots come straight from the PPU framebuffer, cropped like the windowed path. |
| `--record out.mp4` | Pipe every frame to `ffmpeg` (must be on `PATH`) as H.264. Implies `--headless`. |

Headless is the standing way to run anything over RDP or on a build box: it
never opens a window.

## Environment variables

| Variable | Effect |
|---|---|
| `SNESRECOMP_SHOT_FRAMES=120,300,600` | Screenshot after those presented frames (up to 32). |
| `SNESRECOMP_EXIT_FRAME=700` | `exit(0)` after that frame. With the line above, a run captures the same moment every time. |
| `SNESRECOMP_DUMP_PREFIX=path` | Prefix for PPU and snapshot dumps. |
| `SNESRECOMP_MOUSE_SCRIPT="frame:dx:dy:btn[:count] ..."` | Scripted SNES Mouse input. `@x:@y` instead of `dx:dy` parks a virtual pointer at SNES pixel coordinates (needs `recomp_input_set_mouse_cursor_addr`). `btn` is `0`, `L` or `R`. |
| `SNESRECOMP_MOUSE_DEBUG=1` | Log mouse packets. |
| `SNESRECOMP_INTERP_STATS=1` | One line per routine the timed loop interpreted instead of running natively. |
| `SNESRECOMP_VALIDATE_MAX=300` | Calls checked per validated function (default 300). Validation snapshots the whole machine per call, so it is capped. |

The `SMK_*` variables (`SMK_HEADLESS`, `SMK_SCRIPT`, `SMK_RECOMP_*`, `SMK_*_DEBUG`)
predate the toolkit's split from Super Mario Kart and are kept because its
tooling still sets them.

## Validation and profiling (API, `<snesrecomp/func_table.h>`)

```c
recomp_timed_add_intercept(0x01D9E1, true);   /* run the recompiled body here */
recomp_timed_add_validate(0x01D9E1, true);    /* ...or check it against the ROM */
recomp_timed_validate_report();               /* one VALIDATE line per function */
recomp_timed_profile_enable();
recomp_timed_profile_dump(50);                /* "BBAAAA P JSR|JSL" lines: autogen.py's input */
```

Validation compares WRAM, VRAM, CGRAM, OAM, the I/O registers, A/X/Y/DB/DP and
the flags once the genuine routine returns. A function that falls back to the
interpreter part way is skipped, not passed. Why it works this way:
[architecture.md](architecture.md#why-validation-runs-inside-the-timed-loop).
