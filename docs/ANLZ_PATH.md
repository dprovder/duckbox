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
