-- Build the rekordbox analysis store from an audio folder — plain idiomatic SQL.
--
-- An audio folder/glob is queryable AS a table: `FROM '~/Music/Hau5'` routes to
-- the analyzer the same way `FROM 'file.csv'` routes to read_csv (a replacement
-- scan; the explicit form rb_analyze_dir('~/Music/Hau5') works too). So there is
-- no schema to declare and no script to source — each statement below is
-- standalone. Run them however you like: paste into a session, `duckdb -c "..."`,
-- or `duckdb library.duckdb -f src/analyze.sql`. CREATE OR REPLACE is idempotent.
--
-- Then export the whole USB in one line:
--     COPY rb_deck TO '/Volumes/USB' (FORMAT rekordbox);

-- 1. Per-track analysis, straight from the folder (deep rhythm + downbeat + key
--    + loudness, one decode per file). Use a glob like '~/Music/Hau5/00[1-3]*.m4a'
--    to analyze a subset while iterating.
CREATE OR REPLACE TABLE track_analysis AS
  SELECT * FROM '~/Music/Hau5';

-- 2. Waveforms at 150 col/s (a second, lighter decode). The struct-returning
--    rb_waveform is unnested with .* into height/low/mid/high columns.
CREATE OR REPLACE TABLE waveform AS
  SELECT pos, w.* FROM (SELECT pos, rb_waveform(path) AS w FROM track_analysis);

-- 3. Beat grid = the PQTZ entries, one row per beat, phased off the downbeat.
CREATE OR REPLACE TABLE beatgrid AS
  SELECT pos,
         (i - 1)::INT AS beat_index,
         round(t * 1000)::INT AS time_ms,
         ((((i - 1) - greatest(downbeat_index, 0)) % 4 + 4) % 4 + 1)::USMALLINT AS beat_number,
         round(bpm * 100)::USMALLINT AS tempo
  FROM track_analysis, unnest(beats) WITH ORDINALITY AS u(t, i);

-- 4. The single export surface: everything a rekordbox USB needs, joined once,
--    so the export is `COPY rb_deck TO '/Volumes/USB' (FORMAT rekordbox)`.
--    title/artist are derived from the "NN - Artist - Title [label].ext" filename
--    (swap in a JOIN to your own metadata table for richer album/label/genre).
CREATE OR REPLACE VIEW rb_deck AS
WITH base AS (
  SELECT ta.*, regexp_extract(ta.path, '([^/]+)\.[^.]+$', 1) AS stem,
         w.height, w.low, w.mid, w.high
  FROM track_analysis ta JOIN waveform w USING (pos)
)
SELECT
  trim(regexp_replace(stem, '^\s*\d+\s*-\s*[^-]+-\s*', '')) AS title,
  trim(regexp_extract(stem, '^\s*\d+\s*-\s*([^-]+?)\s*-', 1)) AS artist,
  bpm, key_camelot AS key,
  round(beats[len(beats)])::BIGINT AS duration_sec,
  path, regexp_extract(path, '([^/]+)$', 1) AS filename,
  beats, downbeat_index, height, low, mid, high
FROM base;
