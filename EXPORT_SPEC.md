# Export spec — playlists, cue points, beat grid & artwork → rekordbox USB

How to make the things you build in **duckbox** (the UI) show up on a Pioneer CDJ:
named **playlists**, editable **cue/hot-cue points**, the **corrected beat grid**,
and **album artwork**. This extends the existing
`COPY rb_deck TO '/Volumes/USB' (FORMAT rekordbox)` exporter.

Status legend: ✅ done · 🟡 partial · 🔴 not started.

---

## 0. Where things stand

`COPY … (FORMAT rekordbox)` (`src/pdb_writer.cpp` sink + `RbFinalize`) already writes:

- `PIONEER/rekordbox/export.pdb` — 20 DeviceSQL tables: tracks + interned
  artists/albums/genres/labels/keys/colors, and **one "All Tracks" playlist**
  (`tabs[7]` playlist_tree, `tabs[8]` playlist_entries).
- `PIONEER/USBANLZ/**/ANLZ0000.{DAT,EXT}` — `PQTZ` beat grid, `PPTH`, `PWAV/PWV2/
  PWV3/4/5` waveforms, `PVBR`, and **empty** `PCOB` cue tags.
- `/Contents/<file>` audio; track `file_path` rewritten to the USB-relative path.

The track row already reserves `artwork_id` (`u4 @ 0x1c`, currently `0`) and the
writer has `PlaylistTreeRow()` / `PlaylistEntryRow()` helpers. So playlists and
artwork are half-wired; cues are stubbed.

### Data sources (already in `library.duckdb`, populated by the UI)

| Table | Columns | Produced by |
|---|---|---|
| `track_analysis` | `beats DOUBLE[]`, `downbeat_index`, `bpm`, … | analysis + **grid editor** (beats/bpm/downbeat are user-corrected) |
| `cues` | `pos, kind, hot_num, time_ms, loop_time_ms, color INT, comment` | cue bank UI |
| `playlists` | `id, name` | playlist UI |
| `playlist_tracks` | `playlist_id, pos, sort` | playlist UI (drag-reorder sets `sort`) |
| `rb_artwork(path)` | `→ BLOB` (JPEG/PNG) | ffmpeg attached-pic UDF |

---

## 1. Beat grid — `PQTZ` (+ `PQT2`)  🟡 wire the corrected grid

**Already emitted**, but must use the **grid the user corrected in the UI**.
`rb_deck` reads `track_analysis.beats` + `downbeat_index` directly, and the grid
editor writes those columns, so corrected grids already flow through — **just
confirm** the sink never recomputes a grid.

`PQTZ` entry (3 fields each, see `specs/rekordbox_anlz.ksy`):
- `beat_number` u2 = `((i - downbeat_index) mod 4) + 1`  (1 = downbeat)
- `tempo` u2 = `round(bpm * 100)`
- `time` u4 = `round(beats[i] * 1000)` ms

**TODO (optional, CDJ-3000):** also write `PQT2` in `.EXT` (extended grid,
`len_header 56`) — same beat list, plus a bpm/beat block. Needed only by newer
players; NXS2 is fine on `PQTZ` alone.

**Effort:** trivial (verification only) + optional PQT2.

---

## 2. Cue & hot-cue points — `PCOB` / `PCO2`  ✅ DONE

Implemented: every saved cue is exported as a **memory cue** (unlimited, always
shows as a waveform marker on every CDJ). `PCOB` (memory list) is populated in
both `.DAT` and `.EXT` with `PCPT` entries; `PCO2`/`PCP2` in `.EXT` add the
per-cue **colour + comment** for nxs2/CDJ-3000. Plumbed via a `cues LIST(STRUCT(
t, color, comment))` column joined onto `rb_deck` in the export query →
`RbSink` → `AnlzCue` → `BuildCue`/`BuildCue2` in `anlz_writer.cpp`. Verified
byte-for-byte (times, comments, RGB). Hot-cue (A–H) slots left empty for now.
Original design below.



CDJs read cues from the **ANLZ** file, not the pdb. Two tags:

- **`PCOB`** (`.DAT`) — the classic cue list every player understands.
- **`PCO2`** (`.EXT`) — extended list adding **colour** + **comment** (Nexus2 /
  CDJ-3000 show these). Write both from the same `cues` rows.

### `PCOB` layout (per Deep Symmetry `anlz.html`)
```
fourcc  "PCOB"
len_header u4 = 0x18
len_tag    u4
type       u4   ; 0 = memory cues, 1 = hot cues   (write TWO PCOB tags)
unk        u2
len_cues   u2   ; number of cue entries
memory_count u4 ; for type 0
then len_cues × cue_entry:
  fourcc "PCPT"  len_header=0x1c  len_tag
  hot_cue   u4   ; 0 = memory cue, 1..8 = hot cue slot (A..H)
  status    u4   ; 0 = disabled, 4 = enabled
  unk1      u4   ; usually 0x10000
  order_first u2, order_last u2
  type      u1   ; 1 = point, 2 = loop
  unk       ×3
  time      u4   ; ms  ← cues.time_ms
  loop_time u4   ; ms, 0xffffffff if not a loop ← cues.loop_time_ms
  unk ×16
```

### `PCO2` layout
Adds, per cue: `color_id u1` + explicit `r,g,b u1` + a UTF-16BE `comment` with a
length prefix. Newer players prefer `PCO2` when present.

### Mapping from `cues`
| `cues` column | → |
|---|---|
| `kind` | `'hot'` → hot cue (type-1 PCOB, `hot_cue = hot_num`); else memory cue (type-0, `hot_cue = 0`) |
| `hot_num` | hot-cue slot 1..8 |
| `time_ms` | `time` |
| `loop_time_ms` | `loop_time` (NULL → `0xffffffff`, `type = point`) |
| `color` (packed `0xRRGGBB`) | `PCO2` r/g/b bytes (+ nearest rekordbox `color_id`) |
| `comment` | `PCO2` UTF-16BE comment |

### Plumbing (fits the COPY model)
Add a **`cues` LIST column** to `rb_deck` so each track row carries its cues:
```sql
-- in the rb_deck view
LEFT JOIN (
  SELECT pos, list(struct_pack(
           kind := kind, hot := coalesce(hot_num,0),
           t := time_ms, loop := coalesce(loop_time_ms,-1),
           color := coalesce(color,2282416), comment := coalesce(comment,''))
         ORDER BY time_ms) AS cues
  FROM cues GROUP BY pos
) c USING (pos)
```
`RbSink` reads the optional `cues LIST(STRUCT(...))` column; `BuildAnlzDatBytes`
gets the `PCOB` tags, `BuildAnlzExtBytes` gets `PCO2`. Both already build the
byte blobs in plain C++ (`anlz_writer.cpp`), so this is: new struct → serialize
loop → replace the empty `PCOB` with populated tags.

**Effort:** medium. Highest player value. No new player-format research beyond
the byte layouts above (validate against `test/reference/ANLZ0000.EXT`).

---

## 3. Named playlists — pdb `playlist_tree` + `playlist_entries`  🟡

`tabs[7]` (playlist_tree) and `tabs[8]` (playlist_entries) already exist with one
"All Tracks" row. Extend to write the user's playlists.

`PlaylistTreeRow(id, parent_id, sort_order, is_folder, name)` — note the **hidden
`size:4` gap** between `parent_id` and `sort_order` (5×u4 header) already handled
in the helper. `PlaylistEntryRow(entry_index, track_id, playlist_id)`.

### Mapping
- Keep "All Tracks" as tree id 1 (parent 0).
- For each `playlists` row → a tree row: `id = 2 + n`, `parent_id = 0`,
  `sort_order = n`, `is_folder = false`, `name`.
- For each `playlist_tracks` row (ordered by `sort`) → an entry row:
  `entry_index = sort`, `track_id = pdb track id for pos`, `playlist_id = tree id`.
- (Folders/nesting: out of scope; all playlists at root. `is_folder=true` rows +
  child `parent_id` would add nesting later.)

### Plumbing
Playlists are **not per-track**, so they don't fit a single-column add cleanly.
Two options:

- **A — carry membership on the track row (keeps pure COPY):** add a
  `playlists LIST(STRUCT(name, sort))` column to `rb_deck`; `RbFinalize` builds
  the tree from the distinct set of names and the entries from
  `(track, name, sort)`. Simple, but a playlist with **zero** exported tracks
  can't be represented.
- **B — a finalize-time reader (recommended):** `RbFinalize` opens the same
  DuckDB and `SELECT`s `playlists` + `playlist_tracks` directly, so empty
  playlists and exact ordering survive. Needs the sink to know the DB path
  (pass via a `COPY` option, e.g. `(FORMAT rekordbox, PLAYLISTS true)`).

Map `pos → pdb track id` from the id assignment already done in `pdb_writer.cpp`.

**Effort:** low–medium (helpers exist; mostly loop over two tables + id mapping).

---

## 4. Album artwork — pdb `artwork` table (`tabs[13]`) + `PIONEER/Artwork/`  🔴

rekordbox stores cover images as files under `PIONEER/Artwork/` and references
them from the track row via `artwork_id` (already reserved at `u4 @ 0x1c`) into
the **artwork table (pdb page type 13)**.

### Steps
1. For each track, `blob = rb_artwork(path)`; skip if empty.
2. Write the bytes to a file, e.g. `PIONEER/Artwork/00001/<track_id>.jpg`
   (rekordbox buckets by `id/1000`; a flat dir also loads). Convert PNG→JPEG if a
   player is picky (CDJs generally accept both; start by writing bytes as-is with
   the right extension from the magic — `ffd8`=jpg, `8950`=png).
3. Add an **artwork table row** (`tabs[13]`): `id u4` + DeviceSQL `path` string
   = the USB-relative `/PIONEER/Artwork/…` path.
4. Set the track row `artwork_id @ 0x1c` = that id.
5. (Optional) rekordbox also writes small thumbnails; CDJs fall back to the full
   image, so a thumbnail table isn't required for a first pass.

### Plumbing
Add an `art BLOB` column to `rb_deck` (`rb_artwork(path)`); `RbFinalize` dedups
identical images (hash → shared artwork id), writes the files + artwork rows, and
back-patches `artwork_id` on the matching track rows.

**Effort:** medium. Needs the artwork table page (type 13) — confirm the row
layout (`id u4`, `path` DeviceSQL string) against `rekordcrate` / a real
`export.pdb`, and confirm `artwork_id` really lives at `0x1c`.

---

## 5. Suggested order

1. **Cues (PCOB/PCO2)** — biggest DJ payoff, per-track, fits the COPY model.
2. **Artwork** — per-track BLOB + `Artwork/` files + `tabs[13]` + `artwork_id`.
3. **Playlists** — finish `tabs[7]/[8]` from `playlists`/`playlist_tracks`.
4. **PQT2 / PSSI** — newer-player extras (extended grid + phrase/song-structure).

Each phase is independently shippable and validates against `test/reference/`
(real ANLZ + `export.pdb` ground truth) the same way the current writer was.

## 6. References
- Deep Symmetry rekordbox-export-analysis (`anlz.html`, `exports.html`), crate-digger.
- `Holzhaus/rekordcrate` (Rust structs for pdb pages incl. artwork type 13, PCO2).
- `specs/rekordbox_anlz.ksy`, `specs/rekordbox_pdb.ksy`.
- Ground truth: `test/reference/ANLZ0000.{DAT,EXT,2EX}`, `test/demo_export.pdb`.
- Writers to extend: `src/anlz_writer.cpp` (tags), `src/pdb_writer.cpp` (tables + sink/finalize).
