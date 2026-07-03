#include "pdb_writer.hpp"
#include "duckdb/common/serializer/write_stream.hpp"
#include <vector>
#include <string>

namespace duckdb {

// One accumulated row of the tracks table (subset; extend as needed).
struct RbTrack {
	std::string title, artist, genre, key, path;
	double bpm = 0;
	int64_t duration_sec = 0, file_size = 0;
	std::vector<double> beatgrid;
};

// Global sink state: a COPY is one whole file, so we buffer every row and
// serialize the multi-page .pdb + ANLZ tree in finalize (a .pdb is not a
// streamable row format).
struct RbGlobalState : public GlobalFunctionData {
	std::string usb_root;
	std::vector<RbTrack> tracks;
};

//===--------------------------------------------------------------------===//
// DeviceSQL / PIONEER serialization  (format mapped in ../NOTES.md, Phase A)
//===--------------------------------------------------------------------===//
namespace {
struct PdbBuilder {
	// 4096-byte pages; 20 tables (tracks,genres,artists,albums,labels,keys,
	// colors,playlist_tree,playlist_entries, + the empty/unknown ones).
	// Header(LE): u4=0, len_page=4096, num_tables=20, next_unused, u4,
	// sequence, gap[4], then table[] {type,empty,first_page,last_page}.
	// Each data page: 0x28 header + heap; row index built BACKWARDS from the
	// page end (row_groups of 16 + present bitmask). Track row = fixed struct
	// (subtype 0x24, tempo=bpm*100, ids -> artists/keys/genres) + 21 string
	// offsets into device_sql_string heap entries (short-ascii/long-utf16).
	// analyze_path string -> '/PIONEER/USBANLZ/Pxxx/xxxxxxxx/ANLZ0000.DAT'.
	std::vector<uint8_t> Build(const std::vector<RbTrack> &tracks) {
		std::vector<uint8_t> pdb;
		// TODO: intern artists/keys/genres -> id tables; lay out pages;
		//       encode rows + device_sql_strings; back-fill row index.
		//       Round-trip check: parse output with src/parsers/rekordbox_pdb.py
		//       and diff table/row counts against test/demo_export.pdb.
		return pdb;
	}
};

// ANLZ per track: PMAI header + PPTH(path) + PQTZ(beatgrid from rb_beatgrid)
// + PWAV/PWV (waveform) + PCOB (cues). anlz_anlz.py validates the output.
std::vector<uint8_t> BuildAnlz(const RbTrack &t) {
	std::vector<uint8_t> dat;
	// TODO: emit PMAI + PQTZ beats (t.beatgrid) + PPTH(t.path).
	return dat;
}
} // namespace

//===--------------------------------------------------------------------===//
// CopyFunction plumbing
//===--------------------------------------------------------------------===//
// Concrete bind data (FunctionData is abstract). Will hold the column->field
// mapping (which input column is title/artist/bpm/key/beatgrid/path).
struct RbBindData : public FunctionData {
	unique_ptr<FunctionData> Copy() const override { return make_uniq<RbBindData>(*this); }
	bool Equals(const FunctionData &) const override { return true; }
};

static unique_ptr<FunctionData>
RbBind(ClientContext &, CopyFunctionBindInput &, const vector<Identifier> &names,
       const vector<LogicalType> &) {
	// TODO: record which input column is title/artist/bpm/key/beatgrid/path.
	return make_uniq<RbBindData>();
}

static unique_ptr<GlobalFunctionData>
RbInitGlobal(ClientContext &, FunctionData &, const string &file_path) {
	auto gs = make_uniq<RbGlobalState>();
	gs->usb_root = file_path; // e.g. '/Volumes/MYUSB'
	return std::move(gs);
}

static unique_ptr<LocalFunctionData> RbInitLocal(ExecutionContext &, FunctionData &) {
	return make_uniq<LocalFunctionData>();
}

static void RbSink(ExecutionContext &, FunctionData &, GlobalFunctionData &gstate,
                   LocalFunctionData &, DataChunk &input) {
	auto &gs = gstate.Cast<RbGlobalState>();
	input.Flatten();
	for (idx_t r = 0; r < input.size(); r++) {
		RbTrack t;
		// TODO: pull columns per bind mapping (title/artist/bpm/key/path/beats)
		gs.tracks.push_back(std::move(t));
	}
}

// No per-thread local buffering yet: all rows accumulate in the global state,
// so combine is a no-op (required by the copy machinery).
static void RbCombine(ExecutionContext &, FunctionData &, GlobalFunctionData &,
                      LocalFunctionData &) {}

static void RbFinalize(ClientContext &, FunctionData &, GlobalFunctionData &gstate) {
	auto &gs = gstate.Cast<RbGlobalState>();
	// mkdir -p usb_root/PIONEER/{rekordbox,USBANLZ}
	PdbBuilder builder;
	auto pdb = builder.Build(gs.tracks);
	// write usb_root/PIONEER/rekordbox/export.pdb = pdb
	for (auto &t : gs.tracks) {
		auto anlz = BuildAnlz(t);
		// write usb_root/PIONEER/USBANLZ/.../ANLZ0000.DAT = anlz
		(void)anlz;
	}
	(void)pdb;
}

CopyFunction RekordboxCopyFunction::Get() {
	CopyFunction f("rekordbox");
	f.copy_to_bind = RbBind;
	f.copy_to_initialize_global = RbInitGlobal;
	f.copy_to_initialize_local = RbInitLocal;
	f.copy_to_sink = RbSink;
	f.copy_to_combine = RbCombine;
	f.copy_to_finalize = RbFinalize;
	f.extension = "pdb";
	return f;
}

} // namespace duckdb
