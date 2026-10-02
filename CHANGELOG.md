# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Changed
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
