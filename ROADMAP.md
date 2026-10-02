# Roadmap

## Next

- **Play it at a desktop.** Everything so far was verified headless. Run the
  windowed build, check the mouse capture, Alt+Enter, audio latency and that the
  keyboard feels right; fix what turns up.
- **A full playthrough of each game**, saving and loading along the way. Saved
  games and the high-score table go through code no scripted run has reached yet.
- **Gamepad.** The games read a joystick from port 201h. Map an XInput pad onto
  it (axes as RC timings, two buttons), so *JOYSTICK ENABLED* works.
- **A longer conformance script**: a door and an elevator, a pickup, an enemy
  killed, a level change, a save and a load.
- **Toolkit PRs merged**, and this repo pinned to a pcrecomp release instead of
  a branch.

## The remaster

The recompile preserves the game exactly; these are the things worth adding on
top of it, roughly in order of how much love per hour they give.

- **Display**: integer scaling as an option beside 4:3, a CRT/scanline filter
  for those who want 1993 back, and vsync'd presentation.
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
