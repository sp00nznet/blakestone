# The hi-res renderer

`--hires N` (default 4 in a window; F10 toggles) redraws the 3D view — walls,
floor, ceiling and sprites — at N × 320 by N × 200, from the game's own art, while
the rest of the game — logic, HUD, menus — stays the lifted original.

| | |
|---|---|
| ![Aliens of Gold, original and hi-res](screenshots/hires-aog.png) | ![Planet Strike, original and hi-res](screenshots/hires-ps.png) |
| *Aliens of Gold*: original (left), `--hires 4` (right) | *Planet Strike*: shaded wood ceiling and floor tiles |

Code: [`src/hires.c`](../src/hires.c); discovery in `find_renderer()` in
[`tools/lift.py`](../tools/lift.py).

## Why it is built this way

The obvious remaster is to replace the raycaster with a native one. That means
re-implementing every rule the game uses to choose what a ray shows: door
tracks and door sides, pushwalls part-way through a move, animated and
switchable walls, Blake Stone's own wall types. Each is a chance to differ from
the original, and none of it is visible in the code as a table; it is spread
across six hand-written `Hit*` routines.

Instead the original raycaster keeps running and the renderer asks it. The game
casts one ray per screen column and, for each, calls a `Hit*` routine that picks
the texture page and column, computes the wall height, and hands both to the
post scaler. A hook after each of those calls records the answer. Between two of
the game's columns the hi-res renderer interpolates: if both show the same
texture page, the texture coordinate (from the column, refined to sub-texel by the
ray's intercept) and the height are blended, and each output pixel is sampled from
the 64×64 texture directly; at an edge between surfaces it takes the nearer
column. The game's choice of texture column is ground truth — interpolation may
move between the two neighbours' columns, never past them.

The floor and ceiling are the same idea one level down. The span drawer is
called per row and per VGA plane with the row's texture coordinates as a start
and a step. Each captured span gives U and V as linear functions of x for its
row; output pixels between rows blend the two rows' functions. Shaded spans
(Planet Strike) carry their shading-table row, and walls recompute
`shade_max − 63·h / normalshade` from the interpolated height, so light falls off
smoothly rather than in steps.

## Who drew which pixel

Every write to video memory is tagged with who made it: the wall scalers, the
span drawers, or anything else (sprites, the weapon, the fizzle fade, text). The
compositor layers by that tag: original pixels from "anything else" stay on top,
then hi-res walls, then hi-res planes. Wall and floor edges are therefore exact
at the output resolution. Sprite pixels are redrawn too (next section); a sprite
whose capture cannot be fitted falls back to nothing rather than to stale pixels,
so a failure shows as a missing sprite, not a smeared one.

## Sprites

Actors, objects and the weapon go through `ScaleLSShape`/`ScaleShape`, which
call a per-column routine once per texel column with the sprite's height and the
destination column; it writes with a Map Mask covering every pixel that texel
column spans (1–4 at a time) and walks the column's posts —
`{end·2, source, start·2}` words ending in 0 — in the shape's page, whose
`t_compshape` header `{leftpix, rightpix, dataofs[]}` says which texel column
those posts are.

Capturing each call gives back the page, the height, the screen span and the
texel column. Consecutive captures of the same page and height, split where the
screen spans stop touching, are one sprite. A texel is `height/64` pixels wide
(the art is square, 64 texels across a `height/2`-pixel half-height), and every
span narrows the interval the sprite's centre can lie in; the fits agree to a
fraction of a pixel. The sprite is then drawn at the output resolution from its
own posts, through the shading table when the game used one, and hidden behind
any hi-res wall column taller than it — the game's own test, in the same units
(wall height = 4 × sprite height at equal distance).

`find_renderer()` finds the masked post scalers as the functions that patch an
`add ebp, imm32` and read `fs:` texels (shaded if they also read `gs:`), the
per-column routines as their far callers, and the DGROUP far pointers those read
the post list and the shading table through.

A frame's captures are committed when the game flips pages (the CRTC start
address changes; this engine writes only the high byte, its pages being 256-byte
aligned), so the data always matches the page on screen.

## Found, not listed

Nothing is addressed by a table of offsets. `find_renderer()` reads them out of
each game's code at lift time:

- the raycaster is the function whose branch opcodes are self-modified, and the
  far calls in it are the six `Hit*` routines;
- the first of those gives `yintercept`, `xintercept`, `pixx`, `wallheight[]` and
  `postsource` by the shape of its instructions;
- its near callee that writes the VGA Map Mask is `ScalePost`, which gives the
  lighting flag (`mov`/`and` in *Aliens of Gold*, `test` in *Planet Strike*),
  `normalshade`, `shade_max` and the shading tables;
- the wall scalers are the functions that read `gs:` texels and patch an
  `add edx, imm32`; the span drawers are the ones that `shld` packed
  coordinates and write `es:[bp+di]`, and what each writes (ceiling, floor,
  shaded) is read from its stores.

Both games resolve fully; a build whose game does not prints
`renderer: not found (...)` and simply has no hi-res mode.

## Cost

About 1.4 ms per 1280×800 frame on the development machine: the planes are
stepped in 16.16 fixed point along each output row, walls likewise down each
column, and the palette is a lookup table per frame.

## Checked by

`tools/conformance.py`: the renderer must be found in each game's code, and the
same moment of play at `--hires 4` must produce a 1280×800 frame whose view is
not the original enlarged.
