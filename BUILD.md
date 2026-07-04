# Building duckbox from source

`duckbox` is a single self-contained DuckDB binary: the DJ analysis UDFs
(`rb_*`), the `httpserver` extension, and Essentia are all compiled **in**, so
the app is served with no Python and no separate server. This is the from-scratch
build. To just hand someone a ready-to-run copy on Apple Silicon, see
[§5 Portable bundle](#5-portable-bundle) instead.

Tested on macOS (Apple Silicon). Linux/Intel should work with the equivalent
packages; adjust `OPENSSL_ROOT_DIR` and Homebrew paths.

## 1. Prerequisites (Homebrew)

```sh
brew install cmake ninja pkg-config \
             ffmpeg libkeyfinder aubio libebur128 openssl@3 fftw eigen
```

`ffmpeg` (decode + artwork), `libkeyfinder` (key), `aubio` (fallback beats),
`libebur128` (loudness) are linked via pkg-config / `find_library`. `openssl@3`
is needed by `httpserver`. `fftw`+`eigen` are for Essentia.

## 2. Clone + submodules

```sh
git clone <this-repo> rekordbox-duck
cd rekordbox-duck/ext-rekordbox
git submodule update --init --recursive        # duckdb + extension-ci-tools
```

## 3. Vendored dependencies (one-time)

### Essentia (lightweight static, KISS FFT — no ffmpeg/taglib/gaia/TF)

```sh
git clone https://github.com/MTG/essentia ../deps/essentia
cd ../deps/essentia
PKG_CONFIG_PATH="$(brew --prefix)/lib/pkgconfig" python3 waf configure \
    --build-static --lightweight= --fft=KISS --prefix="$PWD/install"
python3 waf -j"$(sysctl -n hw.ncpu)"           # ~2 min
python3 waf install                            # -> install/lib/libessentia.a
cd ../../ext-rekordbox
```

### httpserver (built in-tree — `INSTALL FROM community` 404s on this dev binary)

```sh
git clone https://github.com/Query-farm/httpserver ../deps/httpserver
# its CMake references duckdb/third_party/httplib relative to itself:
ln -s ../../ext-rekordbox/duckdb ../deps/httpserver/duckdb
# apply our dev-API-drift + segfault + media-mount + app-index patches:
git -C ../deps/httpserver apply "$PWD/patches/httpserver-duckbox.patch"
```

`extension_config.cmake` already registers it via
`duckdb_extension_load(httpserver SOURCE_DIR ../deps/httpserver)`.

## 4. Build

```sh
OPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" \
GEN=ninja make release EXT_FLAGS=-DWITH_ESSENTIA=ON
```

Output: `build/release/duckdb` — a static binary with `rb_*` + `httpserve_*`
compiled in (verify: `build/release/duckdb -c "SELECT * FROM duckdb_functions()
WHERE function_name LIKE 'rb_%' OR function_name LIKE 'httpserve%'"`).

Run the app:

```sh
ui/duckbox [store.duckdb]          # store defaults to ../library.duckdb
```

Build a library from a folder of audio (if you don't have one):

```sh
build/release/duckdb library.duckdb -c \
  "CREATE TABLE track_analysis AS SELECT * FROM rb_analyze_dir('/path/to/Music');
   CREATE TABLE waveform AS SELECT pos, w.* FROM (SELECT pos, rb_waveform(path) AS w FROM track_analysis);"
# (see dist/duckbox-mac-arm64/analyze.sql for the full pipeline incl. rb_deck)
```

## 5. Portable bundle

Turn the dynamically-linked binary into a folder that runs on **any** Apple
Silicon Mac with no Homebrew:

```sh
brew install dylibbundler
B=dist/duckbox-mac-arm64
mkdir -p "$B/ui"
cp build/release/duckdb "$B/duckbox"
cp ui/app.html "$B/ui/app.html"
dylibbundler -od -b -x "$B/duckbox" -d "$B/libs/" -p @executable_path/libs/
# add analyze.sql, run.sh, README.txt (see the committed-out dist/ layout), then:
cd dist && zip -ry duckbox-mac-arm64.zip duckbox-mac-arm64
```

`dylibbundler` copies every non-system dylib (incl. ffmpeg's tree, ~72 libs) into
`libs/` and rewrites the load paths to `@executable_path/libs/`, then ad-hoc
re-signs. The recipient unzips and runs `./run.sh "/their/Music"`. It's unsigned,
so first launch may need `xattr -dr com.apple.quarantine <folder>`.

## Notes

- Essentia's global `AlgorithmFactory` is **not thread-safe** — analyze in
  parallel across *processes* (`rb_analyze_dir(pat, shard=k, shards=N)` /
  `sql/analyze_parallel.sh`), never with `MaxThreads>1`.
- The store keeps **absolute audio paths** (playback + artwork read files
  directly), so it's machine-specific — each user analyzes their own library.
