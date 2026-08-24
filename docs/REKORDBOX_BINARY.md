# Reading rekordbox instead of probing it

`/Applications/rekordbox 7/rekordbox.app/Contents/MacOS/rekordbox` ships
**unstripped**, with `OSO` debug entries naming all 2,549 original source files.
Everything below came out of it in one sitting, after weeks of black-box work
had stalled on the same questions.

Method:

    lipo -thin arm64 -output rbx rekordbox      # it is a fat binary
    nm -n rbx | grep -i <thing>                 # symbols are not stripped
    nm -ap rbx | awk '/OSO/{b=($NF~/x\.o$/)} b&&/ FUN /{print}'   # per-object function lists
    objdump -d --start-address=.. --stop-address=.. rbx

`objdump --disassemble-symbols` silently ignores its filter on the fat binary —
thin the slice first and use explicit address ranges.

## The analysis-path hash — solved

See `docs/ANLZ_PATH.md`. `analyzer::CreateAnlzFileFolderPath`.

## The pdb table names — solved

See `docs/PDB_TABLES.md`. DeviceSQL compiles its schema to C and leaves an
`__epl_<TABLE>_create` symbol per table.

## PSSI's mask — explained, and it was already public

`analyzer::ConvUSB_ScrambleSongStruct(unsigned char*)`. The 19 mask bytes that
Deep Symmetry documents are not arbitrary: they are the ASCII of

    "Kanzen-niRikaishita"        # 完全に理解した — "completely understood"

XOR 0x80. The function loads that string, then for each byte adds `n` and flips
bit 7 — where `n` is the byte at tag offset `0x11`, the low byte of the
big-endian `len_entries`. Verified to reproduce the documented mask exactly, for
every `n`:

    mask[i] = ((K[i] ^ 0x80) + n) & 0xFF

The compiler folds element 0 into `n - 0x35`, which is the same thing:
`0xCB ≡ -0x35 (mod 256)`.

So this confirms rather than unlocks — but it names the field that supplies `n`,
and turns nineteen magic bytes into one line. `ConvUSB_ScrambleVocalDetect` is a
second, *undocumented* scrambled structure (vocal detection, rekordbox 7 era),
should we ever want it.

## Beat detection is a neural network — do not reverse it, replace it

`Resources/models/detect_beat/model7|model8/` are TensorFlow SavedModels, and
`libtensorflow_cc` / `libonnxruntime` are linked in. Disassembling `Beat4Ana` or
`AnalyzeJobBeatDetector` would show tensor plumbing, not a method — the method is
420 KB of weights. Reading the SavedModel's own configuration is far more useful:

| layer | config |
|---|---|
| `STFTLayer` ×3 | `frame_length` 1024 / 2048 / 4096, all `frame_step` **441** |
| `FilteredSpectrogram` | filtered log-magnitude |
| `bidirectional` ×3 | stacked BiRNN |
| `dense` | output activations |

`frame_step` 441 at 44.1 kHz is **100 frames/sec**, and three parallel frame
sizes into a filtered spectrogram into stacked bidirectional RNNs is, feature for
feature, **madmom's `RNNDownBeatProcessor`**. Pioneer did not invent a secret
algorithm; they trained the standard one.

The actionable conclusion: for the downbeat gap, use madmom's DBN downbeat
tracker (or Beat-Transformer) rather than Essentia's meter estimator or our
kick-phase heuristic. Same input representation, same architecture family, open
weights, and ours to ship.

Also present, for context: `detect_cue_model/samplecnn.onnx` (rekordbox's
auto-cue) and `embed_model/{cnn_embedding,model_genre,model_energy,umap_embedder}.onnx`
(track embeddings behind related-track suggestions).

## The colour waveform — decoded

`analyzer::WaveCreator::calcZoomWaveColour(float low, float mid, float high)`,
whose only caller is `createColorWaveData`, feeding `MstStoreColorZoomWave`. So
it is the stored colour, not a display tint:

```
mx = max(low, mid, high)
if mx == 0: return white                  # silence renders white
k = 255/mx;  r = low*k;  g = mid*k;  b = clamp(high*k, 0, 255)
if b < 64:                                # weak highs: pull r and g down together
    t = (g/765) * r * (1 - b/64)          # 765 == 3*255
    r -= t;  g -= t
m = min(max(r, b), 128)
g *= (1 - 0.3*m/128)                      # green loses up to 30% to the louder of r/b
b *= 1.3
return Colour(clamp(r), clamp(g), clamp(b))
```

`MstStoreColorZoomWave` then packs a 4-byte `{r3,g3,b3,height5}` element as
`red<<13 | green<<10 | blue<<7 | height<<2`, big-endian — byte for byte what we
already emit. The 8-to-3-bit reduction is a plain `>>5`.

Implemented in `anlz_writer.cpp`. **Not decoded:** `createColorWaveData`
special-cases whichever channel is the maximum (a `csinc` chain after the call),
so columns dominated by one band may still differ slightly.

**This does not change what we ship today.** `BuildAnlzExtBytes` emits only
`PPTH` + `PWV3` for CDJ-2000NXS compatibility, so `PWV5` — and therefore all of
the above — is dead code until the colour tags are re-enabled for a CDJ-3000.

## PWV3 whiteness — traced to a dead end, usefully

The chain, followed all the way down:

| function | what it actually does |
|---|---|
| `MstStoreZoomWave` | serialises only. 16-byte element, height `+0x08`, whiteness `+0x0C`, packed `(whiteness << 5) \| (height & 0x1f)` |
| `BALibWrapper::GetZoomWave` | version dispatch to `BeatAnalyzer_1_0` / `_2_0`, nothing else |
| `BA_GetZoomWave` | widens **two int16 per column** (4-byte stride, `ld2.4h`) to two floats |
| `Analyzer::getZoomWave` | a plain accessor: returns the buffer at `Analyzer+0x3b30`, count at `+0x3b2c` |

So whiteness is **not** a post-hoc function of three band magnitudes that we can
port. The BeatAnalyzer engine emits exactly two numbers per column — height and
whiteness — as products of its own analysis pass, and everything downstream just
moves them around.

That reframes the problem rather than solving it. There is no formula to lift;
matching rekordbox means matching the *distribution* its DSP produces, which is
precisely what `WhitenessSeries()` already does by rank-mapping each column
against its own track. The existing approach is the right shape, and improving it
means better calibration data, not more disassembly.

If someone does want the real thing, the tractable route is dynamic, not static:
the buffer at `Analyzer+0x3b30` is a plain array with its count next to it, so
observing it during an analysis run beats decompiling the pass that fills it.

Also still open: `MstStoreWholeWave` for the overview waveform's height/colour
gain, and the `ConvUSB_*` endian converters, each of which carries an exact
struct layout (`WAVE_InfHeader`, `QUANTIZE_InfHeader`, `QUANTIZE_EXT_InfHeader`
= `PQT2`, `CUE_ExtPointInfo`).
