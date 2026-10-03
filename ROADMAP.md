# Roadmap

## Next

- **Hands on.** The window has run on a virtual monitor; nobody has yet played
  with a real keyboard, mouse or pad. Check mouse capture, Alt+Enter, the pad
  mapping and audio latency; fix what turns up.
- **A full playthrough of each game.** The high-score table, level changes and
  the endings go through code no scripted run has reached yet.
- **Floor 2 in the mission script.** The route already kills, picks up and
  reaches the elevator panel; the game wants the RED card first. It lies at
  (51,47) in a room of plasma spheres, past a one-tile corridor an informant
  stands in (docs/conformance.md, "Routes"). Then the same for *Planet
  Strike*, whose teleporter says "TELEPORT DISABLED" for every other area.

## The remaster

The recompile preserves the game exactly; these are the things worth adding on
top of it, roughly in order of how much love per hour they give.

- **Display**: vsync'd presentation (sharp, pixel-perfect and CRT modes are in),
  and a CRT mode with a little horizontal bloom.
- **Audio**: an OPL3 option (ymfm has it) and a proper resampler for the
  Sound Blaster's 7 kHz effects.
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
