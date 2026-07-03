// Essentia-backed analysis UDFs. Compiled only when WITH_ESSENTIA=ON (which
// defines HAVE_ESSENTIA and links ../deps/essentia/install/lib/libessentia.a).
//
// Why Essentia in addition to aubio/libKeyFinder: Essentia's RhythmExtractor2013
// is a materially better beat tracker on electronic music than aubio's default,
// and it returns a per-track confidence plus the real per-beat tick list — which
// is exactly what we need to lay down an accurate CDJ tempo grid (ANLZ PQTZ).
// It also unlocks the semantic descriptors (danceability, dynamic complexity,
// onset rate, tuning) that aubio/KeyFinder/ebur128 don't provide.
//
// We build Essentia "lightweight" (no ffmpeg/taglib/gaia/tensorflow), so its own
// loaders and MusicExtractor are unavailable. That's fine: we feed it the mono
// float buffer produced by rbx::DecodeMono(), and call the individual algorithms
// directly.

#include "analysis.hpp"
#include "audio_decode.hpp"

#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"

#ifdef HAVE_ESSENTIA
#include <essentia/algorithmfactory.h>
#include <essentia/essentiamath.h>
#include <essentia/pool.h>
#endif

#include <string>
#include <vector>
#include <cmath>

namespace duckdb {

#ifdef HAVE_ESSENTIA

// Call once per process. essentia::init() registers the algorithm factory; it is
// not thread-safe, so LoadInternal() calls EssentiaInit() before registering the
// UDFs (extension load is single-threaded). A second call is a cheap no-op guard.
void EssentiaInit() {
	static bool done = false;
	if (!done) {
		essentia::init();
		done = true;
	}
}

namespace {

using essentia::Real;
using essentia::standard::Algorithm;
using essentia::standard::AlgorithmFactory;

// Result of RhythmExtractor2013 over one decoded track.
struct Rhythm {
	double bpm = 0;             // global BPM estimate
	double confidence = 0;      // 0..5.32 (Essentia scale); >2 is trustworthy
	std::vector<double> beats;  // beat tick times, seconds
};

// RhythmExtractor2013 needs mono audio at 44100 Hz (what DecodeMono gives us).
Rhythm AnalyzeRhythm(const rbx::Audio &a) {
	Rhythm r;
	if (!a.ok || a.samples.size() < 4096) return r;

	std::vector<Real> signal(a.samples.begin(), a.samples.end());

	AlgorithmFactory &F = AlgorithmFactory::instance();
	// "multifeature" is the accurate (if slower) method and is the only one that
	// emits a confidence value; "degara" is the faster fallback.
	Algorithm *rhythm = F.create("RhythmExtractor2013", "method", "multifeature");

	Real bpm = 0, confidence = 0;
	std::vector<Real> ticks, estimates, bpmIntervals;

	rhythm->input("signal").set(signal);
	rhythm->output("bpm").set(bpm);
	rhythm->output("ticks").set(ticks);
	rhythm->output("confidence").set(confidence);
	rhythm->output("estimates").set(estimates);
	rhythm->output("bpmIntervals").set(bpmIntervals);
	rhythm->compute();
	delete rhythm;

	r.bpm = bpm;
	r.confidence = confidence;
	r.beats.assign(ticks.begin(), ticks.end());
	return r;
}

// Danceability (Streaming Detrended Fluctuation Analysis). 0..~3, higher = more
// danceable. Cheap and model-free.
double AnalyzeDanceability(const rbx::Audio &a) {
	if (!a.ok || a.samples.size() < 4096) return 0;
	std::vector<Real> signal(a.samples.begin(), a.samples.end());
	AlgorithmFactory &F = AlgorithmFactory::instance();
	Algorithm *dance = F.create("Danceability", "sampleRate", (Real)a.sample_rate);
	Real danceability = 0;
	std::vector<Real> dfa;
	dance->input("signal").set(signal);
	dance->output("danceability").set(danceability);
	dance->output("dfa").set(dfa);
	dance->compute();
	delete dance;
	return danceability;
}

// Deep rhythm: metrical structure + downbeat, not just BPM/beats.
struct DeepRhythm {
	double bpm = 0, confidence = 0;
	int beat_count = 0;
	int meter = 0;              // estimated time signature (beats per bar), 0 = unknown
	int downbeat_index = -1;    // which beat (0-based, within first bar) is bar-1 "the 1"
	double downbeat_sec = 0;    // time of that first downbeat
	double ibi_stddev_ms = 0;   // inter-beat-interval spread; low = rock-solid (fixed grid ok)
	double onset_rate = 0;      // onsets per second (rhythmic density)
	std::vector<double> beats;         // tick times (s)
	std::vector<double> beat_loudness; // per-beat energy, whole spectrum
	std::vector<double> kick_pattern;  // per-beat 0-200 Hz energy ratio (the bass/kick hits)
};

// Full metrical analysis over one decoded track. Chains:
//   RhythmExtractor2013 -> beats -> BeatsLoudness -> Beatogram -> Meter,
// then infers the downbeat from the low-band (kick) pattern.
DeepRhythm AnalyzeDeepRhythm(const rbx::Audio &a) {
	DeepRhythm d;
	if (!a.ok || a.samples.size() < 4096) return d;
	std::vector<Real> signal(a.samples.begin(), a.samples.end());
	AlgorithmFactory &F = AlgorithmFactory::instance();

	// 1) Beats + BPM + confidence.
	Algorithm *rhythm = F.create("RhythmExtractor2013", "method", "multifeature");
	Real bpm = 0, confidence = 0;
	std::vector<Real> ticks, estimates, bpmIntervals;
	rhythm->input("signal").set(signal);
	rhythm->output("bpm").set(bpm);
	rhythm->output("ticks").set(ticks);
	rhythm->output("confidence").set(confidence);
	rhythm->output("estimates").set(estimates);
	rhythm->output("bpmIntervals").set(bpmIntervals);
	rhythm->compute();
	delete rhythm;

	d.bpm = bpm;
	d.confidence = confidence;
	d.beats.assign(ticks.begin(), ticks.end());
	d.beat_count = (int)ticks.size();

	// Inter-beat-interval spread -> tempo stability (ms).
	if (ticks.size() > 2) {
		std::vector<double> ibi;
		for (size_t i = 1; i < ticks.size(); i++) ibi.push_back((ticks[i] - ticks[i - 1]) * 1000.0);
		double m = 0; for (double v : ibi) m += v; m /= ibi.size();
		double s = 0; for (double v : ibi) s += (v - m) * (v - m);
		d.ibi_stddev_ms = std::sqrt(s / ibi.size());
	}

	// 2) Onset rate (rhythmic density).
	{
		Algorithm *onset = F.create("OnsetRate");
		std::vector<Real> onsetTimes; Real onsetRate = 0;
		onset->input("signal").set(signal);
		onset->output("onsets").set(onsetTimes);
		onset->output("onsetRate").set(onsetRate);
		onset->compute();
		delete onset;
		d.onset_rate = onsetRate;
	}

	if (ticks.size() < 4) return d; // not enough to talk about bars

	// 3) Per-beat loudness + band ratios (band[0] = 0-200 Hz = kick/bass).
	Algorithm *bl = F.create("BeatsLoudness", "sampleRate", (Real)a.sample_rate, "beats", ticks);
	std::vector<Real> loudness;
	std::vector<std::vector<Real>> bandRatio;
	bl->input("signal").set(signal);
	bl->output("loudness").set(loudness);
	bl->output("loudnessBandRatio").set(bandRatio);
	bl->compute();
	delete bl;

	for (Real v : loudness) d.beat_loudness.push_back(v);
	for (auto &b : bandRatio) d.kick_pattern.push_back(b.empty() ? 0.0 : (double)b[0]);

	// 4) Meter (time signature) via Beatogram -> Meter. Needs enough beats.
	if (loudness.size() >= 16 && bandRatio.size() == loudness.size()) {
		Algorithm *beatg = F.create("Beatogram", "size", 16);
		std::vector<std::vector<Real>> beatogram;
		beatg->input("loudness").set(loudness);
		beatg->input("loudnessBandRatio").set(bandRatio);
		beatg->output("beatogram").set(beatogram);
		beatg->compute();
		delete beatg;

		Algorithm *met = F.create("Meter");
		Real meter = 0;
		met->input("beatogram").set(beatogram);
		met->output("meter").set(meter);
		met->compute();
		delete met;
		// Meter is Essentia's *experimental* estimator — trust it only when it
		// lands on a plausible bar length; otherwise report 0 (unknown) and let
		// the downbeat search fall back to 4/4.
		int mi = (int)std::lround(meter);
		if (mi >= 3 && mi <= 7) d.meter = mi;
	}

	// 5) Downbeat: pick the bar phase whose beats carry the most kick energy.
	//    Search 4 phases (4/4) unless the meter is a clear odd/compound value.
	int m = (d.meter == 3 || d.meter == 5 || d.meter == 6 || d.meter == 7) ? d.meter : 4;
	if ((int)d.kick_pattern.size() >= m) {
		int best_p = 0; double best = -1;
		for (int p = 0; p < m; p++) {
			double sum = 0; int cnt = 0;
			for (size_t i = p; i < d.kick_pattern.size(); i += m) { sum += d.kick_pattern[i]; cnt++; }
			double avg = cnt ? sum / cnt : 0;
			if (avg > best) { best = avg; best_p = p; }
		}
		d.downbeat_index = best_p;
		d.downbeat_sec = d.beats[best_p];
	}
	return d;
}

} // namespace

// Bridge for AnalyzeTrack (analysis.cpp): run Essentia rhythm + danceability on
// an already-decoded buffer and copy the results into the shared feature struct.
void FillEssentiaFeatures(const rbx::Audio &audio, TrackFeatures &out) {
	DeepRhythm d = AnalyzeDeepRhythm(audio);
	out.bpm = d.bpm;
	out.confidence = d.confidence;
	out.beat_count = d.beat_count;
	out.meter = d.meter;
	out.downbeat_index = d.downbeat_index;
	out.downbeat_sec = d.downbeat_sec;
	out.ibi_stddev_ms = d.ibi_stddev_ms;
	out.onset_rate = d.onset_rate;
	out.beats = d.beats;
	out.beat_loudness = d.beat_loudness;
	out.kick_pattern = d.kick_pattern;
	out.danceability = AnalyzeDanceability(audio);
}

#endif // HAVE_ESSENTIA

//===--------------------------------------------------------------------===//
// UDFs
//===--------------------------------------------------------------------===//

// rb_rhythm(VARCHAR) -> STRUCT(bpm DOUBLE, confidence DOUBLE, beats DOUBLE[]).
// Essentia RhythmExtractor2013. `beats` is the tick list (seconds) that feeds the
// PQTZ tempo grid; `confidence` lets you flag tracks that need manual gridding.
void RbRhythmFun(DataChunk &args, ExpressionState &state, Vector &result) {
	for (idx_t r = 0; r < args.size(); r++) {
		Value pv = args.data[0].GetValue(r);
		child_list_t<Value> f;
#ifdef HAVE_ESSENTIA
		Rhythm rh;
		if (!pv.IsNull()) rh = AnalyzeRhythm(rbx::DecodeMono(pv.ToString(), 44100));
		vector<Value> beats;
		for (double b : rh.beats) beats.push_back(Value::DOUBLE(b));
		f.emplace_back("bpm", Value::DOUBLE(rh.bpm));
		f.emplace_back("confidence", Value::DOUBLE(rh.confidence));
		f.emplace_back("beats", Value::LIST(LogicalType::DOUBLE, std::move(beats)));
#else
		f.emplace_back("bpm", Value::DOUBLE(0));
		f.emplace_back("confidence", Value::DOUBLE(0));
		f.emplace_back("beats", Value::LIST(LogicalType::DOUBLE, vector<Value>{}));
#endif
		result.SetValue(r, Value::STRUCT(std::move(f)));
	}
}

// rb_rhythm_deep(VARCHAR) -> STRUCT of the full metrical picture:
//   bpm, confidence, beat_count, meter (time signature), downbeat_index +
//   downbeat_sec (the inferred bar-1 "1"), ibi_stddev_ms (tempo stability),
//   onset_rate, and the per-beat beats/beat_loudness/kick_pattern arrays.
// This is what fills the downbeat gap for the PQTZ tempo grid.
void RbRhythmDeepFun(DataChunk &args, ExpressionState &state, Vector &result) {
	for (idx_t r = 0; r < args.size(); r++) {
		Value pv = args.data[0].GetValue(r);
		child_list_t<Value> f;
#ifdef HAVE_ESSENTIA
		DeepRhythm d;
		if (!pv.IsNull()) d = AnalyzeDeepRhythm(rbx::DecodeMono(pv.ToString(), 44100));
		auto dbl_list = [&](const std::vector<double> &v) {
			vector<Value> out;
			for (double x : v) out.push_back(Value::DOUBLE(x));
			return Value::LIST(LogicalType::DOUBLE, std::move(out));
		};
		f.emplace_back("bpm", Value::DOUBLE(d.bpm));
		f.emplace_back("confidence", Value::DOUBLE(d.confidence));
		f.emplace_back("beat_count", Value::INTEGER(d.beat_count));
		f.emplace_back("meter", Value::INTEGER(d.meter));
		f.emplace_back("downbeat_index", Value::INTEGER(d.downbeat_index));
		f.emplace_back("downbeat_sec", Value::DOUBLE(d.downbeat_sec));
		f.emplace_back("ibi_stddev_ms", Value::DOUBLE(d.ibi_stddev_ms));
		f.emplace_back("onset_rate", Value::DOUBLE(d.onset_rate));
		f.emplace_back("beats", dbl_list(d.beats));
		f.emplace_back("beat_loudness", dbl_list(d.beat_loudness));
		f.emplace_back("kick_pattern", dbl_list(d.kick_pattern));
#else
		auto empty = Value::LIST(LogicalType::DOUBLE, vector<Value>{});
		f.emplace_back("bpm", Value::DOUBLE(0));
		f.emplace_back("confidence", Value::DOUBLE(0));
		f.emplace_back("beat_count", Value::INTEGER(0));
		f.emplace_back("meter", Value::INTEGER(0));
		f.emplace_back("downbeat_index", Value::INTEGER(-1));
		f.emplace_back("downbeat_sec", Value::DOUBLE(0));
		f.emplace_back("ibi_stddev_ms", Value::DOUBLE(0));
		f.emplace_back("onset_rate", Value::DOUBLE(0));
		f.emplace_back("beats", empty);
		f.emplace_back("beat_loudness", empty);
		f.emplace_back("kick_pattern", empty);
#endif
		result.SetValue(r, Value::STRUCT(std::move(f)));
	}
}

// rb_danceability(VARCHAR) -> DOUBLE. Essentia Danceability (0..~3).
void RbDanceabilityFun(DataChunk &args, ExpressionState &state, Vector &result) {
	for (idx_t r = 0; r < args.size(); r++) {
		Value pv = args.data[0].GetValue(r);
		double d = 0;
#ifdef HAVE_ESSENTIA
		if (!pv.IsNull()) d = AnalyzeDanceability(rbx::DecodeMono(pv.ToString(), 44100));
#endif
		result.SetValue(r, Value::DOUBLE(d));
	}
}

} // namespace duckdb
