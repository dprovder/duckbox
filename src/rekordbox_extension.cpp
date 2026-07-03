#define DUCKDB_EXTENSION_MAIN
#include "rekordbox_extension.hpp"
#include "analysis.hpp"
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
