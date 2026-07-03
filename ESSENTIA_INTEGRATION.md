# Essentia integration — high-level descriptors (next session)

aubio + libKeyFinder + libebur128 (all wired now) give BPM / beatgrid / key / loudness.
**Essentia** is the one that adds *semantic* descriptors: danceability, mood
(happy/sad/aggressive/relaxed), genre, voice/instrumental, dynamic complexity, onset rate,
tuning — the AcousticBrainz feature set. It's pure C++, so it links into this extension the
same way the others do. The only cost is that it's a **source build** (no brew/vcpkg).

Source + all deps are already staged: `../deps/essentia`, and brew has
`fftw eigen libyaml libsamplerate taglib chromaprint ffmpeg`.

## Build libessentia (one-time)
```sh
cd ../deps/essentia
python3 waf configure --build-static --lightweight= --fft=KISS \
    --prefix=$PWD/install            # add --with-gaia for SVM high-level models
python3 waf -j$(sysctl -n hw.ncpu)
python3 waf install
# -> ../deps/essentia/install/{include,lib/libessentia.a}
```
Apple-Silicon notes: if `waf configure` fails on a dep, point it at brew with
`PKG_CONFIG_PATH=/opt/homebrew/lib/pkgconfig`. The high-level *models* (mood/genre) need
either `--with-gaia` (classic SVM, `brew install gaia` — may need source) or the
TensorFlow build; low-level + danceability/BPM/key work without either.

## Wire into CMake (hook already present)
`CMakeLists.txt` already has `option(WITH_ESSENTIA ...)`. Update it to point at the local
build:
```cmake
if(WITH_ESSENTIA)
  find_library(ESSENTIA_LIB essentia PATHS ${CMAKE_SOURCE_DIR}/../deps/essentia/install/lib REQUIRED)
  find_path(ESSENTIA_INC essentia/algorithmfactory.h PATHS ${CMAKE_SOURCE_DIR}/../deps/essentia/install/include REQUIRED)
  add_compile_definitions(HAVE_ESSENTIA)
endif()
```
Then `make release WITH_ESSENTIA=ON`.

## New UDFs (guard with `#ifdef HAVE_ESSENTIA`)
Put these in a new `src/essentia_analysis.cpp` so the default build stays lean.

```cpp
// rb_danceability(VARCHAR) -> DOUBLE (0..3-ish; higher = more danceable)
// rb_descriptors(VARCHAR)  -> STRUCT(danceability, dynamic_complexity, onset_rate,
//                                    bpm_confidence, key, key_strength, tuning_hz)
#include <essentia/algorithmfactory.h>
#include <essentia/pool.h>
using namespace essentia; using namespace essentia::standard;

essentia::init();  // ONCE per process — do it in the extension Load(), essentia::shutdown() never needed
AlgorithmFactory& F = AlgorithmFactory::instance();
// Decode with our rbx::DecodeMono (already returns mono float @ 44100) and feed:
std::vector<Real> audio(a.samples.begin(), a.samples.end());
Algorithm* dance = F.create("Danceability");
Real danceability, ddummy; std::vector<Real> dfx;
dance->input("signal").set(audio);
dance->output("danceability").set(danceability);
dance->output("dfa").set(dfx);
dance->compute();
```
Best path for the full set: use **`MusicExtractor`** (Essentia's batch descriptor pipeline)
which fills a `Pool` with everything at once — then map the Pool values into a DuckDB
`STRUCT`. One decode, ~all descriptors. This becomes the efficient `rb_descriptors(path)`
that supersedes calling rb_bpm/rb_key/etc separately.

## Register (in `rekordbox_extension.cpp` LoadInternal)
```cpp
essentia::init();  // safe to call once here
loader.RegisterFunction(ScalarFunction("rb_danceability", {LogicalType::VARCHAR},
                        LogicalType::DOUBLE, RbDanceabilityFun));
```

## Payoff query it unlocks
```sql
SELECT title, rb_key(path), rb_bpm(path), rb_danceability(path) AS dance
FROM lib
WHERE rb_key(path) IN ('8A','9A') AND rb_bpm(path) BETWEEN 124 AND 128
ORDER BY dance DESC;   -- peak-time, harmonically-locked, most danceable first
```

## Reality check
- danceability / dynamic-complexity / BPM-confidence / tuning: **work from the plain
  static lib**, no models. Wire these first — biggest value, least pain.
- mood / genre classifiers: need gaia (SVM) or TF models — a second, optional step.
- Essentia's own `KeyExtractor` and `RhythmExtractor2013` can eventually *replace* our
  aubio/libKeyFinder paths if we want a single engine — but no need; keep what works.
