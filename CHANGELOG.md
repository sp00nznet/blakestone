# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- Hi-res sprites: actors, objects and the weapon are redrawn at the output
  resolution from their own posts and shading, fitted from the per-column
  routine the game already calls, and occluded by the hi-res walls.
- The remaster: a hi-res 3D view (`--hires N`, default 4 in a window, F10
  toggles). Walls, floor and ceiling are redrawn at N x 320x200 from the games'
  own textures and lighting; the original raycaster and span drawer still run
  and are asked what each column and row shows, and the frame is composited by
  who drew each pixel, so sprites, HUD and logic are untouched. Everything it
  needs is found in each game's code at lift time (find_renderer). 1.4 ms a
  frame at 4x. Conformance gains two checks: 16/16 and 15/15.
- Saving and loading are checked by the conformance harness: a mission saved
  from the in-game menu, then loaded in a fresh process. 14/14 and 13/13.
- Gamepad: an XInput pad drives the games through the keys they already use
  (no in-game joystick calibration). Untested with real hardware.
- `--realtime`, to put a headless run back on the wall clock.
- Display modes, cycled with F11 or chosen with `--display`: `sharp` (4:3,
  as before), `pixel` (whole multiples, square pixels) and `crt` (4:3 with
  scanlines, each source row bright in the middle and dimmed at its edges).

### Changed
- Headless runs use deterministic time: each interrupt poll advances 300 µs,
  and DOS reports a fixed date. Runs are reproducible frame for frame and about
  13x faster than real time; the harness went from ~15 minutes to ~1. Before
  this, the menu scripts raced Planet Strike's title fade and failed at random.
- The windowed build has now run (on an offstage virtual monitor): 4:3 window,
  real time, intro through gameplay.
- Setup and CI pin pcrecomp to commit `fc852e3` (its `main` with #35–#39
  merged) instead of the `work/blakestone-integration` branch. Lifting against
  it reproduces the same C byte for byte; conformance still 12/12 and 11/11.

## [0.1.0] - 2026-10-02

First release: both games boot, reach their menus, start a mission and play,
recompiled from the Steam executables (*Aliens of Gold* 2.10R, *Planet Strike*
V1.01). Conformance 12/12 and 11/11.

### Added
- `tools/lift.py`: one lift driver for both games — LZEXE unpacking, recursive
  descent per code segment, Borland switch tables (dense, sparse, and sparse on
  32-bit values), stored-code-pointer recovery (far pointers in data and in code,
  near pointers, Borland `_INIT_`/`_EXIT_` records, pointer tables inside code
  segments), self-modifying-code detection, and the build tree with an embedded
  load image.
- The DOS machine in `src/`: INT 21h with a read-only data directory and a
  separate save directory, a DOS memory arena, BIOS video/keyboard/timer/mouse,
  PIC, PIT, keyboard controller, interrupt delivery on loop back-edges, a planar
  VGA with latches and all four write modes, an x87, ymfm's OPL2, a Sound Blaster
  DSP with 8237 DMA and IRQ, and the PC speaker.
- Win32 host: 4:3 nearest-neighbour window, Alt+Enter fullscreen, raw-input
  mouse, F12 screenshots, waveOut audio.
- Headless mode with `--record` (ffmpeg), scripted input (`--keys`), timed frame
  grabs (`--shot-at`) and `--wav`.
- `tools/conformance.py` (scripted end-to-end checks with a regression baseline)
  and `tests/selftest.c` (the hardware models, no game data); CI runs the latter.
- `Setup.cmd` / `scripts/setup.ps1`: the Quick start.
- Docs: architecture, lifting notes, conformance.

### Toolkit
Needs these pcrecomp changes, made for these games and opened as separate PRs:
- `tools/drm/unlzexe.py` — LZEXE 0.90/0.91 unpacking (pcrecomp#35).
- decode16/lift16: Borland's `INT 3Eh` emulator shortcuts; 387 `fsin`/`fcos`/`fsincos`. (pcrecomp#36)
- decode16: `mov` to/from FS and GS decoded as ES/CS. (pcrecomp#37)
- recomp16 `cpu.h`: 32-bit `ESI EDI EBP ESP`. (pcrecomp#38)
- lift16: self-modified immediates and self-modified branch opcodes. (pcrecomp#39)

[Unreleased]: https://github.com/sp00nznet/blakestone/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/sp00nznet/blakestone/releases/tag/v0.1.0
