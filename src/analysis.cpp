#include "analysis.hpp"
#include "audio_decode.hpp"

#include <keyfinder/keyfinder.h>
#include <keyfinder/audiodata.h>
#include <string>

namespace duckdb {

// key_t enum index -> Camelot code (ordering per libKeyFinder constants.h).
static const char *CAMELOT[25] = {
    "11B","8A","6B","3A","1B","10A","8B","5A","3B","12A","10B","7A","5B",
    "2A","12B","9A","7B","4A","2B","11A","9B","6A","4B","1A",""};

// ---- rb_key: FULLY IMPLEMENTED (ports our kfcli) ------------------------
void RbKeyFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(
	    args.data[0], result, args.size(), [&](string_t path_s) {
		    auto audio = rbx::DecodeMono(path_s.GetString(), 44100);
		    if (!audio.ok) return StringVector::AddString(result, "");
		    KeyFinder::AudioData a;
		    a.setFrameRate(audio.sample_rate);
		    a.setChannels(1);
		    a.addToSampleCount(audio.samples.size());
		    for (size_t i = 0; i < audio.samples.size(); i++)
			    a.setSample(i, audio.samples[i]);
		    KeyFinder::KeyFinder kf;
		    int idx = (int)kf.keyOfAudio(a);
		    if (idx < 0 || idx > 24) idx = 24;
		    return StringVector::AddString(result, CAMELOT[idx]);
	    });
}

// ---- rb_bpm: TODO wire aubio_tempo over audio.samples -------------------
void RbBpmFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(
	    args.data[0], result, args.size(), [&](string_t path_s) -> double {
		    auto audio = rbx::DecodeMono(path_s.GetString(), 44100);
		    if (!audio.ok) return 0.0;
		    // TODO: feed audio.samples to aubio's `aubio_tempo` in a hop loop,
		    // collect beat periods, return the median BPM. (aubio/tempo.h)
		    return 0.0;
	    });
}

// ---- rb_beatgrid: TODO aubio beat tracking -> LIST<DOUBLE> --------------
void RbBeatgridFun(DataChunk &args, ExpressionState &state, Vector &result) {
	// Pattern: ListVector::PushBack each beat timestamp per row, then set
	// the list entry (offset,length). Beats come from aubio_tempo's
	// last_tatum/last_beat outputs over the sample stream.
	auto count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto list_entries = FlatVector::GetData<list_entry_t>(result);
	auto &child = ListVector::GetEntry(result);
	idx_t offset = 0;
	auto paths = FlatVector::GetData<string_t>(args.data[0]);
	for (idx_t r = 0; r < count; r++) {
		std::vector<double> beats; // TODO: aubio beat times for paths[r]
		for (double b : beats) {
			ListVector::PushBack(result, Value::DOUBLE(b));
		}
		list_entries[r].offset = offset;
		list_entries[r].length = beats.size();
		offset += beats.size();
	}
	(void)child;
}

// ---- rb_loudness: TODO libebur128 -> STRUCT(lufs,true_peak,lra) ---------
void RbLoudnessFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &children = StructVector::GetEntries(result);
	auto lufs = FlatVector::GetData<double>(*children[0]);
	auto tp = FlatVector::GetData<double>(*children[1]);
	auto lra = FlatVector::GetData<double>(*children[2]);
	auto paths = FlatVector::GetData<string_t>(args.data[0]);
	for (idx_t r = 0; r < args.size(); r++) {
		// TODO: ebur128_add_frames_float over audio.samples;
		// ebur128_loudness_global / _loudness_range / true-peak.
		(void)paths;
		lufs[r] = 0.0; tp[r] = 0.0; lra[r] = 0.0;
	}
}

} // namespace duckdb
