#include "analysis.hpp"
#include "audio_decode.hpp"

#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector/constant_vector.hpp"

#include <keyfinder/keyfinder.h>
#include <keyfinder/audiodata.h>
#include <aubio/aubio.h>
#include <ebur128.h>

#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

namespace duckdb {

// key_t enum index -> Camelot code (ordering per libKeyFinder constants.h).
static const char *CAMELOT[25] = {
    "11B","8A","6B","3A","1B","10A","8B","5A","3B","12A","10B","7A","5B",
    "2A","12B","9A","7B","4A","2B","11A","9B","6A","4B","1A",""};

//===--------------------------------------------------------------------===//
// Analysis helpers (each takes already-decoded mono float audio)
//===--------------------------------------------------------------------===//
struct TempoResult { double bpm = 0; std::vector<double> beats; };

// aubio tempo: hop through the samples, collect beat times + overall BPM.
static TempoResult AnalyzeTempo(const rbx::Audio &a) {
	TempoResult r;
	if (!a.ok || a.samples.empty()) return r;
	const uint_t win = 1024, hop = 512;
	aubio_tempo_t *t = new_aubio_tempo("default", win, hop, (uint_t)a.sample_rate);
	if (!t) return r;
	fvec_t *in = new_fvec(hop);
	fvec_t *out = new_fvec(1);
	for (size_t pos = 0; pos + hop <= a.samples.size(); pos += hop) {
		for (uint_t i = 0; i < hop; i++) in->data[i] = a.samples[pos + i];
		aubio_tempo_do(t, in, out);
		if (out->data[0] != 0) r.beats.push_back((double)aubio_tempo_get_last_s(t));
	}
	r.bpm = (double)aubio_tempo_get_bpm(t);
	// Fallback: median inter-beat interval if aubio's running estimate is unset.
	if (r.bpm <= 1.0 && r.beats.size() > 4) {
		std::vector<double> iv;
		for (size_t i = 1; i < r.beats.size(); i++) iv.push_back(r.beats[i] - r.beats[i - 1]);
		std::sort(iv.begin(), iv.end());
		double med = iv[iv.size() / 2];
		if (med > 0) r.bpm = 60.0 / med;
	}
	del_fvec(in); del_fvec(out); del_aubio_tempo(t);
	return r;
}

// libebur128: integrated LUFS, loudness range, and true-peak (dBTP).
static bool AnalyzeLoudness(const rbx::Audio &a, double &lufs, double &tp, double &lra) {
	if (!a.ok || a.samples.empty()) return false;
	ebur128_state *st = ebur128_init(1, (unsigned long)a.sample_rate,
	    EBUR128_MODE_I | EBUR128_MODE_LRA | EBUR128_MODE_TRUE_PEAK);
	if (!st) return false;
	ebur128_add_frames_float(st, a.samples.data(), a.samples.size());
	ebur128_loudness_global(st, &lufs);
	ebur128_loudness_range(st, &lra);
	double peak = 0;
	ebur128_true_peak(st, 0, &peak);
	tp = (peak > 0) ? 20.0 * std::log10(peak) : -70.0;
	ebur128_destroy(&st);
	return true;
}

//===--------------------------------------------------------------------===//
// Scalar UDFs
//===--------------------------------------------------------------------===//

// libKeyFinder over already-decoded audio -> Camelot code.
static std::string KeyOfAudio(const rbx::Audio &audio) {
	if (!audio.ok || audio.samples.empty()) return "";
	KeyFinder::AudioData a;
	a.setFrameRate(audio.sample_rate);
	a.setChannels(1);
	a.addToSampleCount(audio.samples.size());
	for (size_t i = 0; i < audio.samples.size(); i++) a.setSample(i, audio.samples[i]);
	KeyFinder::KeyFinder kf;
	int idx = (int)kf.keyOfAudio(a);
	if (idx < 0 || idx > 24) idx = 24;
	return CAMELOT[idx];
}

// rb_key(VARCHAR) -> VARCHAR (Camelot). libKeyFinder.
void RbKeyFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(
	    args.data[0], result, args.size(), [&](string_t path_s) {
		    return StringVector::AddString(result, KeyOfAudio(rbx::DecodeMono(path_s.GetString(), 44100)));
	    });
}

// rb_analyze(VARCHAR) -> STRUCT(bpm, key, beatgrid, lufs, true_peak, lra).
// Decodes the file ONCE and runs every analyzer on the same buffer. This is the
// function to materialize into a table: `CREATE TABLE lib AS SELECT path,
// rb_analyze(path).* FROM glob(...)`.
void RbAnalyzeFun(DataChunk &args, ExpressionState &state, Vector &result) {
	for (idx_t r = 0; r < args.size(); r++) {
		Value pv = args.data[0].GetValue(r);
		auto audio = rbx::DecodeMono(pv.IsNull() ? "" : pv.ToString(), 44100);
		auto tempo = AnalyzeTempo(audio);
		std::string key = KeyOfAudio(audio);
		double lufs = -70, tp = -70, lra = 0;
		AnalyzeLoudness(audio, lufs, tp, lra);

		vector<Value> beats;
		for (double b : tempo.beats) beats.push_back(Value::DOUBLE(b));
		child_list_t<Value> f;
		f.emplace_back("bpm", Value::DOUBLE(tempo.bpm));
		f.emplace_back("key", Value(key));
		f.emplace_back("beatgrid", Value::LIST(LogicalType::DOUBLE, std::move(beats)));
		f.emplace_back("lufs", Value::DOUBLE(lufs));
		f.emplace_back("true_peak", Value::DOUBLE(tp));
		f.emplace_back("lra", Value::DOUBLE(lra));
		result.SetValue(r, Value::STRUCT(std::move(f)));
	}
}

// rb_bpm(VARCHAR) -> DOUBLE. aubio tempo.
void RbBpmFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(
	    args.data[0], result, args.size(), [&](string_t path_s) -> double {
		    return AnalyzeTempo(rbx::DecodeMono(path_s.GetString(), 44100)).bpm;
	    });
}

// rb_beatgrid(VARCHAR) -> DOUBLE[] of beat timestamps (seconds). aubio.
void RbBeatgridFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	for (idx_t r = 0; r < count; r++) {
		Value pv = args.data[0].GetValue(r);
		vector<Value> beats;
		if (!pv.IsNull()) {
			for (double b : AnalyzeTempo(rbx::DecodeMono(pv.ToString(), 44100)).beats)
				beats.push_back(Value::DOUBLE(b));
		}
		result.SetValue(r, Value::LIST(LogicalType::DOUBLE, std::move(beats)));
	}
}

// rb_loudness(VARCHAR) -> STRUCT(lufs, true_peak, lra). libebur128.
void RbLoudnessFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	for (idx_t r = 0; r < count; r++) {
		Value pv = args.data[0].GetValue(r);
		double lufs = -70, tp = -70, lra = 0;
		if (!pv.IsNull()) AnalyzeLoudness(rbx::DecodeMono(pv.ToString(), 44100), lufs, tp, lra);
		child_list_t<Value> fields;
		fields.emplace_back("lufs", Value::DOUBLE(lufs));
		fields.emplace_back("true_peak", Value::DOUBLE(tp));
		fields.emplace_back("lra", Value::DOUBLE(lra));
		result.SetValue(r, Value::STRUCT(std::move(fields)));
	}
}

} // namespace duckdb
