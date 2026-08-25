# Writing a CDJ USB

```zsh
cd ~/Music/rekordbox-duck
ui/duckbox-usb /Volumes/YOUR_USB
```

That writes `export.pdb`, the audio, and your beat grid, cue points and
waveforms. No rekordbox anywhere in the pipeline.

## 1. Prepare the drive — once

**FAT32 with an MBR partition map.** A drive that arrives NTFS or GPT gives
`NO USB` on the player; no CDJ reads NTFS.

```zsh
diskutil list                                    # find your disk, e.g. disk4
diskutil eraseDisk FAT32 KINGSTON MBR disk4      # DESTROYS the drive
```

Run that yourself — it is destructive and deliberately not scripted here. Check
the disk number twice; `disk0` is your Mac.

## 2. Export

**Whole library:**

```zsh
ui/duckbox-usb /Volumes/YOUR_USB
```

**A subset** — the third argument is a SQL `WHERE` clause over the `rb_deck`
view, aliased `d`:

```zsh
ui/duckbox-usb /Volumes/YOUR_USB library.duckdb "d.pos IN ('001','005','037')"
ui/duckbox-usb /Volumes/YOUR_USB library.duckdb "d.bpm BETWEEN 122 AND 126"
```

**From the UI** — tick tracks, type a destination, press **Build USB**. Same
exporter, reached over HTTP. See [RUNNING_THE_UI.md](RUNNING_THE_UI.md).

The two front ends do not send the same query, and neither is strictly better:

|  | `ui/duckbox-usb` | UI **Build USB** |
|---|---|---|
| track selection | whole library, or a `WHERE` clause | only the ticked rows |
| cue points | yes | yes |
| album artwork | **no** | yes |
| playlists | **no** | yes |
| clears the previous `PIONEER/` + `Contents/` | yes | no — writes over the top |
| preserves an existing `USBMNG.DAT` | yes | no — writes a fresh one |
| strips `._` sidecars, calls `sync` | yes | no |

So the UI writes a *richer* export and the script does the drive hygiene. If you
export from the UI, run `sync` yourself before ejecting.

**Raw, if you want to see the machinery:**

```sql
COPY rb_deck TO '/Volumes/YOUR_USB' (FORMAT rekordbox);
```

`rb_deck` (defined in `sql/analyze.sql`) is the join of `track_analysis` and
`waveform`; the COPY sink maps input columns by name, so any relation with the
right column names works. `ui/duckbox-usb` is a thin wrapper that adds the cue
join and preserves `USBMNG.DAT`.

## 3. Eject

```zsh
sync
diskutil eject disk4
```

`duckbox-usb` already calls `sync`. Do not pull the drive without ejecting —
FAT32 writes sit in the buffer cache and a half-written `export.pdb` browses
empty.

## What lands on the drive

```
Contents/<file>.…                                     your audio
PIONEER/rekordbox/export.pdb                          the DeviceSQL database
PIONEER/USBANLZ/P0xx/xxxxxxxx/ANLZ0000.DAT            grid, cues, waveforms
PIONEER/USBANLZ/P0xx/xxxxxxxx/ANLZ0000.EXT            scroll waveform
PIONEER/USBANLZ/USBMNG.DAT                            analysis slot table
PIONEER/MYSETTING.DAT                                 player settings
PIONEER/{Artwork,CDJ,MPJ}/                            skeleton a real export carries
```

One analysis folder per track, at the address the player computes from the
file's USB-relative path (see [ANLZ_PATH.md](ANLZ_PATH.md)). Roughly 61 KB of
analysis per track.

The `.EXT` deliberately carries only `PPTH` + `PWV3`. A CDJ-2000NXS throws
`E-8709` when it meets the CDJ-3000-era colour tags. Memory cues still ship, via
`PCOB` in the `.DAT`.

## Timing and gotchas

- **A full library takes 2–3 minutes**, almost all of it copying ~2 GB of audio.
  A three-track export is seconds — use one while iterating.
- **`duckbox-usb` deletes `PIONEER/` and `Contents/` before writing.** On a slow
  drive holding a large library that removal alone can take minutes. If it
  stalls, you can drop the `rm -rf` line and export over the top instead: orphan
  files are harmless, because the pdb is authoritative about what exists.
- **Any existing `USBMNG.DAT` is preserved.** It records which `ANLZ####` file is
  current per index, and a player that has analysed on this drive depends on it.
  `duckbox-usb` copies it aside and restores it.
- **macOS `._` sidecar files are deleted** after the write; they confuse some
  players.
- **The path is hashed exactly as written.** Rename a file on the drive and its
  analysis address changes, so re-export rather than renaming in place.

## Checking it without a deck

```zsh
find /Volumes/YOUR_USB/PIONEER/USBANLZ -name 'ANLZ0000.DAT' | wc -l   # == track count
ls /Volumes/YOUR_USB/Contents | wc -l
```

Every track row's `analyze_path` should name a folder that exists, and its
`file_path` a file that exists. If browsing works on the player but no waveform
or grid appears, read the diagnostics in [ANLZ_PATH.md](ANLZ_PATH.md) — in
particular, a macro waveform that draws *during playback* means the player never
parsed your file at all.
