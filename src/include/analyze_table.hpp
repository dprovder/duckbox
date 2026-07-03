#pragma once
#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

// rb_analyze_dir(VARCHAR) -> one row per audio file, all features. Usage:
//   CREATE TABLE track_analysis AS SELECT * FROM rb_analyze_dir('~/Music/Hau5');
// Accepts a directory (globs common audio extensions) or a glob pattern.
TableFunction GetAnalyzeDirFunction();

// Registers the replacement scan so `FROM '<audio glob or folder>'` routes to
// rb_analyze_dir (like `FROM 'file.csv'` routes to read_csv).
void RegisterAudioReplacementScan(DatabaseInstance &db);

} // namespace duckdb
