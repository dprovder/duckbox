// rb_analyze_dir table function: glob a folder of audio and return one analysis
// row per file. Makes DuckDB fully self-contained — no source .duckdb, no shell:
//   CREATE TABLE track_analysis AS SELECT * FROM rb_analyze_dir('~/Music/Hau5');

#include "analyze_table.hpp"
#include "analysis.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/function/replacement_scan.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace duckdb {
namespace {

struct AnalyzeDirBind : public TableFunctionData {
	std::string pattern;
};

struct AnalyzeDirState : public GlobalTableFunctionState {
	std::vector<std::string> files;
	idx_t index = 0;
};

bool IsAudioPath(const std::string &p) {
	auto dot = p.find_last_of('.');
	if (dot == std::string::npos) return false;
	std::string ext = p.substr(dot + 1);
	for (auto &c : ext) c = (char)std::tolower((unsigned char)c);
	return ext == "m4a" || ext == "mp3" || ext == "wav" || ext == "aiff" || ext == "aif" ||
	       ext == "flac" || ext == "ogg" || ext == "aac" || ext == "wma" || ext == "mp4";
}

// Column layout (kept in sync with the SetValue calls below).
unique_ptr<FunctionData> AnalyzeDirBindFun(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {
	auto bind = make_uniq<AnalyzeDirBind>();
	bind->pattern = StringValue::Get(input.inputs[0]);

	auto add = [&](const char *n, LogicalType t) {
		names.emplace_back(n);
		return_types.push_back(std::move(t));
	};
	add("pos", LogicalType::VARCHAR);      // leading digits of filename ("003"), else ''
	add("path", LogicalType::VARCHAR);
	add("filename", LogicalType::VARCHAR);
	add("bpm", LogicalType::DOUBLE);
	add("confidence", LogicalType::DOUBLE);
	add("beat_count", LogicalType::INTEGER);
	add("meter", LogicalType::INTEGER);
	add("downbeat_index", LogicalType::INTEGER);
	add("downbeat_sec", LogicalType::DOUBLE);
	add("ibi_stddev_ms", LogicalType::DOUBLE);
	add("onset_rate", LogicalType::DOUBLE);
	add("danceability", LogicalType::DOUBLE);
	add("key_camelot", LogicalType::VARCHAR);
	add("lufs", LogicalType::DOUBLE);
	add("true_peak", LogicalType::DOUBLE);
	add("lra", LogicalType::DOUBLE);
	add("beats", LogicalType::LIST(LogicalType::DOUBLE));
	add("beat_loudness", LogicalType::LIST(LogicalType::DOUBLE));
	add("kick_pattern", LogicalType::LIST(LogicalType::DOUBLE));
	return std::move(bind);
}

unique_ptr<GlobalTableFunctionState> AnalyzeDirInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<AnalyzeDirBind>();
	auto state = make_uniq<AnalyzeDirState>();

	std::string pat = bind.pattern;
	if (pat.rfind("~/", 0) == 0) {
		const char *home = std::getenv("HOME");
		if (home) pat = std::string(home) + pat.substr(1);
	}
	if (!FileSystem::HasGlob(pat)) pat += "/*"; // a plain directory -> glob its entries

	auto &fs = FileSystem::GetFileSystem(context);
	for (auto &info : fs.Glob(pat)) {
		if (IsAudioPath(info.path)) state->files.push_back(info.path);
	}
	std::sort(state->files.begin(), state->files.end());
	return std::move(state);
}

Value DblList(const std::vector<double> &v) {
	vector<Value> out;
	out.reserve(v.size());
	for (double x : v) out.push_back(Value::DOUBLE(x));
	return Value::LIST(LogicalType::DOUBLE, std::move(out));
}

void AnalyzeDirFunc(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<AnalyzeDirState>();
	// Emit a small batch per call so the query stays cancellable and streams.
	const idx_t BATCH = 16;
	idx_t row = 0;
	while (state.index < state.files.size() && row < BATCH) {
		const std::string path = state.files[state.index++];
		TrackFeatures f = AnalyzeTrack(path);

		std::string filename = path.substr(path.find_last_of('/') + 1);
		std::string pos;
		for (char c : filename) {
			if (std::isdigit((unsigned char)c)) pos.push_back(c);
			else break;
		}

		idx_t col = 0;
		output.data[col++].SetValue(row, Value(pos));
		output.data[col++].SetValue(row, Value(path));
		output.data[col++].SetValue(row, Value(filename));
		output.data[col++].SetValue(row, Value::DOUBLE(f.bpm));
		output.data[col++].SetValue(row, Value::DOUBLE(f.confidence));
		output.data[col++].SetValue(row, Value::INTEGER(f.beat_count));
		output.data[col++].SetValue(row, Value::INTEGER(f.meter));
		output.data[col++].SetValue(row, Value::INTEGER(f.downbeat_index));
		output.data[col++].SetValue(row, Value::DOUBLE(f.downbeat_sec));
		output.data[col++].SetValue(row, Value::DOUBLE(f.ibi_stddev_ms));
		output.data[col++].SetValue(row, Value::DOUBLE(f.onset_rate));
		output.data[col++].SetValue(row, Value::DOUBLE(f.danceability));
		output.data[col++].SetValue(row, f.key_camelot.empty() ? Value(LogicalType::VARCHAR)
		                                                        : Value(f.key_camelot));
		output.data[col++].SetValue(row, Value::DOUBLE(f.lufs));
		output.data[col++].SetValue(row, Value::DOUBLE(f.true_peak));
		output.data[col++].SetValue(row, Value::DOUBLE(f.lra));
		output.data[col++].SetValue(row, DblList(f.beats));
		output.data[col++].SetValue(row, DblList(f.beat_loudness));
		output.data[col++].SetValue(row, DblList(f.kick_pattern));
		row++;
	}
	output.SetCardinality(row);
}

// Replacement scan: route `FROM '<audio-glob-or-folder>'` to rb_analyze_dir, the
// same way DuckDB routes `FROM 'file.csv'` to read_csv_auto.
unique_ptr<TableRef> AudioReplacementScan(ClientContext &context, ReplacementScanInput &input,
                                          optional_ptr<ReplacementScanData>) {
	static const vector<string> kExts = {"m4a", "mp3", "wav", "aiff", "aif", "flac", "aac", "ogg", "mp4"};
	std::string name = ReplacementScan::GetFullPath(input);
	bool match = ReplacementScan::CanReplace(name, kExts);
	if (!match) {
		// A plain audio directory (no wildcard, no extension) also routes here.
		std::string path = name;
		if (path.rfind("~/", 0) == 0) {
			const char *home = std::getenv("HOME");
			if (home) path = std::string(home) + path.substr(1);
		}
		if (!FileSystem::HasGlob(path)) {
			auto &fs = FileSystem::GetFileSystem(context);
			if (fs.DirectoryExists(path)) match = true;
		}
	}
	if (!match) return nullptr;
	auto tf = make_uniq<TableFunctionRef>();
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(make_uniq<ConstantExpression>(Value(name)));
	tf->function = make_uniq<FunctionExpression>("rb_analyze_dir", std::move(children));
	return std::move(tf);
}

} // namespace

TableFunction GetAnalyzeDirFunction() {
	return TableFunction("rb_analyze_dir", {LogicalType::VARCHAR}, AnalyzeDirFunc, AnalyzeDirBindFun,
	                     AnalyzeDirInit);
}

void RegisterAudioReplacementScan(DatabaseInstance &db) {
	DBConfig::GetConfig(db).replacement_scans.emplace_back(AudioReplacementScan);
}

} // namespace duckdb
