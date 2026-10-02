# Roadmap

## Next

- **Hands on.** The window has run on a virtual monitor; nobody has yet played
  with a real keyboard, mouse or pad. Check mouse capture, Alt+Enter, the pad
  mapping and audio latency; fix what turns up.
- **A full playthrough of each game.** The high-score table, level changes and
  the endings go through code no scripted run has reached yet.
- **A longer conformance script**: an elevator to the next floor, a pickup, an
  enemy killed.

## The remaster

The recompile preserves the game exactly; these are the things worth adding on
top of it, roughly in order of how much love per hour they give.

- **Display**: vsync'd presentation (sharp, pixel-perfect and CRT modes are in),
  and a CRT mode with a little horizontal bloom.
- **Audio**: an OPL3 option (ymfm has it) and a proper resampler for the
  Sound Blaster's 7 kHz effects.
- **A native renderer at high resolution.** The engine's view is drawn by two
  routines — the raycaster (`AsmRefresh`) and the wall/sprite scalers — both
  hand-written, both self-modifying. Replacing exactly those with native code
  that draws the same scene at 1280×800 or widescreen, while the rest of the
  game stays the lifted original, is the real remaster. It needs the game's
  view state (player position and angle, the tile map, the visible-object list)
  located in DGROUP and documented first.
- **Quality of life**: rebinding keys from the host, autosave on level change.

## Deferred

- **Linux and macOS hosts.** The runtime is plain C except `host.c`; an SDL2
  host would make it portable. Not started, because every user so far is on
  Windows.
- **Disney Sound Source** (the parallel-port DAC both games support). The probe
  answers "absent"; nobody has one.
- **Other versions** (*Aliens of Gold* 1.0/2.0/3.0, shareware, *Planet Strike*
  1.0). The lifter is version-agnostic in principle; each needs its own
  conformance run.

## Out of scope

- Shipping or downloading any game file, or anything generated from the game
  executables.
- Using the GPL *Planet Strike* source release. This project is MIT and works
  from the binaries only; [BStone](https://github.com/bibendovsky/bstone) is
  the source port.
