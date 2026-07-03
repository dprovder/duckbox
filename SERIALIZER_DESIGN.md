# `.pdb` + ANLZ serializer — design to implement next

Goal: make `COPY tracks TO '/Volumes/USB' (FORMAT rekordbox)` write a CDJ-mountable
Pioneer tree. The DuckDB plumbing (`bind`/`sink`/`combine`/`finalize`) is already wired
in `src/pdb_writer.cpp`; only the byte serialization in `PdbBuilder::Build` and
`BuildAnlz` is TODO.

**Ground truth to validate against (all present in this repo):**
- Byte layouts: `specs/rekordbox_pdb.ksy`, `specs/rekordbox_anlz.ksy`
- Compiled parsers: `../src/parsers/rekordbox_pdb.py`, `rekordbox_anlz.py`
- Real reference DBs: `test/demo_export.pdb` (has tracks), `test/empty_export.pdb`
- Phase-A field notes: `../NOTES.md`

## 0. What `finalize` receives
`RbGlobalState.tracks` — one `RbTrack` per input row (title/artist/genre/key/path/bpm/
duration/file_size/beatgrid). The bind step still needs to map input columns → these
fields (currently a TODO in `RbBind`); decide a required column contract, e.g.
`COPY (SELECT title, artist, genre, key, bpm, duration_sec, path, beatgrid FROM ...)`.

## 1. `export.pdb` layout (DeviceSQL)
- File = N × 4096-byte pages. **Header page (index 0):**
  `u4=0, len_page=4096, num_tables=20, next_unused_page, u4=?, sequence, gap[4]`,
  then `num_tables` × `table{ u4 type, u4 empty_candidate, u4 first_page, u4 last_page }`.
- **20 tables, in this order** (type enum value = index):
  tracks, genres, artists, albums, labels, keys, colors, playlist_tree,
  playlist_entries, unknown_9, unknown_10, history_playlists, history_entries,
  artwork, unknown_14, unknown_15, columns, unknown_17, unknown_18, history.
  Empty tables still need a (mostly-empty) page each — copy the shapes from
  `empty_export.pdb`.
- **Each table = linked list of pages.** rekordbox's convention: `first_page` is a
  "strange"/garbage page with 0 rows; real rows start on the next page. Simplest correct
  approach: mimic `demo_export.pdb` — one empty first page + data pages.

### Page (data) structure
- Header (0x28 bytes): `gap[4]=0, u4 page_index, u4 type, page_ref next_page, u4 sequence,
  4 bytes, b13 num_row_offsets + b11 num_rows + u1 page_flags, u2 free_size, u2 used_size,
  u2 tx_row_count, u2 tx_row_index, u2, u2`.
- Then a **heap** growing forward from offset 0x28, holding row structs + strings.
- A **row index growing BACKWARDS from the end of the page**: groups of 16 rows. Each
  `row_group` = 16 × u2 offsets (relative to heap start = page+0x28) preceded by a u16
  present bitmask. `num_row_groups = (num_row_offsets-1)/16 + 1`. Build this last, after
  you know each row's heap offset.

### Track row (fixed part, then strings)
`u2 subtype=0x24, u2 index_shift, u4 bitmask, u4 sample_rate, u4 composer_id,
u4 file_size, u4 ?, u2 =19048, u2 =30967, u4 artwork_id, u4 key_id, u4 orig_artist_id,
u4 label_id, u4 remixer_id, u4 bitrate, u4 track_number, u4 tempo(=bpm*100),
u4 genre_id, u4 album_id, u4 artist_id, u4 id, u2 disc, u2 play_count, u2 year,
u2 sample_depth, u2 duration(sec), u2 =41, u1 color_id, u1 rating, u2 =1, u2 (2|3),
u2 ofs_strings[21]`.
The 21 offsets point (relative to row start) to `device_sql_string` heap entries.
String slots (index → purpose), from real data:
`0 isrc, 1 texter, 2..8 unknown, 9 kuvo_public, 10 mix_name, 11 comment(? verify order
against parser), 12 release_date, 13 analyze_path, 14 analyze_date, 15 date_added,
16 autoload_hot_cues='ON', 17 message, 18 kuvo?, 19 file_path='/Contents/...', 20 filename,`
+ title. **Verify exact indices by parsing `demo_export.pdb` and printing `ofs_strings`
alongside each string** — don't trust this list blindly.

### device_sql_string encoding
Two forms (see `specs` types `device_sql_short_ascii` / `device_sql_long_ascii` /
`_long_utf16`):
- **short ASCII**: 1 length byte `(len<<1)|1`, then ASCII bytes.
- **long**: `0x40` marker, u2 length, u1 kind, then payload (ASCII or UTF-16LE).
Use short for ≤127-char ASCII; long-utf16 when non-ASCII (accents like "Andrés").

### id tables (artists/keys/genres/albums/labels/colors)
Intern distinct values → sequential ids (1-based). Each row is tiny: e.g.
`artist_row{ subtype, index_shift, u4 id, u1 ofs_name, device_sql_string name }`.
Track row references them by id (0 = none). Key names: store Camelot or musical — check
what rekordbox expects (`keys` table rows in `demo_export.pdb` show the format).

### playlists
`playlist_tree` = folder/list nodes; `playlist_entries` = (playlist_id, track_id, seq).
Create one list named after the source, all tracks in order.

## 2. ANLZ files (per track)
`analyze_path` in the track row points at `/PIONEER/USBANLZ/Pxxx/xxxxxxxx/ANLZ0000.DAT`.
Generate that folder hierarchy (the hex dirs are derived from the track id) and write:
- **PMAI** file header (magic `PMAI`, len_header, len_file).
- **PPTH** — the track's on-USB path (UTF-16BE).
- **PQTZ** — beatgrid: for each beat, `(u2 beat_number 1-4, u2 tempo*100, u4 time_ms)`.
  Feed from `RbTrack.beatgrid` (from `rb_beatgrid`). Beat numbers cycle 1..4.
- **PWAV/PWV2** (preview) + **PWV4/PWV5** (color/scroll waveform) — downsampled
  amplitude/color. Can be minimal at first (CDJ tolerates missing detail better than a
  missing beatgrid).
- **PCOB/PCO2** — memory/hot cues; empty is fine initially.
The `.EXT` file mirrors `.DAT` with the extended (color-waveform, extended cues) tags.

## 3. Build order (incremental, always round-trip-validated)
1. **Header + 20 empty tables** → parse with `rekordbox_pdb.py`, assert num_tables=20.
2. **keys/genres/artists id tables** with a couple rows → parse, read names back.
3. **tracks table**, one row, all ids=0, only title/filename/file_path/tempo/duration →
   parse, diff the row vs a `demo_export.pdb` row field-by-field.
4. Wire id references (artist/key/genre) → parse, confirm joins resolve.
5. **playlist_tree + playlist_entries** → parse, confirm list + entries.
6. **ANLZ PMAI+PPTH+PQTZ** (beatgrid) → parse with `rekordbox_anlz.py`.
7. Point track `analyze_path` at the ANLZ, write full tree.
8. **HARDWARE GATE**: mount USB in a CDJ. Iterate on whatever it rejects.

## 4. Gotchas caught in Phase A
- `sequence` in header is "next"; each page copies it before increment.
- First page of each table is intentionally junk with 0 rows.
- The total row count YouTube... (n/a) — for us, watch `free_size`/`used_size` must be
  consistent with the heap+index or the CDJ may reject the page.
- Multi-byte: everything little-endian **except** ANLZ strings which are UTF-16**BE**.
- Validate every step with the Python parsers before moving on; a malformed `.pdb` makes
  the CDJ silently ignore the whole USB.
