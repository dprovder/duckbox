#define DUCKDB_EXTENSION_MAIN
#include "rekordbox_extension.hpp"
#include "analysis.hpp"
#include "analyze_table.hpp"
#include "waveform.hpp"
#include "anlz_writer.hpp"
#include "pdb_writer.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	// ---- analysis scalar UDFs (decode + analyze a file path) ----
	loader.RegisterFunction(
	    ScalarFunction("rb_bpm", {LogicalType::VARCHAR}, LogicalType::DOUBLE, RbBpmFun));

	// rb_key(VARCHAR) -> VARCHAR (Camelot, e.g. '6A')   [IMPLEMENTED]
	loader.RegisterFunction(
	    ScalarFunction("rb_key", {LogicalType::VARCHAR}, LogicalType::VARCHAR, RbKeyFun));

	loader.RegisterFunction(
	    ScalarFunction("rb_beatgrid", {LogicalType::VARCHAR},
	                   LogicalType::LIST(LogicalType::DOUBLE), RbBeatgridFun));

	child_list_t<LogicalType> loud{{"lufs", LogicalType::DOUBLE},
	                               {"true_peak", LogicalType::DOUBLE},
	                               {"lra", LogicalType::DOUBLE}};
	loader.RegisterFunction(
	    ScalarFunction("rb_loudness", {LogicalType::VARCHAR},
	                   LogicalType::STRUCT(loud), RbLoudnessFun));

	// rb_analyze(VARCHAR) -> everything, from ONE decode. The workhorse:
	//   CREATE TABLE lib AS SELECT path, rb_analyze(path).* FROM glob(...)
	child_list_t<LogicalType> analysis{{"bpm", LogicalType::DOUBLE},
	                                   {"key", LogicalType::VARCHAR},
	                                   {"beatgrid", LogicalType::LIST(LogicalType::DOUBLE)},
	                                   {"lufs", LogicalType::DOUBLE},
	                                   {"true_peak", LogicalType::DOUBLE},
	                                   {"lra", LogicalType::DOUBLE}};
	loader.RegisterFunction(
	    ScalarFunction("rb_analyze", {LogicalType::VARCHAR},
	                   LogicalType::STRUCT(analysis), RbAnalyzeFun));

	// ---- Essentia-backed UDFs (rhythm + descriptors) ----
	// rb_rhythm(VARCHAR) -> STRUCT(bpm, confidence, beats DOUBLE[]).
	// RhythmExtractor2013 tick list + confidence -> accurate PQTZ tempo grid.
#ifdef HAVE_ESSENTIA
	EssentiaInit(); // register Essentia's algorithm factory once, single-threaded here
#endif
	child_list_t<LogicalType> rhythm{{"bpm", LogicalType::DOUBLE},
	                                 {"confidence", LogicalType::DOUBLE},
	                                 {"beats", LogicalType::LIST(LogicalType::DOUBLE)}};
	loader.RegisterFunction(
	    ScalarFunction("rb_rhythm", {LogicalType::VARCHAR},
	                   LogicalType::STRUCT(rhythm), RbRhythmFun));

	// rb_rhythm_deep(VARCHAR) -> full metrical structure incl. meter + downbeat.
	child_list_t<LogicalType> deep{{"bpm", LogicalType::DOUBLE},
	                               {"confidence", LogicalType::DOUBLE},
	                               {"beat_count", LogicalType::INTEGER},
	                               {"meter", LogicalType::INTEGER},
	                               {"downbeat_index", LogicalType::INTEGER},
	                               {"downbeat_sec", LogicalType::DOUBLE},
	                               {"ibi_stddev_ms", LogicalType::DOUBLE},
	                               {"onset_rate", LogicalType::DOUBLE},
	                               {"beats", LogicalType::LIST(LogicalType::DOUBLE)},
	                               {"beat_loudness", LogicalType::LIST(LogicalType::DOUBLE)},
	                               {"kick_pattern", LogicalType::LIST(LogicalType::DOUBLE)}};
	loader.RegisterFunction(
	    ScalarFunction("rb_rhythm_deep", {LogicalType::VARCHAR},
	                   LogicalType::STRUCT(deep), RbRhythmDeepFun));

	// rb_danceability(VARCHAR) -> DOUBLE (0..~3, higher = more danceable).
	loader.RegisterFunction(
	    ScalarFunction("rb_danceability", {LogicalType::VARCHAR},
	                   LogicalType::DOUBLE, RbDanceabilityFun));

	// ---- rb_analyze_dir(VARCHAR) table function: folder -> analysis rows ----
	loader.RegisterFunction(GetAnalyzeDirFunction());
	// `FROM '<audio glob or folder>'` routes to rb_analyze_dir automatically.
	RegisterAudioReplacementScan(loader.GetDatabaseInstance());

	// ---- rb_waveform(VARCHAR) -> STRUCT(cols, height[], low[], mid[], high[]) ----
	child_list_t<LogicalType> wf{{"cols", LogicalType::INTEGER},
	                             {"height", LogicalType::LIST(LogicalType::UTINYINT)},
	                             {"low", LogicalType::LIST(LogicalType::UTINYINT)},
	                             {"mid", LogicalType::LIST(LogicalType::UTINYINT)},
	                             {"high", LogicalType::LIST(LogicalType::UTINYINT)}};
	loader.RegisterFunction(
	    ScalarFunction("rb_waveform", {LogicalType::VARCHAR}, LogicalType::STRUCT(wf), RbWaveformFun));

	// ---- ANLZ writers: .DAT and .EXT from stored beatgrid + waveform arrays ----
	auto U8 = LogicalType::LIST(LogicalType::UTINYINT);
	loader.RegisterFunction(
	    ScalarFunction("rb_anlz_dat",
	                   {LogicalType::VARCHAR, LogicalType::LIST(LogicalType::DOUBLE),
	                    LogicalType::DOUBLE, LogicalType::INTEGER, U8, U8, U8, U8},
	                   LogicalType::BLOB, RbAnlzDatFun));
	loader.RegisterFunction(
	    ScalarFunction("rb_anlz_ext", {LogicalType::VARCHAR, U8, U8, U8, U8},
	                   LogicalType::BLOB, RbAnlzExtFun));

	// ---- the exporter: COPY <tracks> TO 'USB' (FORMAT rekordbox) ----
	loader.RegisterFunction(RekordboxCopyFunction::Get());
}

void RekordboxExtension::Load(ExtensionLoader &loader) { LoadInternal(loader); }

} // namespace duckdb

extern "C" {
// New-style C++ entry point (DuckDB >= 1.2). Expands to
// rekordbox_duckdb_cpp_init(duckdb::ExtensionLoader &loader).
DUCKDB_CPP_EXTENSION_ENTRY(rekordbox, loader) {
	duckdb::LoadInternal(loader);
}
}
