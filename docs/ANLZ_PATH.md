# How a CDJ locates a track's analysis files

The player does **not** use the `analyze_path` stored in the track row. It
computes the location itself, so analysis written anywhere else is ignored and
the deck silently re-analyses the track on every load.

    /PIONEER/USBANLZ/P<xxx>/<NNNNNNNN>/ANLZ0000.DAT

* `NNNNNNNN` is an index into a 200003-slot table inside `PIONEER/USBANLZ/USBMNG.DAT`
  (`byte offset = 53 + 2*index`; the player marks a used slot with `0x0008` and keeps
  a count at file offset 28).
* `P<xxx>` is a separate, still-unsolved function. Observed values are always < 128.

## The index function

Determined empirically from 39 samples: files with controlled names were placed on a
USB with no analysis, the deck was asked to analyse them, and the folders it created
were read back.

    index(name) = ((H0 + c1*W6 + c2*W5 + c3*W4) mod 2^32) mod 200003

for an 8-character name `T c1 c2 c3 . m p 3`, where each `c` is the character's
value relative to `'0'` (0x30), and

    H0 = 0x93611680
    W4 = 0x0D55355B   (character nearest the extension)
    W5 = 0x63CC9A08
    W6 = 0x7CC2F3B2

Weights depend only on a character's position *from the end of the name* and are
independent of file length, size and duration (verified with 30/60/90-second files).

### Why this was hard to see

The sum is taken mod 2^32 and only then reduced mod 200003, so whenever adding a
character's weight overflows 2^32 the observed index drops by `2^32 mod 200003 =
102874`. Measuring the same character position in two different names therefore
yields two different "weights" that differ by exactly that constant, which defeats
any naive fit. It is not a polynomial rolling hash: an exhaustive search over all
multiplier/scale pairs mod 2^32, and every standard hash family across encodings,
found no match.

### Recovering the constants

Each sample only reveals `H mod 200003`, i.e. one part in 21475 of a 32-bit value,
so the constants cannot be read off directly. Writing `H0 = h0 + a*P` (P = 200003)
with `h0` the known residue, each sample instead pins a linear combination of the
unknown multipliers to an interval of width `2^32/P`. Probes must use large
character deltas (`0`..`Z`, multiplier 42) — stepping only `0`..`3` yields intervals
so wide they carry almost no information.

21973 tuples satisfy all 39 constraints, but they agree on the predicted index for
every unseen name, so the residual degeneracy is harmless.


## Open problem: the player will not display our analysis

Browsing works. Playback works. Duration is right. But no waveform, grid or cues,
even when our analysis sits in the exact folder the player computes and
`analyze_path` agrees with it.

Individually proven correct against a CDJ-validated drive (`random-usb`):

* our audio — plays with a waveform under that drive's pdb
* our ANLZ bytes — display correctly when placed at that drive's `analyze_path`
* our track rows — the same six tracks analysed by us and diffed field by field
  against rekordbox's rows for the identical files: `file_size`, `sample_rate`,
  `bitmask`, `u3@18`, `u4@1a`, `u5@56`, `u7@5c` all match; the raw byte dump shows
  an identical 21-slot string layout
* the tables we leave empty — the working pdb still works with genres, albums,
  labels and artwork emptied
* the full `PIONEER` skeleton — now generated, `USBMNG.DAT` byte-identical
* the linkage — our pdb using rekordbox's exact `file_path` and `analyze_path`,
  with our analysis in that folder, still does not display

### Where to look next

The format is DeviceSQL (Encirq, 1998; now Ubiquitous AI). It is proprietary,
with no published format spec or source, and its SQL is compiled to C at build
time, so all query logic lives in the player firmware. DeviceSQL ships MPHash and
MPAVL indexing, which is very likely what the `page_flags & 0x40` "index" pages
are. We reproduce their *shape* — `u32 page_index | u32 first_data_page | u32
0x03ffffff | u32 0 | u32 word5 | N entries | 0x1ffffff8 fill` — but the meaning is
inferred from six sample files. Entries look like row pointers rather than plain
page numbers (`0x65b` = page 203, slot 3), so an index that merely looks right may
not resolve a lookup. If the player reaches analysis through an index rather than
straight off the row, that would be invisible to black-box testing, since browsing
clearly takes a different path.

Getting further probably needs the DeviceSQL format documentation or firmware
disassembly, not more USB experiments.


## Measured weight table

Recovered by using rekordbox itself as the oracle: generate files with controlled
names, import and export them, then read the folder rekordbox assigned to each
from `export.pdb`. rekordbox and the player agree on this function, so no CDJ is
needed — 50 samples came from three exports.

Weight is indexed by a character's distance from the **end of the filename**,
counting the extension (so the last character of an `.mp3` stem is offset 4).
Values are mod 200003.

| end offset | weight |
|---|---|
| 4 | 84673 |
| 5 | 128047 |
| 6 | 119759 |
| 7 | 142065 |
| 8 | 103380 |
| 9 | 172628 |
| 10 | 110359 |
| 11 | 10281 |
| 12 | 194214 |
| 13 | 161890 |
| 14 | 41273 |
| 15 | 184536 |
| 16 | 157977 |
| 17 | 158186 |
| 18 | 50320 |
| 19 | 64015 |
| 21 | 127140 |
| 23 | 31025 |
| 25 | 100387 |
| 27 | 31896 |

Offsets 4, 5 and 6 independently match the constants recovered earlier from CDJ
probes (84673 / 128047 / 119759), confirming the weights are position-from-the-end
constants that transfer between filenames of different lengths and content.

Carries must be resolved when combining measurements: an observation only fixes
`W` modulo the wrap count, so each measurement yields a candidate set
`{(delta + k*(2^32 mod P)) * dv^-1}` with `k` bounded by the signed character
delta, and the true weight is the intersection across measurements. Probes using
characters *below* the reference (negative deltas) shift the carry pattern and
resolve ties that same-direction probes cannot.

---

# Confirmed on hardware (CDJ-2000NXS)

Everything above concerns *where* the analysis goes. Getting a CDJ to actually
render it needed two more fixes, both verified on a player: our beat grid, cue
points and both waveforms now display from a duck-only export, with no rekordbox
anywhere in the pipeline.

## 1. PPTH must be NUL-terminated (this was the blocker)

`PPTH`'s length field counts a UTF-16 NUL terminator. We declared `len*2 + 2`
and then wrote only `len*2` bytes:

```
ours   PPTH tag_total=52  declared_len=38  actual_payload=36   <-- 2 bytes short
real   PPTH tag_total=54  declared_len=38  actual_payload=38
deck   PPTH tag_total=54  declared_len=38  actual_payload=38
```

A CDJ reads exactly `declared_len` bytes, so it consumed the first two bytes of
the *next* tag header, `PVBR` became `PV??`, and the tag chain derailed. The
player discards the whole file and re-analyses the track, writing its own
`ANLZ0001` (then `ANLZ0002`, ...) into the folder beside ours.

The symptom set is distinctive and worth recognising, because none of it points
at PPTH directly:

- macro waveform is **computed live as the track plays** instead of appearing on
  load — the stored `PWAV` was never reached,
- micro/scrolling waveform stays blank — `PWV3` in the `.EXT` was never reached,
- no beat grid,
- a new `ANLZ####` appears next to ours after every load.

If the macro waveform draws in during playback, the player is not reading the
file at all; look at parsing before looking at content.

## 2. PWV3 whiteness

Each `PWV3` byte is `(whiteness << 5) | height`. Deriving whiteness from the
high band's absolute share of energy puts bass-heavy material at 0-1, and the
player draws the detail waveform near-black on black — present, correctly
located, invisible. Real exports sit at 6-7 for most of a track. `WhitenessSeries()`
ranks each column against the rest of its own track and maps it onto the measured
distribution. See `anlz_writer.cpp`.

## 3. The player does compute the folder, but P need not be solved

Measured directly: for `/Contents/TE3F.mp3` the player independently chose
`P014/00000378`, and for `/Contents/TC2D.mp3` `P045/00011063` — both indices
exactly as predicted by the constants in `anlz_index.json`. So the index half is
solved and confirmed against hardware.

`P` remains an unsolved second hash (not a function of the index or of the raw
32-bit hash; searched every shift 0-31 against moduli 2-400). It does not need
solving: writing the analysis into all 128 `P000`-`P07F` folders at the computed
index covers whichever the player picks, and that is confirmed working. Cost is
~61 KB x 128 per track, so prefer the exact folder when it is known.

`ui/duckbox-learn` recovers the exact folder from a player that has analysed a
track, and caches it in `ui/anlz-folders.json`. That path also sidesteps the
index computation entirely, which is what makes arbitrary (non-`Txxx`) filenames
work.

## 4. USBMNG.DAT is a slot table

`offset = 53 + 2*index`, u16 little-endian, holding which `ANLZ####` file is
current for that index. It reads as all-zero on a normal drive because everything
sits in slot 0. A player that analyses a track sets its slot: after it wrote
`ANLZ0001` for index 888, byte 1829 went `00 -> 01`, and `(1829-53)/2 = 888`.
Preserve any existing USBMNG.DAT when rewriting a drive.
