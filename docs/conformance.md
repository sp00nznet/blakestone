# Conformance

```
py tools/conformance.py            # check every game you have data and a build for
py tools/conformance.py --update   # record the current counts as the new baseline
```

Current figure (2026-10-02): **Aliens of Gold 14/14, Planet Strike 13/13.** The whole
run takes about a minute: headless time is deterministic and runs faster than
real time ([architecture](architecture.md#time)).

## What it checks, and why that is the ground truth

There is no reference build to diff frames against — the original only runs
under DOSBox — so the checks are things the original visibly does, run headless
with scripted keys (`--keys`) and fixed-time frame grabs (`--shot-at`).

| Check | Fails when |
|-------|-----------|
| lift: every instruction form handled | `tools/lift.py` met an instruction lift16 emitted as `UNHANDLED` |
| boot: clean exit | the process crashed or the game quit with an error code |
| boot: no dispatch misses | the guest jumped somewhere with no lifted function |
| boot: graphics on screen | 12 s in, the frame is not a graphics-mode picture (8+ colours) |
| play: clean exit / no JAM error | the game wrote its `.ERR` file (the `$8202` of [lifting.md](lifting.md) §6) |
| play: no dispatch misses | as above, through menus, a briefing and a minute of play |
| play: textured 3D view | the view window has fewer than 40 colours — the wall scaler and raycaster are both self-modifying, and a mis-lift shows up as flat or noisy walls |
| play: view changes as the player moves | under 20 % of the view changed between 47.5 s and 66.5 s while walking and turning |
| play: audio | the WAV the run wrote is near silent (OPL music and effects) |
| play: Sound Blaster initialised | the game never reset the DSP, set a rate and turned the speaker on |
| play: Sound Blaster digitized playback (*Aliens of Gold* only) | firing the pistol — a digitized sound — never started a DMA block |
| save: a mission saves to slot 0 | ESC → SAVE MISSION → slot 0 → a typed name did not write a `SAVEGAM0` of more than 1 KB |
| load: the saved mission loads into the 3D view | a fresh process, LOAD MISSION → slot 0, is not back in a textured view |

*Planet Strike*'s starting pistol is an FM sound, and its digitized sounds
depend on which actors turn up, so that game is not given the playback check:
it would pass or fail by chance. It does get "initialised".

The menu scripts are timed per game and were measured frame by frame (`--shot-at`).
Two things that are easy to get wrong: a key pressed while a menu is still drawing
is dropped, and a key pressed during Planet Strike's post-title credits skips them
and is spent doing it. Because headless time is deterministic, once measured these
land on the same frame every run. A save folder with different contents (a CONFIG,
say) changes start-up timing, so the harness always starts from a fresh one.

## Baseline and regressions

`tests/conformance_baseline.json` holds the pass count per game. A run that
passes fewer fails with `REGRESSION` and a non-zero exit. `--update` writes the
current counts; do that only when a change adds checks or fixes one.

## Where it runs

The game data is commercial and never in the repository. On a machine without
`original/<game>/` — CI included — every game is reported as `SKIP` and the run
passes. CI instead runs `tests/selftest.c`, which checks the VGA, x87 and sound
models on their own (Mode X planes, latch copies, CRTC page flips, the DAC, 80-bit
reals, `fistp` rounding, the DSP reset handshake, AdLib detection).

Each run leaves its frames, WAV and stderr in
`work/<game>/conformance/<timestamp>/`.
