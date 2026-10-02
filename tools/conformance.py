#!/usr/bin/env python3
"""
conformance.py - does each recompiled game still do what the original does?

    py tools/conformance.py              # every game whose data and build exist
    py tools/conformance.py --update     # accept the current counts as the baseline

There is no reference build to diff against frame by frame (the original runs
only under DOSBox), so the ground truth is behaviour the original visibly has:
it boots, reaches its menus, starts a mission, renders a textured 3D view that
changes as the player moves, plays FM music and digitized sound through the
Sound Blaster, and never reports a JAM error or lands on an address with no
lifted code. Each scenario drives the game headless with scripted keys and
checks those things. See docs/conformance.md.

The game data is not redistributable, so on a machine without it (CI) every
game is reported as skipped and the run passes. A count below the committed
baseline (tests/conformance_baseline.json) fails, so a regression cannot land
quietly; --update records a new, higher figure.
"""
import json
import os
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(ROOT, 'tests', 'conformance_baseline.json')
GAMES = {'aog': 'BS_AOG.EXE', 'ps': 'BS_FIRE.EXE'}

# Title, menus, mission and difficulty select, briefing pages, then play:
# walk, turn, fire, open a door. Both games accept the same script.
ENTER_THROUGH = ','.join(f'{t}:ENTER' for t in
                         (3000, 9000, 13000, 15000, 17000, 20000, 22000, 24000, 26000,
                          28000, 30000, 32000, 34000, 36000, 40000, 44000))
# Fire and use a door early: Planet Strike's starting pistol is an FM sound,
# so walking alone never reaches the digitized path.
PLAY = ('47000:CTRL,48000:CTRL,49000:CTRL,50000:2,51000:CTRL,52000:CTRL+1500,'
        '54000:3,55000:CTRL+1500,57000:SPACE,58000:UP+2500,61000:SPACE,'
        '62000:LEFT+1500,64000:UP+3000,67500:SPACE')


def read_bmp(path):
    """The runtime's own BMPs: 32-bit, top-down. -> (w, h, list of 0xRRGGBB)"""
    d = open(path, 'rb').read()
    w, h = struct.unpack_from('<ii', d, 18)
    h = abs(h)
    px = struct.unpack_from(f'<{w * h}I', d, 54)
    return w, h, [p & 0xFFFFFF for p in px]


def region(img, x0, y0, x1, y1):
    w, h, px = img
    return [px[y * w + x] for y in range(y0, min(y1, h)) for x in range(x0, min(x1, w))]


def wav_rms(path):
    d = open(path, 'rb').read()
    n = (len(d) - 44) // 2
    if n <= 0:
        return 0.0
    s = struct.unpack_from(f'<{n}h', d, 44)
    step = max(1, n // 200000)
    sub = s[::step]
    return (sum(v * v for v in sub) / len(sub)) ** 0.5


def run(game, out, seconds, keys, shots, trace=False):
    exe = os.path.join(ROOT, 'build', 'Release', f'bstone_{game}.exe')
    save = os.path.join(out, 'save')
    os.makedirs(save, exist_ok=True)
    shot_arg = ','.join(f'{ms}:{os.path.join(out, name + ".bmp")}' for name, ms in shots.items())
    cmd = [exe, '--headless', '--seconds', str(seconds), '--save', save,
           '--data', os.path.join(ROOT, 'original', game), '--wav', os.path.join(out, 'audio.wav')]
    if keys:
        cmd += ['--keys', keys]
    if shot_arg:
        cmd += ['--shot-at', shot_arg]
    if trace:
        cmd += ['--trace']
    p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors='replace',
                       timeout=seconds * 3 + 60)
    open(os.path.join(out, 'stderr.txt'), 'w').write(p.stderr)
    errs = [f for f in os.listdir(save) if f.upper().endswith('.ERR')]
    return p.returncode, p.stderr, errs


def check_game(game):
    stamp = time.strftime('%Y%m%d-%H%M%S')
    base = os.path.join(ROOT, 'work', game, 'conformance', stamp)
    results = []

    def ok(name, cond, why=''):
        results.append((name, bool(cond), why))

    lj = os.path.join(ROOT, 'work', game, 'lift.json')
    lift = json.load(open(lj)) if os.path.exists(lj) else {}
    ok('lift: every instruction form handled', lift and not lift.get('unhandled'),
       ', '.join(lift.get('unhandled', {})) if lift else 'no lift.json')

    out = os.path.join(base, 'boot')
    rc, err, errs = run(game, out, 14, '', {'intro': 12000})
    ok('boot: clean exit', rc == 0, f'exit {rc}')
    ok('boot: no dispatch misses', '[miss]' not in err, err.count('[miss]'))
    intro = read_bmp(os.path.join(out, 'intro.bmp'))
    ok('boot: graphics on screen', len(set(region(intro, 0, 0, 320, 200))) >= 8,
       f'{len(set(region(intro, 0, 0, 320, 200)))} colours')

    out = os.path.join(base, 'play')
    rc, err, errs = run(game, out, 70, ENTER_THROUGH + ',' + PLAY,
                        {'view1': 47500, 'view2': 66500}, trace=True)
    ok('play: clean exit', rc == 0, f'exit {rc}')
    ok('play: no JAM error', not errs, ' '.join(errs))
    ok('play: no dispatch misses', '[miss]' not in err, err.count('[miss]'))
    v1, v2 = (read_bmp(os.path.join(out, f'view{i}.bmp')) for i in (1, 2))
    view1, view2 = region(v1, 16, 16, 304, 150), region(v2, 16, 16, 304, 150)
    ok('play: textured 3D view', len(set(view1)) >= 40, f'{len(set(view1))} colours')
    moved = sum(a != b for a, b in zip(view1, view2)) / max(1, len(view1))
    ok('play: view changes as the player moves', moved > 0.2, f'{moved:.0%} of pixels')
    rms = wav_rms(os.path.join(out, 'audio.wav'))
    ok('play: audio', rms > 200, f'rms {rms:.0f}')
    ok('play: Sound Blaster initialised (DSP reset, rate, speaker on)',
       '[sb] DSP 40' in err and '[sb] DSP D1' in err)
    if game == 'aog':
        # Aliens of Gold's pistol is a digitized sound, so firing it is a
        # deterministic test of the DMA/IRQ path. Planet Strike's is FM; its
        # digitized sounds depend on which actors turn up, so it is not
        # checked here (docs/conformance.md).
        ok('play: Sound Blaster digitized playback', err.count('[sb] play') > 0,
           f"{err.count('[sb] play')} blocks")
    return results


def main():
    update = '--update' in sys.argv
    base = json.load(open(BASELINE)) if os.path.exists(BASELINE) else {}
    totals, failed_regression = {}, False
    for game, exe in GAMES.items():
        have_data = os.path.exists(os.path.join(ROOT, 'original', game, exe))
        have_exe = os.path.exists(os.path.join(ROOT, 'build', 'Release', f'bstone_{game}.exe'))
        if not (have_data and have_exe):
            print(f'{game}: SKIP -- needs original/{game}/{exe} (your own copy) and a build; '
                  'see README, Getting Started')
            continue
        res = check_game(game)
        passed = sum(1 for _, good, _ in res if good)
        totals[game] = {'passed': passed, 'total': len(res)}
        print(f'{game}: {passed}/{len(res)}')
        for name, good, why in res:
            print(f'  {"PASS" if good else "FAIL"}  {name}' + (f'  ({why})' if why != '' else ''))
        if passed < base.get(game, {}).get('passed', 0):
            print(f'  REGRESSION: baseline is {base[game]["passed"]}/{base[game]["total"]}')
            failed_regression = True
    if update and totals:
        base.update(totals)
        os.makedirs(os.path.dirname(BASELINE), exist_ok=True)
        json.dump(base, open(BASELINE, 'w'), indent=1)
        print(f'baseline updated: {BASELINE}')
    return 1 if failed_regression else 0


if __name__ == '__main__':
    sys.exit(main())
