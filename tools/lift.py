#!/usr/bin/env python3
"""
lift.py - lift a Blake Stone executable to C.

    py tools/lift.py aog        # Aliens of Gold  (original/aog/BS_AOG.EXE)
    py tools/lift.py ps         # Planet Strike   (original/ps/BS_FIRE.EXE)

Both games are the same JAM engine built with Borland C++ 3.x (1991 RTL),
medium model, LZEXE-packed. The pipeline is the same for each:

  1. unpack      pcrecomp tools/drm/unlzexe.py
  2. discover    recursive descent from the entry point, following near and far
                 calls by *decoding* them (a 9A byte in data is not a call), plus
                 every far code pointer the relocation table names in data --
                 the actor state tables are far structs full of them -- plus
                 whatever a previous run recorded as a dispatch miss.
  3. lift        pcrecomp lift16.Lifter, one C function per entry point. A
                 function is the closure of everything reachable from its entry
                 without a call, so shared tails are simply lifted twice.
  4. emit        work/<game>/gen/: chunked C, prototypes, segment constants,
                 and the address -> function table indirect calls go through.

The output is generated from the retail binary and is never committed; see the
README. Why the pieces are shaped this way: docs/architecture.md.
"""
import filecmp
import json
import re
import shutil
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

GAMES = {
    'aog': ('aog', 'BS_AOG.EXE'),
    'ps':  ('ps', 'BS_FIRE.EXE'),
}

# Where the program image goes in guest memory. DOS would put the PSP wherever
# the last TSR ended; this is a clean machine, so it goes low, and the game gets
# everything from just above the image to A000 for its heap. Baked into the
# lifted C (relocated segment immediates), so the runtime must agree:
# src/dos.c LOAD_SEG.
PSP_SEG = 0x0100
LOAD_SEG = PSP_SEG + 0x10

NL = chr(10)
CHUNK = 150          # functions per generated .c file: MSVC is slow on huge TUs


def _pcrecomp_home():
    """PCRECOMP_HOME, else a sibling checkout -- never one person's drive."""
    env = os.environ.get('PCRECOMP_HOME')
    cands = [env] if env else []
    cands += [os.path.join(os.path.dirname(ROOT), 'pcrecomp'),
              os.path.join(os.path.dirname(ROOT), 'pcrecomp-main')]
    for c in cands:
        if c and os.path.isdir(os.path.join(c, 'tools', 'lift')):
            return os.path.abspath(c)
    sys.exit('cannot find the pcrecomp toolkit; set PCRECOMP_HOME to a checkout of\n'
             '  https://github.com/sp00nznet/pcrecomp')


PCR = _pcrecomp_home()
for sub in ('disasm', 'lift', 'drm'):
    sys.path.insert(0, os.path.join(PCR, 'tools', sub))
import decode16                     # noqa: E402
decode16.WRAP_NEAR_TARGETS = True   # IP arithmetic wraps inside the segment
decode16.EMU87_INTS = True          # Borland emulator INT 34h-3Dh are x87 ops (BuildTables, CalcProjection)
from decode16 import Decoder, OpType  # noqa: E402
import lift16                       # noqa: E402
from lift16 import Lifter           # noqa: E402
import unlzexe                      # noqa: E402

TERMINATORS = {'ret', 'retf', 'iret', 'hlt'}
JCC = {'jo', 'jno', 'jb', 'jae', 'je', 'jne', 'jbe', 'ja', 'js', 'jns', 'jp',
       'jnp', 'jl', 'jge', 'jle', 'jg', 'loop', 'loopz', 'loopnz', 'jcxz'}


class Image:
    def __init__(self, path):
        raw = open(path, 'rb').read()
        if raw[0x1C:0x20] in (b'LZ09', b'LZ91'):
            raw = unlzexe.unpack(raw)
        self.exe = raw
        h = struct.unpack_from('<14H', raw, 0)
        self.hdr = h[4] * 16
        self.img = raw[self.hdr:]
        self.ss, self.sp, self.ip, self.cs = h[7], h[8], h[10], h[11]
        self.relocs = [struct.unpack_from('<HH', raw, 0x1C + 4 * i) for i in range(h[3])]
        # Linear positions of the relocated words, and what is stored there.
        self.reloc_at = {s * 16 + o for o, s in self.relocs}
        # Borland's c0 opens `mov dx, DGROUP`.
        assert self.img[self.cs * 16 + self.ip] == 0xBA, 'not a Borland c0 entry'
        self.dgroup = struct.unpack_from('<H', self.img, self.cs * 16 + self.ip + 1)[0]
        self._dec = {}

    def decode(self, cs, ip):
        d = self._dec.get(cs)
        if d is None:
            d = self._dec[cs] = Decoder(self.img[cs * 16:cs * 16 + 0x10000], cs * 16)
        d.pos = ip
        try:
            return d.decode_one()
        except (IndexError, KeyError):
            return None


def jump_table(img, fn, ins):
    """Arms of a Borland switch, `jmp word cs:[bx+disp]`, as IPs.

    Dense:  cmp bx, N / ja default / shl bx,1 / jmp cs:[bx+table]
    Sparse: mov cx, N / mov bx, values / ... loop ... / jmp cs:[bx+N*2]
            (the arm offsets follow the N case values; N*4 when they are dwords)
    The bound comes from the code in front of the jump; without one the table
    is not trusted, and the jump goes to the runtime dispatcher instead.
    """
    o = ins.op1
    if not (o and o.type == OpType.MEM and o.base == 'bx' and not o.index
            and o.seg == 'cs'):
        return None
    prev = [fn[a] for a in sorted(fn) if ins.address - 48 <= a < ins.address]
    bound = bx_imm = cx_imm = None
    for j in prev[-16:]:
        if not (j.op1 and j.op1.type == OpType.REG16 and j.op2
                and j.op2.type in (OpType.IMM8, OpType.IMM16)):
            continue
        reg = repr(j.op1)
        if j.mnemonic == 'cmp':
            bound = (j.op2.disp & 0xFFFF) + 1
        elif j.mnemonic == 'mov' and reg == 'bx':
            bx_imm = j.op2.disp & 0xFFFF
        elif j.mnemonic == 'mov' and reg == 'cx':
            cx_imm = j.op2.disp & 0xFFFF
    # Sparse: the arms follow the case values -- one word per case for a
    # 16-bit switch, two (low words, then high words) for a 32-bit one.
    if bx_imm is not None and cx_imm and (o.disp & 0xFFFF) in (cx_imm * 2, cx_imm * 4):
        bound, start = cx_imm, (bx_imm + o.disp) & 0xFFFF
    else:
        start = o.disp & 0xFFFF
    if not bound or bound > 0x1000:
        return None
    base = ins.offset - ins.address               # cs * 16
    return [struct.unpack_from('<H', img, base + ((start + 2 * k) & 0xFFFF))[0]
            for k in range(bound)]


def discover(im, extra):
    """{linear entry: cs}, {linear entry: {ip: Instruction}}, {entry: {jmp ip: arms}}"""
    entries = {}
    work = []

    def add(cs, ip):
        lin = cs * 16 + ip
        if lin >= im.dgroup * 16:                      # code ends where DGROUP begins
            return
        if lin not in entries:
            entries[lin] = cs
            work.append(lin)

    add(im.cs, im.ip)
    # Recovered pointers wait until everything reachable by calls has been
    # walked: a call states its target's CS, a stored pointer only implies one,
    # and the first CS an address is reached with is the one it keeps.
    pending = sorted(extra)

    bodies, tables, bad = {}, {}, []
    codesegs = set()
    while work or pending:
        if not work:
            for cs, ip in pending:
                add(cs, ip)
            pending = []
            continue
        lin = work.pop()
        cs = entries[lin]
        codesegs.add(cs)
        fn, tbl = {}, {}
        todo = [lin - cs * 16]
        while todo:
            ip = todo.pop()
            if ip in fn:
                continue
            ins = im.decode(cs, ip)
            if ins is None or ins.mnemonic == 'db':
                bad.append((cs, ip, lin))
                continue
            fn[ip] = ins
            m, o = ins.mnemonic, ins.op1
            nxt = (ip + ins.length) & 0xFFFF
            if m in TERMINATORS or m == 'jmp far':
                continue
            if m == 'jmp':
                if o.type in (OpType.REL8, OpType.REL16):
                    todo.append(o.disp & 0xFFFF)
                elif o.type == OpType.FAR:
                    add(o.far_seg, o.disp)
                else:
                    arms = jump_table(im.img, fn, ins)
                    if arms:
                        tbl[ip] = arms
                        todo.extend(arms)
                continue
            if m in JCC:
                todo.append(o.disp & 0xFFFF)
            elif m == 'call' and o and o.type == OpType.REL16:
                add(cs, o.disp & 0xFFFF)
            elif m == 'call' and o and o.type == OpType.FAR:
                add(o.far_seg, o.disp)
            todo.append(nxt)
        bodies[lin] = fn
        tables[lin] = tbl
    return entries, bodies, tables, codesegs, bad


# How a function in this program starts, when something stores a pointer to
# it rather than calling it: a Borland frame, `enter`, or the register saves
# that open an interrupt handler or a hand-written asm routine.
# The first group is distinctive enough to make a segment code on its own;
# the second only counts inside a segment already known to be code.
STRONG_SIGS = tuple(bytes(x) for x in (
    (0x55, 0x8B, 0xEC), (0x50, 0x53, 0x51, 0x52), (0x66, 0x56, 0x66, 0x57), (0x66, 0x60)))
ENTRY_SIGS = STRONG_SIGS + tuple(bytes(x) for x in (
    (0xC8,), (0x1E, 0x50), (0x60,), (0x1E, 0x56, 0x57), (0x56, 0x57),
    (0x1E, 0xB8)))                               # push ds / mov ax, DGROUP


def code_pointers(im, entries, bodies, codesegs, tables=False):
    """Entry points nothing calls directly, found from where their address is
    stored. Every one of these used to surface at run time as a dispatch miss:

      far, in data      a relocated code segment with its offset in front of it:
                        the actor state tables, the menu handler tables
      far, in code      a relocated code-segment immediate with the offset as a
                        neighbouring immediate -- `push seg / push off` is how
                        setvect() gets the timer, keyboard and SB handlers
      near, in code     `mov ax, offset` / `push offset` of a routine in the
                        same segment: atexit-style callbacks
      near, in DGROUP   the Borland runtime's own vectors (_INIT_ records,
                        _exitbuf and friends) into its code segment

    The near forms are only taken when the target opens like a function
    (ENTRY_SIGS); a plain immediate is far more often a number."""
    out = set()
    img = im.img

    # A near pointer can only name code in its own segment, which ends where
    # the next one begins. Without this a word that happens to point at a
    # prologue in a later segment becomes an entry with the wrong CS -- and
    # every near call and cs: read inside it resolves against that CS. (Planet
    # Strike's `push cs / call 000Fh` landed in the startup code and ran main
    # a second time.)
    known = sorted(codesegs)

    def seg_end(seg):
        later = [c for c in known if c > seg]
        return min(later[0] if later else im.dgroup, im.dgroup) * 16

    def opens_function(seg, off, sigs=ENTRY_SIGS):
        t = seg * 16 + off
        if seg in codesegs and t >= seg_end(seg):
            return False
        return t < len(img) and img[t:t + 4].startswith(sigs) and im.decode(seg, off)

    # bytes the walk decoded as instructions: a relocation there is an
    # immediate, handled below; anywhere else it is data
    covered = {ins.offset + k for body in bodies.values() for ins in body.values()
               for k in range(ins.length)}
    for p in sorted(im.reloc_at):
        seg = struct.unpack_from('<H', img, p)[0]
        if seg in codesegs and p >= 2 and p not in covered:
            off = struct.unpack_from('<H', img, p - 2)[0]
            if seg * 16 + off < seg_end(seg) and im.decode(seg, off) is not None:
                out.add((seg, off))

    for lin, body in bodies.items():
        cs = entries[lin]
        seq = [body[a] for a in sorted(body)]
        for i, ins in enumerate(seq):
            imms = [o for o in (ins.op1, ins.op2) if o is not None and o.type == OpType.IMM16]
            if ins.mnemonic not in ('mov', 'push') or not imms:
                continue
            v = imms[-1].disp & 0xFFFF
            at = ins.offset + ins.length - 2
            if at in im.reloc_at:
                if v < im.dgroup:                       # maybe a far pointer's segment half
                    for j in seq[max(0, i - 3):i + 4]:
                        for o in (j.op1, j.op2):
                            if (j is not ins and o is not None and o.type == OpType.IMM16
                                    and j.mnemonic in ('mov', 'push')):
                                off = o.disp & 0xFFFF
                                # a segment nothing calls yet must prove it is code
                                if (im.decode(v, off) is not None if v in codesegs
                                        else opens_function(v, off, STRONG_SIGS)):
                                    out.add((v, off))
            elif opens_function(cs, v):
                out.add((cs, v))

    seg0 = min(codesegs)
    for p in range(im.dgroup * 16, len(img) - 1):
        v = img[p] | img[p + 1] << 8
        if v and opens_function(seg0, v):
            out.add((seg0, v))

    # Borland's _INIT_/_EXIT_ lists: 6-byte records {type, priority, offset,
    # segment 0} the startup walks and far-calls through its own CS. The
    # zero segment carries no relocation, so the scan above cannot see them.
    # c0 ends each list with `mov di, <end>`; walk back from every such end.
    ip = 0
    for _ in range(200):
        ins = im.decode(seg0, ip)
        if ins is None:
            break
        o = ins.op2
        if ins.mnemonic == 'mov' and repr(ins.op1) == 'di' and o is not None and o.type == OpType.IMM16:
            end = im.dgroup * 16 + (o.disp & 0xFFFF)
            r = end - 6
            while r >= im.dgroup * 16 and img[r] in (0, 1, 2, 0xFF) and img[r + 4:r + 6] == bytes(2):
                off = img[r + 2] | img[r + 3] << 8
                if not im.decode(seg0, off):
                    break
                out.add((seg0, off))
                r -= 6
        ip += ins.length

    # Tables of near pointers inside a code segment, walked with lodsw/call ax
    # (the video-card probe list): runs of three or more words in bytes the
    # walk never decoded, each naming a decodable, not-yet-walked spot in the
    # same segment that the instruction before cannot fall into. Only once
    # the walk has converged -- before that, most of the image is "not
    # decoded" and data passes for tables by the thousand.
    if not tables:
        return out

    def after_exit(t):
        return (img[t - 1] in (0xC3, 0xCB, 0xCF) or img[t - 3] in (0xC2, 0xCA, 0xE9)
                or img[t - 2] == 0xEB)

    starts = sorted(codesegs) + [im.dgroup]
    found = []
    for cs, nxt in zip(starts, starts[1:]):
        lo, hi = cs * 16, min(nxt * 16, cs * 16 + 0x10000, len(img) - 1)
        p = lo
        while p < hi - 6:
            best = None
            for stride in (2, 3, 4):                  # bare words, or word + flag fields
                run, q = [], p
                while q + 1 < hi and q not in covered and q + 1 not in covered:
                    v = img[q] | img[q + 1] << 8
                    t = lo + v
                    if not (lo <= t < hi and t not in covered and not (p <= t < q + stride)
                            and after_exit(t) and im.decode(cs, v)):
                        break
                    run.append(v)
                    q += stride
                if len(set(run)) >= 3 and (best is None or len(run) > len(best[0])):
                    best = (run, q)
            if best:
                out.update((cs, v) for v in best[0])
                found.append((cs, p, best[0]))
                p = best[1]
            else:
                p += 1
    if os.environ.get('LIFT_SHOW_TABLES'):
        for cs, p, run in found:
            print(f'  table {cs:04X}:{p - cs * 16:04X} -> ' + ' '.join(f'{v:04X}' for v in run))
    return out


WRITES = {'mov', 'add', 'sub', 'adc', 'sbb', 'and', 'or', 'xor', 'inc', 'dec',
          'not', 'neg', 'shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'pop', 'xchg'}


def smc_targets(entries, bodies):
    """Linear addresses the code writes through a constant `cs:` address --
    the self-modifying inner loops patch their own immediates this way."""
    out = set()
    for lin, body in bodies.items():
        cs = entries[lin]
        for ins in body.values():
            o = ins.op1
            if (ins.mnemonic in WRITES and o is not None and o.type in (OpType.MEM, OpType.MOFFS)
                    and o.seg == 'cs' and not o.base and not o.index):
                for k in range(o.size or 2):
                    out.add(cs * 16 + ((o.disp + k) & 0xFFFF))
    return out


def _mem(op):
    """DGROUP offset of a plain `ds:[imm]` operand, else None."""
    if op is not None and op.type in (OpType.MEM, OpType.MOFFS) and not op.base \
            and not op.index and op.seg in ('', 'ds'):
        return op.disp & 0xFFFF
    return None


def find_renderer(entries, bodies, smc):
    """What the hi-res renderer (src/hires.c) needs, read out of this game's
    own code so no address is written down per version. See docs/renderer.md.

    Returns ({linear: hook}, {name: DGROUP offset}) or ({}, {}) with a
    reason printed, in which case the build simply has no hi-res mode.
      hook 'wall'  -- the self-modifying wall post scalers (tag their writes)
      hook 'plane' -- the floor/ceiling span drawer (tag its writes)
      hook 'hit'   -- the raycaster's six Hit* routines (capture each column)
    """
    seq = {lin: [b[a] for a in sorted(b)] for lin, b in bodies.items()}
    hooks = {}
    # The raycaster is the function whose branches are self-modified; the far
    # calls in it are the Hit* routines, the first of them HitVertWall.
    ray = [lin for lin, s in seq.items()
           if any(i.mnemonic.startswith('j') and i.offset in smc for i in s)]
    if len(ray) != 1:
        print(f'  renderer: {len(ray)} raycaster candidates, hi-res off')
        return {}, {}
    hits = []
    for i in seq[ray[0]]:
        if i.mnemonic == 'call' and i.op1 is not None and i.op1.type == OpType.FAR:
            t = i.op1.far_seg * 16 + i.op1.disp
            if t in entries and t not in hits:
                hits.append(t)
    for t in hits:
        hooks[t] = 'hit'
    for lin, s in seq.items():
        reads_gs = any(o is not None and o.type == OpType.MEM and o.seg == 'gs'
                       for i in s for o in (i.op1, i.op2))
        patched_add = any(i.mnemonic == 'add' and repr(i.op1) == 'edx' and i.offset + 3 in smc
                          for i in s)
        if reads_gs and patched_add:
            hooks[lin] = 'wall'
        if any(i.mnemonic == 'shld' for i in s) and any(
                i.mnemonic == 'mov' and i.op1 is not None and i.op1.type == OpType.MEM
                and i.op1.seg == 'es' and i.op1.base == 'bp' and i.op1.index == 'di' for i in s):
            hooks[lin] = 'plane'

    v = {}
    hv = seq[hits[0]] if hits else []
    for k, i in enumerate(hv):
        m, a = i.mnemonic, _mem(i.op2)
        if (m == 'mov' and repr(i.op1) == 'ax' and a is not None and k > 0
                and hv[k - 1].mnemonic == 'mov' and repr(hv[k - 1].op1) == 'dx'
                and _mem(hv[k - 1].op2) == a + 2):
            v.setdefault('yint', a)                     # yintercept: dx:ax = [a+2]:[a]
        if m == 'add' and _mem(i.op1) is not None and k + 1 < len(hv) and hv[k + 1].mnemonic == 'adc':
            v.setdefault('xint', _mem(i.op1))           # xintercept, low word
        if (m == 'mov' and repr(i.op1) == 'bx' and a is not None and k + 2 < len(hv)
                and hv[k + 1].mnemonic == 'shl' and hv[k + 2].mnemonic == 'mov'
                and hv[k + 2].op1 is not None and hv[k + 2].op1.type == OpType.MEM
                and hv[k + 2].op1.base == 'bx' and repr(hv[k + 2].op2) == 'ax'):
            v.setdefault('pixx', a)
            v.setdefault('wallheight', hv[k + 2].op1.disp & 0xFFFF)
        if m == 'mov' and _mem(i.op1) is not None and repr(i.op2) == 'ax':
            v['postseg'] = _mem(i.op1)                  # the last one wins
        if m == 'mov' and _mem(i.op1) is not None and repr(i.op2) == 'si':
            v['postoff'] = _mem(i.op1)
    # ScalePost: the near callee of HitVertWall that programs the Map Mask.
    cs = entries[hits[0]] if hits else 0
    for i in hv:
        if i.mnemonic == 'call' and i.op1 is not None and i.op1.type == OpType.REL16:
            sp = seq.get(cs * 16 + (i.op1.disp & 0xFFFF), [])
            if not any(j.mnemonic == 'out' for j in sp):
                continue
            loads = [(k, _mem(j.op2)) for k, j in enumerate(sp)
                     if j.mnemonic == 'mov' and repr(j.op1) == 'ax' and _mem(j.op2) is not None]
            flag_at = None
            for k, j in enumerate(sp):
                # AOG: mov ax,[flag] / and ax,800h.  PS: test word [flag], 800h.
                if (j.mnemonic == 'test' and _mem(j.op1) is not None and j.op2 is not None
                        and (j.op2.disp & 0xFFFF) == 0x800):
                    v['lightflag'], flag_at = _mem(j.op1), k
                elif (j.mnemonic == 'and' and repr(j.op1) == 'ax' and (j.op2.disp & 0xFFFF) == 0x800
                        and k > 0 and _mem(sp[k - 1].op2) is not None):
                    v['lightflag'], flag_at = _mem(sp[k - 1].op2), k
            after = [a for k, a in loads if flag_at is not None and k > flag_at]
            if len(after) >= 2:
                v['normalshade'], v['shademax'] = after[0], after[1]
            for k, j in enumerate(sp):
                if (j.mnemonic == 'mov' and repr(j.op1) == 'dx' and _mem(j.op2) is not None
                        and k + 1 < len(sp) and repr(sp[k + 1].op1) == 'bx' and _mem(sp[k + 1].op2) is not None):
                    v['ls_seg'], v['ls_off'] = _mem(j.op2), _mem(sp[k + 1].op2)
            break
    # The view's centre row: the wall scaler indexes ylookup with it.
    for lin, kind in hooks.items():
        if kind != 'wall':
            continue
        s = seq[lin]
        for k, i in enumerate(s):
            if (i.mnemonic == 'mov' and repr(i.op1) == 'ax' and _mem(i.op2) is not None
                    and k + 2 < len(s) and s[k + 1].mnemonic == 'shl' and s[k + 2].mnemonic == 'add'
                    and repr(s[k + 2].op1) == 'di'):
                v.setdefault('centery', _mem(i.op2))
    # The floor/ceiling span drawers: what each one writes, and their shared
    # inputs (DGROUP words loaded in a fixed order, then the texture segment
    # as an immediate). Planet Strike has four: shaded or not, with or without
    # the ceiling row.
    plane_flags = {}
    for lin, kind in hooks.items():
        if kind != 'plane':
            continue
        s = seq[lin]
        fl = 0
        for k, i in enumerate(s):
            o = i.op1
            if i.mnemonic == 'mov' and o is not None and o.type == OpType.MEM and o.seg == 'es':
                if o.base == 'di' and not o.index:
                    fl |= 1                               # the ceiling row, es:[di]
                if o.base == 'bp' and o.index == 'di':
                    fl |= 2                               # the floor row, es:[bp+di]
            if any(op is not None and op.type == OpType.MEM and op.seg == 'fs' for op in (i.op1, i.op2)):
                fl |= 4                                   # read through a shading table
        plane_flags[lin] = fl
        order = []
        for k, i in enumerate(s):
            if i.mnemonic == 'mov' and i.op1 is not None and i.op1.type == OpType.REG16:
                a = _mem(i.op2)
                if a is not None:
                    order.append((repr(i.op1), a))
                if (repr(i.op1) == 'ax' and i.op2 is not None and i.op2.type == OpType.IMM16
                        and k + 1 < len(s) and repr(s[k + 1].op1) == 'ds'):
                    v['pl_texseg'] = (i.op2.disp + LOAD_SEG) & 0xFFFF
                if (repr(i.op1) == 'ax' and a is not None and k + 1 < len(s) and repr(s[k + 1].op1) == 'fs'):
                    v['pl_shseg'] = a
                if repr(i.op1) == 'bx' and i.op2 is not None and i.op2.type == OpType.MEM \
                        and i.op2.seg == 'ss' and not i.op2.base:
                    v['pl_shoff'] = i.op2.disp & 0xFFFF
        names_ = {'bp': ['pl_bp'], 'cx': ['pl_cx'], 'dx': ['pl_dxh', 'pl_dxl'],
                  'si': ['pl_sih', 'pl_sil'], 'di': ['pl_di']}
        seen = {}
        for reg, a in order:
            if reg in names_ and seen.get(reg, 0) < len(names_[reg]):
                v.setdefault(names_[reg][seen.get(reg, 0)], a)
                seen[reg] = seen.get(reg, 0) + 1
    # Sprites: the masked post scalers (a patched `add ebp, imm32` and fs:
    # texels), and the per-column routines that call them, which take the
    # column's height as their first argument and read the column's post
    # list through a far pointer in DGROUP.
    sprite_scalers = {}
    for lin, s in seq.items():
        if any(i.mnemonic == 'add' and repr(i.op1) == 'ebp' and i.offset + 3 in smc for i in s) and \
                any(o is not None and o.type == OpType.MEM and o.seg == 'fs' for i in s for o in (i.op1, i.op2)):
            shaded = any(o is not None and o.type == OpType.MEM and o.seg == 'gs'
                         for i in s for o in (i.op1, i.op2))
            sprite_scalers[lin] = shaded
            hooks[lin] = 'sprite'
            if shaded:
                for k, i in enumerate(s):
                    if repr(i.op1) == 'ax' and _mem(i.op2) is not None and k + 1 < len(s) \
                            and repr(s[k + 1].op1) == 'gs':
                        v['sp_shseg'] = _mem(i.op2)
                    if repr(i.op1) == 'bx' and _mem(i.op2) is not None and i.mnemonic == 'mov':
                        v.setdefault('sp_shoff', _mem(i.op2))
    for lin, s in seq.items():
        called = [i.op1.far_seg * 16 + i.op1.disp for i in s
                  if i.mnemonic == 'call' and i.op1 is not None and i.op1.type == OpType.FAR]
        hit = [t for t in called if t in sprite_scalers]
        if not hit:
            continue
        hooks[lin] = ('scol', 1 if sprite_scalers[hit[0]] else 0)
        for k, i in enumerate(s[:8]):
            if (i.mnemonic == 'mov' and repr(i.op1) == 'ax' and _mem(i.op2) is not None and k + 1 < len(s)
                    and repr(s[k + 1].op1) == 'dx' and _mem(s[k + 1].op2) is not None):
                v.setdefault('sp_cmdseg', _mem(i.op2))
                v.setdefault('sp_cmdoff', _mem(s[k + 1].op2))
    v.setdefault('sp_shseg', 0)
    v.setdefault('sp_shoff', 0)
    v.setdefault('pl_shseg', 0)
    v.setdefault('pl_shoff', 0)
    for lin, fl in plane_flags.items():
        hooks[lin] = ('plane', fl)

    need = ('yint', 'xint', 'pixx', 'wallheight', 'postseg', 'postoff', 'lightflag',
            'normalshade', 'shademax', 'ls_seg', 'ls_off', 'centery',
            'pl_bp', 'pl_cx', 'pl_dxh', 'pl_dxl', 'pl_sih', 'pl_sil', 'pl_di', 'pl_texseg')
    missing = [n for n in need if n not in v]
    kinds = sorted(h if isinstance(h, str) else h[0] for h in hooks.values())
    if missing or 'plane' not in kinds or 'wall' not in kinds or kinds.count('hit') != 6:
        print(f'  renderer: not found ({", ".join(missing) or kinds}), hi-res off')
        return {}, {}
    print(f'  renderer: {kinds.count("wall")} wall scalers, {kinds.count("plane")} span drawers, 6 hit routines; '
          + ' '.join(f'{n}={v[n]:04X}' for n in need))
    return hooks, v


def load_misses(path):
    out = set()
    if os.path.exists(path):
        for line in open(path):
            line = line.strip()
            if ':' in line:
                s, o = line.split(':')[:2]
                seg = int(s, 16) - LOAD_SEG
                if 0 <= seg < 0x10000:
                    out.add((seg, int(o, 16)))
    return out


def main():
    game = sys.argv[1] if len(sys.argv) > 1 else 'aog'
    sub, exe = GAMES[game]
    src = os.path.join(ROOT, 'original', sub, exe)
    if not os.path.exists(src):
        sys.exit(f'{src} not found -- copy your own game files into original/{sub}/ '
                 '(see README, Getting Started)')
    work = os.path.join(ROOT, 'work', game)
    out = os.path.join(work, 'gen')
    os.makedirs(out, exist_ok=True)

    im = Image(src)
    open(os.path.join(work, 'unpacked.exe'), 'wb').write(im.exe)
    # Anything a previous run jumped to that had no lifted function. Local to
    # this checkout (work/ is never committed); see docs/lifting.md.
    extra = load_misses(os.path.join(work, 'misses.txt'))

    # Walk from the entry point, then add the stored code pointers the walk
    # makes visible and walk again until nothing new turns up.
    fps = set()
    for final in (False, True):
        while True:
            entries, bodies, tables, codesegs, bad = discover(im, extra | fps)
            more = code_pointers(im, entries, bodies, codesegs, tables=final) - fps
            if not more:
                break
            fps |= more

    print(f'{exe}: DGROUP {im.dgroup:04X}, {len(codesegs)} code segments, '
          f'{len(entries)} functions, {sum(len(b) for b in bodies.values())} '
          f'instructions lifted, {len(fps)} stored code pointers, '
          f'{len(extra)} extra entries, {sum(len(t) for t in tables.values())} switch tables')
    if bad:
        print(f'  {len(bad)} undecodable targets, first: '
              + ', '.join(f'{c:04X}:{i:04X} (in fn_{l:05X})' for c, i, l in bad[:5]))

    names = {lin: f'fn_{lin:05X}' for lin in entries}
    lifter = Lifter(known_funcs=names, reloc_words=im.reloc_at, load_seg=LOAD_SEG)
    lifter.far_base = 0
    lifter.dispatch = True
    lifter.iret_frame = True
    lifter.x87 = True
    lifter.smc_imm = smc_targets(entries, bodies)
    hooks, hv = find_renderer(entries, bodies, lifter.smc_imm)
    print(f'  {len(lifter.smc_imm)} self-modified code bytes')
    lift16.DIV0_FN = 'recomp_div0'

    # Write into a fresh directory, then sync() it over the real one.
    final, out = out, out + '.new'
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)

    order = sorted(entries)
    unhandled = {}
    files = []
    for ci in range(0, len(order), CHUNK):
        chunk = order[ci:ci + CHUNK]
        path = os.path.join(out, f'recomp_{ci // CHUNK:03d}.c')
        files.append(path)
        with open(path, 'w') as f:
            f.write('/* generated by tools/lift.py from your own copy of the game -- do not commit */\n')
            f.write('#include "lifted.h"\n\n')
            for lin in chunk:
                cs = entries[lin]
                insts = [bodies[lin][a] for a in sorted(bodies[lin])]
                insts = _seal_fallthrough(insts)
                lift16._CODE_SEG = f'{cs:04X}'
                lifter.jump_tables = {ip: [cs * 16 + a for a in arms]
                                      for ip, arms in tables[lin].items()}
                hook = hooks.get(lin)
                body = lifter.lift_function(names[lin] + ('_body' if hook else ''), insts,
                                            cs * 16, entry_addr=lin - cs * 16)
                for line in body.split('\n'):
                    if 'UNHANDLED' in line or 'needs dispatch' in line:
                        k = line.split('/*')[-1].strip(' */')
                        unhandled[k] = unhandled.get(k, 0) + 1
                f.write(body + '\n\n')
                # Renderer hooks wrap the original, which runs unchanged.
                if hook == 'hit':
                    f.write(f'void {names[lin]}(CPU *cpu) {{ {names[lin]}_body(cpu); hires_hit(cpu); }}\n\n')
                elif hook == 'wall':
                    f.write(f'void {names[lin]}(CPU *cpu) {{ int _t = g_draw_tag; g_draw_tag = DRAW_WALL; '
                            f'{names[lin]}_body(cpu); g_draw_tag = _t; }}\n\n')
                elif hook == 'sprite':
                    f.write(f'void {names[lin]}(CPU *cpu) {{ int _t = g_draw_tag; g_draw_tag = DRAW_SPRITE; '
                            f'{names[lin]}_body(cpu); g_draw_tag = _t; }}\n\n')
                elif hook and hook[0] == 'scol':          # ('scol', shaded)
                    f.write(f'void {names[lin]}(CPU *cpu) {{ hires_sprite_col(cpu, {hook[1]}); '
                            f'{names[lin]}_body(cpu); }}\n\n')
                elif hook:                                # ('plane', what it draws)
                    f.write(f'void {names[lin]}(CPU *cpu) {{ int _t = g_draw_tag; g_draw_tag = DRAW_PLANE; '
                            f'hires_plane(cpu, {hook[1]}); {names[lin]}_body(cpu); g_draw_tag = _t; }}\n\n')

    # A far call lift16 could not resolve names a function that does not
    # exist (in practice: bytes decoded past a call that never returns).
    # Define each as a dispatch, which logs a miss if it is ever reached,
    # rather than let the link fail.
    strays = set()
    for path in files:
        strays |= set(re.findall(r'\b(far_[0-9A-F]{4}_[0-9A-F]{4})\(cpu\)', open(path).read()))
    if strays:
        with open(files[-1], 'a') as f:
            for n in sorted(strays):
                f.write(f'void {n}(CPU *cpu) {{ cpu->sp += 4; '
                        f'dispatch_far(cpu, 0x{n[4:8]}, 0x{n[9:13]}); }}\n')
        print(f'  {len(strays)} far calls into nothing (stubbed as dispatch)')

    with open(os.path.join(out, 'recomp_all.h'), 'w') as f:
        f.write('/* generated by tools/lift.py -- do not commit */\n#pragma once\n')
        for cs in sorted(codesegs):
            f.write(f'#define SEG_{cs:04X} 0x{(cs + LOAD_SEG) & 0xFFFF:04X}\n')
        for lin in order:
            f.write(f'void {names[lin]}(CPU *cpu);\n')
        for n in sorted(strays):
            f.write(f'void {n}(CPU *cpu);\n')
    with open(os.path.join(out, 'recomp_dispatch.c'), 'w') as f:
        f.write('/* generated by tools/lift.py -- do not commit */\n#include "lifted.h"\n')
        f.write(f'const uint16_t g_load_seg = 0x{LOAD_SEG:04X};\n')
        f.write(f'const uint16_t g_psp_seg = 0x{PSP_SEG:04X};\n')
        f.write(f'const uint16_t g_entry_cs = 0x{im.cs:04X}, g_entry_ip = 0x{im.ip:04X};\n')
        f.write(f'const uint16_t g_entry_ss = 0x{im.ss:04X}, g_entry_sp = 0x{im.sp:04X};\n')
        f.write(f'const char g_game_id[] = "{game}";\n')
        f.write(f'const unsigned g_func_count = {len(order)};\n')
        hn = ('yint', 'xint', 'pixx', 'wallheight', 'postseg', 'postoff', 'lightflag',
              'normalshade', 'shademax', 'ls_seg', 'ls_off', 'centery',
              'pl_bp', 'pl_cx', 'pl_dxh', 'pl_dxl', 'pl_sih', 'pl_sil', 'pl_di', 'pl_texseg',
              'pl_shseg', 'pl_shoff', 'sp_cmdseg', 'sp_cmdoff', 'sp_shseg', 'sp_shoff')
        f.write('const HiresVars g_hires = {' + ('1, ' if hv else '0, ')
                + ', '.join(f'0x{hv.get(n, 0):04X}' for n in hn) + '};\n')
        f.write('const RecompFunc g_funcs[] = {\n')
        for lin in order:
            f.write(f'  {{0x{lin:05X}, {names[lin]}}},\n')
        f.write('};\n')
    # The load module itself, so the built exe needs only the data files: the
    # code is lifted, but the image's data -- DGROUP, the far tables -- is
    # loaded and relocated at boot exactly as DOS would.
    with open(os.path.join(out, 'image.c'), 'w') as f:
        f.write('/* generated by tools/lift.py -- do not commit */' + NL + '#include <stdint.h>' + NL)
        f.write(f'const unsigned g_image_len = {len(im.img)};' + NL)
        f.write(f'const unsigned g_nrelocs = {len(im.relocs)};' + NL)
        f.write('const uint16_t g_relocs[][2] = {' + NL)
        f.write(','.join(f'{{0x{o:X},0x{s:X}}}' for o, s in im.relocs) + NL)
        f.write('};' + NL + 'const uint8_t g_image[] = {' + NL)
        for i in range(0, len(im.img), 32):
            f.write(','.join(str(b) for b in im.img[i:i + 32]) + ',' + NL)
        f.write('};' + NL)
    json.dump({'game': game, 'dgroup': im.dgroup, 'functions': len(order),
               'codesegs': sorted(codesegs), 'unhandled': unhandled,
               'renderer': bool(hv)},
              open(os.path.join(work, 'lift.json'), 'w'), indent=1)
    if unhandled:
        print(f'  unhandled instruction forms: {sum(unhandled.values())} '
              f'({len(unhandled)} kinds), first: {list(unhandled)[:6]}')
    changed = sync(out, final)
    print(f'  -> {final} ({len(files)} files, {changed} changed)')


def sync(src, dst):
    """Move src's files over dst's, touching only those whose contents
    differ, and drop dst's files src does not have. An unchanged lift then
    leaves every timestamp alone and the next build compiles nothing -- which
    is what lets Setup be re-run cheaply -- and a smaller lift cannot leave a
    bigger one's chunks behind for the build's glob to pick up."""
    changed = 0
    for f in os.listdir(dst):
        if not os.path.exists(os.path.join(src, f)):
            os.remove(os.path.join(dst, f))
            changed += 1
    for f in os.listdir(src):
        a, b = os.path.join(src, f), os.path.join(dst, f)
        if os.path.exists(b) and filecmp.cmp(a, b, shallow=False):
            continue
        shutil.copyfile(a, b)
        changed += 1
    shutil.rmtree(src)
    return changed


def _seal_fallthrough(insts):
    """lift16 emits instructions in list order and lets C fall from one to the
    next. If a fallthrough successor is not the next one in the list -- two
    paths decoded the same bytes at different alignments -- make the edge an
    explicit jmp so C cannot fall into the wrong instruction."""
    out = []
    for i, ins in enumerate(insts):
        out.append(ins)
        m = ins.mnemonic
        if m in TERMINATORS or m in ('jmp', 'jmp far'):
            continue
        nxt = (ins.address + ins.length) & 0xFFFF
        if i + 1 < len(insts) and insts[i + 1].address == nxt:
            continue
        j = decode16.Instruction()
        j.mnemonic, j.address, j.offset, j.length = 'jmp', -1, ins.offset, 0
        j.raw = b''
        j.op1 = decode16.Operand(type=OpType.REL16, disp=nxt, size=2)
        out.append(j)
    return out


if __name__ == '__main__':
    main()
