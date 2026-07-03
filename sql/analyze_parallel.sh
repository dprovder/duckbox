#!/bin/bash
# Parallel library analysis by PROCESS sharding.
#
# rb_analyze_dir is single-threaded on purpose: Essentia's global AlgorithmFactory
# is NOT thread-safe — analysing on multiple threads in one process corrupts
# results AND runs slower than serial (contention). Separate processes each get
# their own Essentia state, so N workers over the same pattern with shard=0..N-1
# (the `shard`/`shards` named params partition the file list) give a clean ~Nx
# speedup. Each worker writes parquet; then we merge into the store.
#
# Usage:  sql/analyze_parallel.sh [AUDIO_DIR_OR_GLOB] [STORE.duckdb] [N]
set -uo pipefail
PAT="${1:-$HOME/Music/Hau5}"
STORE="${2:-library.duckdb}"
N="${3:-$(sysctl -n hw.ncpu 2>/dev/null || echo 8)}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
DUCKDB="$HERE/build/release/duckdb"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

t0=$(date +%s)
for k in $(seq 0 $((N-1))); do
  "$DUCKDB" -c "
    CREATE TEMP TABLE ta AS SELECT * FROM rb_analyze_dir('$PAT', shard=$k, shards=$N);
    COPY ta TO '$WORK/s$k.ta.parquet' (FORMAT parquet);
    COPY (SELECT pos, w.* FROM (SELECT pos, rb_waveform(path) AS w FROM ta)) TO '$WORK/s$k.wf.parquet' (FORMAT parquet);
  " >/dev/null 2>&1 &
done
wait
echo "analysed in $(($(date +%s)-t0))s across $N processes"

# Merge shards into the store + rebuild the derived tables.
"$DUCKDB" "$STORE" -c "
CREATE OR REPLACE TABLE track_analysis AS SELECT * FROM read_parquet('$WORK/*.ta.parquet');
CREATE OR REPLACE TABLE waveform AS SELECT * FROM read_parquet('$WORK/*.wf.parquet');
CREATE OR REPLACE TABLE beatgrid AS
  SELECT pos, (i-1)::INT AS beat_index, round(t*1000)::INT AS time_ms,
         ((((i-1)-greatest(downbeat_index,0))%4+4)%4+1)::USMALLINT AS beat_number,
         round(bpm*100)::USMALLINT AS tempo
  FROM track_analysis, unnest(beats) WITH ORDINALITY AS u(t,i);
" >/dev/null 2>&1
echo "store '$STORE' built: $("$DUCKDB" "$STORE" -noheader -list -c 'SELECT count(*) FROM track_analysis') tracks"
