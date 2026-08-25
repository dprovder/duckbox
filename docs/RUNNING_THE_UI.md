# Running duckbox, the library UI

```zsh
cd ~/Music/rekordbox-duck
ext-rekordbox/ui/duckbox
```

Opens <http://127.0.0.1:9999/> in your browser. Ctrl-C stops it.

There is no Python and no second server. The `duckdb` binary has the
`httpserver` extension compiled in alongside the `rb_*` analysis functions, and
it serves both the app page and every byte of data. The UI is one file,
`ext-rekordbox/ui/app.html`: vanilla JS and a canvas, no dependencies.

## Arguments

```zsh
ext-rekordbox/ui/duckbox /path/to/other.duckdb    # store (default ../library.duckdb)
PORT=9001 ext-rekordbox/ui/duckbox                # port  (default 9999)
```

The launcher kills anything already listening on the port, waits for the server
to answer, then opens your browser.

## Prerequisites

**A built binary** at `ext-rekordbox/build/release/duckdb`:

```zsh
cd ext-rekordbox
OPENSSL_ROOT_DIR=$(brew --prefix openssl@3) GEN=ninja make release EXT_FLAGS=-DWITH_ESSENTIA=ON
```

See [../BUILD.md](../BUILD.md). OpenSSL is needed because `httpserver` looks for
it via `find_package`.

**A store.** If `library.duckdb` does not exist yet, build it from an audio
folder — an audio path is queryable as a table:

```zsh
ext-rekordbox/build/release/duckdb library.duckdb -f ext-rekordbox/sql/analyze.sql
```

Edit the folder path at the top of `sql/analyze.sql` first. Analysis is about
13 s/track serial; to use all cores, `sql/analyze_parallel.sh ~/Music/Hau5
library.duckdb 8` shards across processes (Essentia is **not** thread-safe, so
parallelism has to be across processes, never threads).

Once a store exists you can grow it from inside the UI with **Analyze folder**,
which appends and dedupes by filename.

## What you get

- **Browse** — sortable, filterable track table: BPM range, energy, search.
- **Harmonic mixing** — a clickable Camelot wheel (inner ring minor, outer
  major) that filters to compatible keys, plus scored mix suggestions weighting
  key, tempo (half/double aware) and energy.
- **Waveform** — rekordbox-style three-band colour render with the beat grid and
  downbeat overlaid. Scroll to zoom, drag to pan, click to seek, space to
  play/pause. `⌖ follow` locks the view to the playhead.
- **Grid editing** — the playhead deliberately does *not* snap, because the
  detected grid is often the thing that is wrong. Put the playhead on the true
  beat 1 and press **beat 1 ← playhead**; nudge by ±3/±10 ms; trim BPM by ±0.1.
  Saves a fixed grid back to `track_analysis`.
- **Cues** — auto cues suggested from beat loudness (In / Drop / Break) as ghost
  chips you can keep, plus manual beat-snapped cues. Stored in the `cues` table
  and exported as ANLZ memory cues.
- **Playlists** — sidebar, drag to reorder, rename, delete.
- **Export USB** — tick tracks, press the button. See
  [USB_EXPORT.md](USB_EXPORT.md).
- **Album art**, per-track **reanalyze**, and a settings modal.

The store is opened read-write: edits to grids, cues and playlists persist
immediately.

## Notes

- **Run it in the foreground.** The server thread lives inside a `duckdb -c`
  invocation kept alive by `DUCKDB_HTTPSERVER_FOREGROUND=1`. Backgrounding it
  with `&` or `nohup` tends to get the process reaped.
- **It binds `127.0.0.1` only** — not reachable from the network, which is what
  you want given it will execute SQL for anyone who can reach it.
- **Audio streams from your local disk** over `GET /media/<absolute-path>` with
  HTTP range support, so the store keeps absolute paths and is not portable
  between machines as-is.
- **Everything over HTTP arrives as a string**, including ids and numbers —
  relevant only if you are editing `app.html`.
