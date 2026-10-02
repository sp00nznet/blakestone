# Lifting Blake Stone: what it took

The things that each cost real time, in the order they turned up, so the next
Borland/JAM/Wolfenstein-engine title costs less. Code: [`tools/lift.py`](../tools/lift.py)
and, in pcrecomp, `decode16.py` / `lift16.py` / `cpu.h` / `unlzexe.py`.

## 1. Both executables are LZEXE 0.91

`BS_AOG.EXE` and `BS_FIRE.EXE` carry `LZ91` at offset 1Ch. The disassembler
sees only the 300-byte decompressor. pcrecomp had no LZEXE support, so
`tools/drm/unlzexe.py` was added there (pure Python, nothing is executed):

```
original/aog/BS_AOG.EXE: 139809 -> 346176 bytes, 3820 relocations, entry 0000:0000
original/ps/BS_FIRE.EXE: 144390 -> 355408 bytes, 3935 relocations, entry 0000:0000
```

The unpacked entry is Borland's c0 (`mov dx, DGROUP / mov cs:[02AE], dx /
mov ah, 30h / int 21h`) and the strings say *Borland C++ - Copyright 1991*.

## 2. Find code by decoding it, not by looking for 9Ah

The first function map trusted any relocation preceded by a `9A` byte as a far
call. Two "code segments" it produced, `3714` and `384A`, were the string
`NEW MISSION` and a run of zeros. Now a segment is code only if the walk from
the entry point reaches it through instructions it actually decoded.

## 3. Borland's 8087 emulator

The game builds its trig and projection tables with floating point (the
Wolfenstein engine's `BuildTables`/`CalcProjection`), compiled for Borland's
emulator: `INT 34h`–`3Dh` stand for the x87 escape opcodes and are decoded
back into them (`decode16.EMU87_INTS`). Two more things were needed:

- **387 trig**: Borland's math library checks for a 387 and uses `fsin` /
  `fsincos` when there is one. lift16 now lifts `D9 FE`, `D9 FF`, `D9 FB`.
- **`INT 3Eh` shortcuts**: `CD 3E EC 90` is not an interrupt followed by
  `repnz nop`; it is the emulator's own transcendental call, with a function
  byte and a pad. The 387 path sits right beside each one, which gives the
  table away: `3E EC` = sin (beside `fsin`), `3E F0` = tan (beside
  `fsincos; fdivp`), `3E F2` = atan. decode16 now decodes it as `emu3e`.

## 4. It is a 386 program in 16-bit clothes

The JAM engine needs a 386 (it checks: *no386*), and its asm uses
operand-size prefixes freely: `imul ecx`, `shr eax, 10h`, `add ebp, imm32`.
pcrecomp's `CPU` struct had 32-bit `EAX..EDX` but only 16-bit `SI DI BP SP`,
so the first build failed on `cpu->ebp`. They are unions now; writing the
16-bit half leaves the top half alone, as on the hardware.

## 5. `mov gs, ax` decoded as `mov cs, ax`

`8E E8`: the reg field is 5, GS. decode16 masked it with `& 3` and got CS. The
wall scaler loads its texture segment into GS and reads `gs:[bx+si]`, so every
texel came from the wrong place. Fixed in decode16.

## 6. Self-modifying code, twice

With GS fixed the walls were still noise. The wall scaler (`fn_04B92` and
three siblings) does this before its column loop:

```
mov dword cs:[0x72], edx      ; store the per-column step...
...
add edx, 0x12345678           ; ...into this instruction's immediate
```

A lifter that turns `0x12345678` into a C constant has lifted the placeholder.
pcrecomp's lift16 now takes a set of self-modified addresses (`smc_imm`);
`tools/lift.py` fills it with every constant `cs:` address the code writes, and
an immediate overlapping one is read from guest memory at run time.

That fixed the walls. Then actors started failing with a JAM error the game
writes to `BS_AOG.ERR`:

```
$8202
```

`JM_ERROR.H`, which ships with the game, decodes it: unit `0x82` is
`D3_STATE_ERROR`, error `02` is `MOVEOBJ_BAD_DIR`. An actor's `dir` word was
`0x0102` — the low byte a real direction, the high byte junk. A write watch
(`BSTONE_WATCH`, below) on that word named the writer: the raycaster
(`AsmRefresh`, `fn_187BE`), running off the end of its tables. It also patches
code — **opcodes** this time:

```
mov byte cs:[0x2AC], 0x7D     ; jge
mov byte cs:[0x258], 0x7E     ; jle
```

per view quadrant, flipping the direction of its tile-stepping branches. A
branch whose opcode byte is written now evaluates its condition from the byte
in memory (`cc_dyn` in `cpu.h`). Across both games those are the only two
kinds of self-modification: four patched `imm32`s and two patched `jcc`s each.

## 7. Switch tables

Borland emits two shapes and the game has a few of each:

- dense: `cmp bx, N / ja default / shl bx, 1 / jmp cs:[bx+table]` — one had 579
  cases (a `switch` on tile number), over the first cap of 512;
- sparse: `mov cx, N / mov bx, values / ... loop ... / jmp cs:[bx+N*2]`, and its
  **32-bit** variant, where the case values are dwords (low words, then high
  words) and the arms sit at `[bx + N*4]`.

The bound is read from the code in front of the jump; without one the jump goes
to the dispatcher instead of being guessed.

## 8. Functions nothing calls

A recursive walk finds what is called. Plenty is only *stored*: actor state
tables, setvect() handlers, atexit-style callbacks, the runtime's own vectors.
Each of these used to surface as a run-time `[miss]`. `code_pointers()` now
finds them statically, and with the run-time miss list empty both games run
the full conformance script with zero misses:

| Form | Example | How it is found |
|------|---------|-----------------|
| far, in data | actor state tables | a relocated code segment with its offset word in front |
| far, in code | `push seg / push off` for `setvect(8, ...)` | a relocated code-segment immediate with the offset as a neighbouring immediate |
| near, in code | `mov ax, offset handler / push ax` | an immediate that lands on a function prologue in the same segment |
| near, in DGROUP | `_exitbuf` | a data word that lands on a prologue in the runtime's segment |
| Borland `_INIT_`/`_EXIT_` | the startup's init list | 6-byte records `{type, priority, offset, 0}` walked back from c0's `mov di, <end>` |
| table in a code segment | the video-card probe list, walked with `lodsw / call ax` | 3-byte records found as a run of words that each land right after a `ret`/`jmp` |

Two lessons from getting that wrong first:

- **A pointer can only name code inside its segment's own extent.** A relocated
  data word holding segment 0, with `F7F3` in front of it, was taken as a far
  pointer — but segment 0's code ends at `4B80`, and `F7F3` is a function in a
  later segment. It was lifted with CS = 0, so its `push cs / call 000Fh`
  (a far call to a sibling in the same segment) called into the *startup code*
  — and Planet Strike ran `main` a second time after the title screen. It
  showed up as `open VSWAP.VSIVSI`: the extension appended twice.
- **A call states its target's CS; a stored pointer only implies one.** The walk
  from the entry point finishes before any recovered pointer is added, so the
  first CS an address is reached with is the authoritative one.

## 9. Small runtime traps

- **`_setargv` returns by jumping.** It pops its own return address and jumps
  to it after building `argv` on the stack. With the lifter's sentinel return
  address that is a jump to `0xFFFF`, which the dispatcher now treats as a
  return to the C caller.
- **DOS does not wrap reads at 64 KB.** An early `3Fh` split a read that crossed
  `FFFF` and wrapped the rest to `DS:0000`. DOS normalises the pointer and
  carries on linearly.

## Debugging kit

All of these exist because something was invisible without them.

| | |
|---|---|
| `work/<game>/misses.txt` | every address the guest jumped to that had no lifted function; the next `tools/lift.py` makes them entries. Expected to stay empty now (section 8). |
| `--trace` | every DOS call, DSP command and unhandled port |
| `cmake -B build-dbg -DBSTONE_TRACE=ON` | a build with a PDB, a lifted-call ring buffer and the two below |
| `BSTONE_WATCH=<linear hex>` | each write to that byte, with the host call stack — which is the guest call stack, because lifted functions keep their `fn_` names |
| `BSTONE_BT_OPEN=<name>` | the call stack at every open of a matching file |
| `LIFT_SHOW_TABLES=1` | the code-segment pointer tables `tools/lift.py` accepted |
| `--shot-at`, `--record` | frames at fixed emulated times; a video of a run nobody watched |
