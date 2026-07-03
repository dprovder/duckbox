# duckdb-rekordbox

A DuckDB extension that turns DuckDB into a **DJ analysis engine + rekordbox USB exporter**.
Analyze audio with SQL functions, then `COPY` a table straight to a CDJ-ready USB — no rekordbox app.

```sql
LOAD rekordbox;

-- INSERT does the analysis (decode + aubio/libKeyFinder/Essentia in C++)
CREATE TABLE lib AS
SELECT file AS path,
       rb_bpm(file)      AS bpm,      -- DOUBLE
       rb_key(file)      AS key,      -- VARCHAR, Camelot (e.g. '6A')   ✅ implemented
       rb_beatgrid(file) AS beats,    -- DOUBLE[]  beat timestamps
       rb_loudness(file) AS loud      -- STRUCT(lufs, true_peak, lra)
FROM glob('~/Music/Hau5/*.m4a');

-- COPY writes the whole Pioneer tree: export.pdb + per-track ANLZ
COPY lib TO '/Volumes/MYUSB' (FORMAT rekordbox);
```

## Status

| Piece | State |
|---|---|
| Extension skeleton, registration, build files | ✅ scaffolded |
| `audio_decode` (FFmpeg → mono float PCM) | ✅ implemented |
| `rb_key` (libKeyFinder → Camelot) | ✅ implemented (ported from `../src/../kfcli`) |
| `rb_bpm` / `rb_beatgrid` (aubio) | ⬜ stubbed w/ API notes |
| `rb_loudness` (libebur128) | ⬜ stubbed |
| `COPY (FORMAT rekordbox)` → `export.pdb` | ⬜ plumbing done; DeviceSQL serializer TODO |
| ANLZ writer (PQTZ beatgrid / PPTH / PWAV) | ⬜ stubbed |
| High-level via Essentia (danceability/mood/genre) | ⬜ optional `-DWITH_ESSENTIA=ON` |

The `.pdb`/ANLZ byte layout is already reverse-mapped in [`../NOTES.md`](../NOTES.md) and there are
compiled parsers in [`../src/parsers/`](../src/parsers) plus reference databases in
[`../test/`](../test) for round-trip validation.

## Bootstrap (one-time)

This uses the standard DuckDB extension build. From this directory:

```sh
# 1. get DuckDB + the extension build tooling as submodules
git init && git submodule add https://github.com/duckdb/duckdb
git submodule add https://github.com/duckdb/extension-ci-tools
git checkout -b main   # pin duckdb/ to your target tag, e.g. v1.5.3

# 2. system deps not in vcpkg
brew install libkeyfinder aubio libebur128     # FFmpeg comes via vcpkg
```

> The linter/IDE errors about `duckdb.hpp not found` disappear after the submodules exist —
> that's where the DuckDB headers live.

## Build & test

```sh
make release            # -> build/release/extension/rekordbox/rekordbox.duckdb_extension
make test               # runs test/sql/*.test
# high-level descriptors:
make release WITH_ESSENTIA=ON
```

Load it (unsigned local build):

```sh
duckdb -unsigned
D LOAD './build/release/extension/rekordbox/rekordbox.duckdb_extension';
D SELECT rb_key('~/Music/Hau5/171 - Pliva.m4a');
```

## Architecture

```
 file path ─► rb_* scalar UDFs ─► DuckDB table ─► COPY(FORMAT rekordbox) ─► /PIONEER/
              │  audio_decode (FFmpeg)                                        ├ rekordbox/export.pdb
              │  libKeyFinder (key)                                           └ USBANLZ/**/ANLZ0000.DAT
              │  aubio (bpm, beatgrid)
              │  libebur128 (loudness)
              └  [Essentia] (danceability, mood, genre)   ← optional
```

- **Analysis** = scalar functions (`src/analysis.cpp`). Each decodes once via `src/audio_decode.cpp`.
- **Export** = a custom `CopyFunction` (`src/pdb_writer.cpp`). Rows are buffered in the global
  state and serialized to the multi-page `.pdb` + ANLZ files in `finalize` (a `.pdb` is not a
  streamable row format).

## Validation strategy (no CDJ needed until the end)

1. Build `export.pdb`, parse it back with `../src/parsers/rekordbox_pdb.py`, diff table/row
   counts and a track row against `../test/demo_export.pdb`.
2. Same for ANLZ via `rekordbox_anlz.py`.
3. Final gate: mount the USB in a CDJ and confirm beatgrids load. Iterate.

## Prior art

- [`whisper`](https://duckdb.org/community_extensions/extensions/whisper) — proves the
  FFmpeg-in-a-DuckDB-extension pattern (transcription, not music analysis).
- [libKeyFinder](https://mixxxdj.github.io/libkeyfinder/), [aubio](https://aubio.org),
  [Essentia](https://essentia.upf.edu) — the C++ MIR stack (all used by open-source DJ tools).
- No existing DuckDB extension does music/DJ analysis or rekordbox export.
