#pragma once
#include "duckdb.hpp"
#include "audio_decode.hpp"

#include <string>
#include <vector>

namespace duckdb {

// Unified per-track features: one decode, every analyzer. Backs both rb_analyze
// and the rb_analyze_dir table function. With Essentia: deep rhythm + downbeat +
// danceability; without it, aubio tempo only.
struct TrackFeatures {
	bool ok = false;
	double bpm = 0, confidence = 0, downbeat_sec = 0, ibi_stddev_ms = 0, onset_rate = 0, danceability = 0;
	int32_t beat_count = 0, meter = 0, downbeat_index = -1;
	std::string key_camelot;
	double lufs = -70, true_peak = -70, lra = 0;
	std::vector<double> beats, beat_loudness, kick_pattern;
};
TrackFeatures AnalyzeTrack(const std::string &path); // decodes `path` once, runs all analyzers

#ifdef HAVE_ESSENTIA
// Fills the Essentia-derived fields (rhythm + danceability) from decoded audio.
// Implemented in essentia_analysis.cpp; called by AnalyzeTrack.
void FillEssentiaFeatures(const rbx::Audio &audio, TrackFeatures &out);
#endif

// Scalar UDFs: each takes a file path (VARCHAR) and analyzes the audio.
void RbBpmFun(DataChunk &args, ExpressionState &state, Vector &result);       // -> DOUBLE
void RbKeyFun(DataChunk &args, ExpressionState &state, Vector &result);       // -> VARCHAR (Camelot)  [DONE]
void RbBeatgridFun(DataChunk &args, ExpressionState &state, Vector &result);  // -> DOUBLE[]
void RbLoudnessFun(DataChunk &args, ExpressionState &state, Vector &result);  // -> STRUCT
// One-decode, all-features. -> STRUCT(bpm,key,beatgrid,lufs,true_peak,lra). Use this.
void RbAnalyzeFun(DataChunk &args, ExpressionState &state, Vector &result);

// Essentia-backed UDFs (src/essentia_analysis.cpp). Always registered; without
// WITH_ESSENTIA they return zero/empty. RbRhythm gives the accurate beat-tick
// list + confidence used to build the ANLZ PQTZ tempo grid.
void RbRhythmFun(DataChunk &args, ExpressionState &state, Vector &result);       // -> STRUCT(bpm,confidence,beats DOUBLE[])
void RbRhythmDeepFun(DataChunk &args, ExpressionState &state, Vector &result);   // -> STRUCT(meter,downbeat,kick_pattern,...)
void RbDanceabilityFun(DataChunk &args, ExpressionState &state, Vector &result); // -> DOUBLE
#ifdef HAVE_ESSENTIA
void EssentiaInit(); // call once in LoadInternal before registering UDFs
#endif

} // namespace duckdb
