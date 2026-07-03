# duckdb-rekordbox

A DuckDB extension that turns DuckDB into a **DJ analysis engine + rekordbox USB exporter**.
Analyze audio with SQL functions, then `COPY` a table straight to a CDJ-ready USB — no rekordbox app.

```sql
LOAD rekordbox;

-- Analyze the whole library ONCE (single decode per file) into a table.
-- rb_analyze does all the work: decode + aubio + libKeyFinder + libebur128 in C++.
CREATE TABLE library AS
SELECT file AS path, a.bpm, a.key, a.beatgrid, a.lufs, a.true_peak, a.lra
FROM (SELECT file, rb_analyze(file) AS a FROM glob('~/Music/Hau5/*.m4a'));
--   a = STRUCT(bpm DOUBLE, key VARCHAR, beatgrid DOUBLE[], lufs, true_peak, lra)

-- From here it's just fast SQL over the stored table — no re-analysis:
SELECT path, bpm, key FROM library WHERE key='8A' AND bpm BETWEEN 124 AND 128;

-- COPY writes the whole Pioneer tree: export.pdb + per-track ANLZ (serializer WIP)
COPY library TO '/Volumes/MYUSB' (FORMAT rekordbox);
```

**`rb_analyze(path)` is the primary entry point** — one decode, all features, meant to be
materialized into a table you query forever. The single-purpose functions below still exist
for ad-hoc use.

## Status

| Piece | State |
|---|---|
| Extension skeleton, registration, build files | ✅ scaffolded |
| `audio_decode` (FFmpeg → mono float PCM) | ✅ implemented |
| `rb_analyze` (one decode → all features STRUCT) | ✅ implemented — **use this** |
| `rb_key` (libKeyFinder → Camelot) | ✅ implemented |
| `rb_bpm` / `rb_beatgrid` (aubio) | ✅ implemented |
| `rb_loudness` (libebur128 → LUFS/true-peak/LRA) | ✅ implemented |
| `COPY (FORMAT rekordbox)` → `export.pdb` | 🟡 plumbing + combine done; DeviceSQL serializer TODO (see `SERIALIZER_DESIGN.md`) |
| High-level (danceability/mood/genre) | ⬜ Essentia — see `ESSENTIA_INTEGRATION.md` |
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
