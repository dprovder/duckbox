# How a CDJ locates a track's analysis files

The player does **not** use the `analyze_path` stored in the track row. It
computes the location itself, so analysis written anywhere else is ignored and
the deck silently re-analyses the track on every load.

    /PIONEER/USBANLZ/P<xxx>/<NNNNNNNN>/ANLZ0000.DAT

Both halves come from a single 32-bit hash of the track's USB-relative path,
taken over its UTF-16 code units, two rounds per character:

```
h = 0                                   # uint32, wrapping
for c in utf16(path):                   # e.g. "/Contents/TE3F.mp3"
    h = h * 23497 + c
    h = h * 37813 + c

index = h mod 200003
P     = bits 16, 13, 9, 7, 6, 2, 0 of index, packed in that order
```

That is the whole thing. `P` is not a second hash — it is seven bits gathered
out of the index.

## Why it resisted black-box fitting

Two reasons, and both are visible only once you have the code:

* **The double round.** Per character the recurrence is
  `h*888492061 + c*37814`, not `h*M + c`. Every rolling-hash family was
  searched in the `h*M + c` shape, so none of them could match, and the
  per-position "weights" measured empirically were really the composite
  `37814 * 888492061^k`.
* **`P` is a bit scatter.** It was searched for as a shift or a modulus of the
  index or of the raw 32-bit hash. Gathering non-adjacent bits 16, 13, 9, 7, 6,
  2, 0 is neither, so `P` looked independent of the index it is entirely
  derived from.

## Where it came from

`analyzer::CreateAnlzFileFolderPath` in the rekordbox 7 application binary
(`/Applications/rekordbox 7/rekordbox.app/Contents/MacOS/rekordbox`), which
ships unstripped: `nm` lists the symbol, and the hash is eleven instructions of
straight-line ARM64. The reduction mod 200003 appears as the usual
multiply-high-and-subtract (`0xA7C5075B`, `lsr #49`, `msub` by `0x30D43`), and
the bit gather as a run of `lsr`/`and`/`orr` feeding `String::formattedRaw`.

Confirmed two independent ways:

* it reproduces every address in `ui/anlz-folders.json` harvested from real
  rekordbox exports — 36 of 37, the one holdout being a row whose declared
  `analyze_path` already did not match what was on that drive;
* it reproduces both folders a CDJ-2000NXS chose **on its own**, for names it
  had never seen: `P014/00000378` for `/Contents/TE3F.mp3` and `P045/00011063`
  for `/Contents/TC2D.mp3`. The player and rekordbox run the same function.

Implemented in `src/include/anlz_index.hpp`; the exporter writes one folder per
track and needs neither the 128-folder fanout nor a harvested address.

## Feed it the path exactly as written

The hash runs over UTF-16 code units of the USB-relative path, leading slash
included, forward slashes, no case folding. Accented characters contribute
whatever normalisation form the path is stored in, so a name written NFD hashes
differently from the same name written NFC. Whatever goes on the drive is what
must be hashed.

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

## 3. The folder the player picks

Measured directly: for `/Contents/TE3F.mp3` the player independently chose
`P014/00000378`, and for `/Contents/TC2D.mp3` `P045/00011063`. Both are what the
function at the top of this document computes, so nothing has to be harvested,
learned or fanned out — the exporter writes the one folder the player will read.

Before the function was recovered, the workaround was to write the analysis into
all 128 `P000`-`P07F` folders at the computed index and let the player pick.
That also worked, and is what the first hardware confirmation ran on.

## 4. USBMNG.DAT is a slot table

`offset = 53 + 2*index`, u16 little-endian, holding which `ANLZ####` file is
current for that index. It reads as all-zero on a normal drive because everything
sits in slot 0. A player that analyses a track sets its slot: after it wrote
`ANLZ0001` for index 888, byte 1829 went `00 -> 01`, and `(1829-53)/2 = 888`.
Preserve any existing USBMNG.DAT when rewriting a drive.
