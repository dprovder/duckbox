// rekordbox export.pdb writer (DeviceSQL paged database) + COPY (FORMAT rekordbox).
//
//   COPY (SELECT title, artist, album, genre, label, key, bpm, duration_sec,
//         file_size, bitrate, sample_rate, track_number, year, rating,
//         path, filename FROM ...) TO '/Volumes/USB' (FORMAT rekordbox);
//
// Writes <root>/PIONEER/rekordbox/export.pdb and creates the USBANLZ tree; each
// track row's analyze_path points at PIONEER/USBANLZ/Pxxx/xxxxxxxx/ANLZ0000.DAT.
//
// Format calibrated against test/demo_export.pdb (raw table types are sequential
// 0..19; 4096-byte pages; page header 0x28; row index built backwards from the
// page end in groups of 16). Validate output with src/parsers/rekordbox_pdb.py.

#include "pdb_writer.hpp"
#include "anlz_writer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace duckdb {

struct RbTrack {
	std::string title, artist, album, genre, label, key, file_path, filename;
	double bpm = 0;
	int64_t duration_sec = 0, file_size = 0, bitrate = 0, sample_rate = 0;
	int32_t track_number = 0, year = 0, rating = 0;
	uint16_t file_type = 0;
	// analysis payload for the ANLZ files (optional; written if beats present):
	std::vector<double> beats;
	int32_t downbeat = 0;
	std::vector<uint8_t> wf_height, wf_low, wf_mid, wf_high;
	std::vector<AnlzCue> cues;   // memory cues -> ANLZ PCOB/PCO2
	std::string artwork;         // embedded cover-art bytes (JPEG/PNG), if any
	std::vector<std::pair<std::string, int>> playlists;  // (playlist name, sort)
	// assigned during build:
	uint32_t id = 0, artist_id = 0, album_id = 0, genre_id = 0, label_id = 0, key_id = 0, color_id = 0, artwork_id = 0;
	std::string analyze_path;  // /PIONEER/USBANLZ/Pxxx/xxxxxxxx/ANLZ0000.DAT
	std::string content_path;  // /Contents/<file> — USB-relative audio path (file_path in the pdb)
};

struct RbGlobalState : public GlobalFunctionData {
	std::string usb_root;
	std::vector<RbTrack> tracks;
};

namespace {

constexpr uint32_t PAGE = 4096;
constexpr uint32_t HEAP = 0x28; // page header size; rows/heap start here

// Little-endian byte assembler (the .pdb is little-endian).
struct LE {
	std::string b;
	void u1(uint8_t v) { b.push_back((char)v); }
	void u2(uint16_t v) { u1(v & 0xff); u1(v >> 8); }
	void u4(uint32_t v) { u2(v & 0xffff); u2(v >> 16); }
	void raw(const std::string &s) { b += s; }
};

std::string Utf16LE(const std::string &s) {
	std::string o;
	size_t i = 0;
	while (i < s.size()) {
		uint32_t cp; unsigned char c = s[i];
		if (c < 0x80) { cp = c; i += 1; }
		else if ((c >> 5) == 0x6 && i + 1 < s.size()) { cp = ((c & 0x1f) << 6) | (s[i+1] & 0x3f); i += 2; }
		else if ((c >> 4) == 0xe && i + 2 < s.size()) { cp = ((c & 0x0f) << 12) | ((s[i+1] & 0x3f) << 6) | (s[i+2] & 0x3f); i += 3; }
		else if ((c >> 3) == 0x1e && i + 3 < s.size()) { cp = ((c & 0x07) << 18) | ((s[i+1] & 0x3f) << 12) | ((s[i+2] & 0x3f) << 6) | (s[i+3] & 0x3f); i += 4; }
		else { cp = 0xFFFD; i += 1; }
		auto put16 = [&](uint16_t u) { o.push_back((char)(u & 0xff)); o.push_back((char)(u >> 8)); };
		if (cp <= 0xFFFF) put16((uint16_t)cp);
		else { cp -= 0x10000; put16((uint16_t)(0xD800 + (cp >> 10))); put16((uint16_t)(0xDC00 + (cp & 0x3FF))); }
	}
	return o;
}

bool IsAscii(const std::string &s) {
	for (unsigned char c : s) if (c >= 0x80) return false;
	return true;
}

// DeviceSQL string: short ASCII (odd flag), long ASCII (0x40), or long UTF-16LE (0x90).
std::string Dss(const std::string &s) {
	LE o;
	if (IsAscii(s) && s.size() <= 126) {
		o.u1((uint8_t)(((s.size() + 1) << 1) | 1)); // (len+1)*2+1 ; empty -> 0x03
		o.raw(s);
	} else if (IsAscii(s)) {
		o.u1(0x40); o.u2((uint16_t)(s.size() + 4)); o.u1(0); o.raw(s);
	} else {
		std::string u = Utf16LE(s);
		o.u1(0x90); o.u2((uint16_t)(u.size() + 4)); o.u1(0); o.raw(u);
	}
	return o.b;
}

//===--------------------------------------------------------------------===//
// Row encoders
//===--------------------------------------------------------------------===//
std::string TrackRow(const RbTrack &t) {
	LE h;
	h.u2(0x24);                        // 00 subtype
	h.u2(0);                           // 02 index_shift
	h.u4(0);                           // 04 bitmask
	h.u4((uint32_t)t.sample_rate);     // 08 sample_rate
	h.u4(0);                           // 0c composer_id
	h.u4((uint32_t)t.file_size);       // 10 file_size
	h.u4(0);                           // 14 u2
	h.u2(0);                           // 18 u3
	h.u2(0);                           // 1a u4
	h.u4(t.artwork_id);                // 1c artwork_id
	h.u4(t.key_id);                    // 20 key_id
	h.u4(0);                           // 24 original_artist_id
	h.u4(t.label_id);                  // 28 label_id
	h.u4(0);                           // 2c remixer_id
	h.u4((uint32_t)t.bitrate);         // 30 bitrate
	h.u4((uint32_t)t.track_number);    // 34 track_number
	h.u4((uint32_t)(t.bpm * 100 + 0.5)); // 38 tempo (bpm*100)
	h.u4(t.genre_id);                  // 3c genre_id
	h.u4(t.album_id);                  // 40 album_id
	h.u4(t.artist_id);                 // 44 artist_id
	h.u4(t.id);                        // 48 id
	h.u2(0);                           // 4c disc_number
	h.u2(0);                           // 4e play_count
	h.u2((uint16_t)t.year);            // 50 year
	h.u2(16);                          // 52 sample_depth
	h.u2((uint16_t)t.duration_sec);    // 54 duration
	h.u2(29);                          // 56 u5 (always 29)
	h.u1((uint8_t)t.color_id);         // 58 color_id
	h.u1((uint8_t)t.rating);           // 59 rating
	h.u2(t.file_type);                 // 5a file_type
	h.u2(3);                           // 5c u7 (always 3)

	// 21 strings (index -> field). Offsets are relative to row_base.
	std::string strs[21];
	for (auto &s : strs) s = ""; // default empty
	strs[6] = "ON";              // publish/kuvo
	strs[7] = "ON";              // autoload_hot_cues
	strs[14] = t.analyze_path;   // analyze_path
	strs[15] = "";               // analyze_date
	strs[17] = t.title;          // title
	strs[19] = t.filename;       // filename
	strs[20] = t.content_path;   // file_path — the USB-relative /Contents/ path

	std::string blob;
	uint16_t ofs[21];
	uint32_t base = 0x5e + 21 * 2; // = 0x88, where the string blob begins
	for (int i = 0; i < 21; i++) {
		ofs[i] = (uint16_t)(base + blob.size());
		blob += Dss(strs[i]);
	}
	LE row;
	row.raw(h.b);
	for (int i = 0; i < 21; i++) row.u2(ofs[i]);
	row.raw(blob);
	return row.b;
}

std::string ArtistRow(uint32_t id, const std::string &name) {
	LE r; r.u2(0x60); r.u2(0); r.u4(id); r.u1(0x03); r.u1(0x0a); r.raw(Dss(name)); return r.b;
}
std::string AlbumRow(uint32_t id, uint32_t artist_id, const std::string &name) {
	LE r; r.u2(0x80); r.u2(0); r.u4(0); r.u4(artist_id); r.u4(id); r.u4(0); r.u1(0x03); r.u1(0x16); r.raw(Dss(name)); return r.b;
}
std::string GenreRow(uint32_t id, const std::string &name) { LE r; r.u4(id); r.raw(Dss(name)); return r.b; }
std::string LabelRow(uint32_t id, const std::string &name) { LE r; r.u4(id); r.raw(Dss(name)); return r.b; }
std::string KeyRow(uint32_t id, const std::string &name) { LE r; r.u4(id); r.u4(id); r.raw(Dss(name)); return r.b; }
std::string ColorRow(uint32_t id, const std::string &name) { LE r; r.u2((uint16_t)id); r.u1(0); r.raw(Dss(name)); return r.b; }
std::string PlaylistTreeRow(uint32_t id, uint32_t parent, uint32_t sort, bool folder, const std::string &name) {
	LE r; r.u4(parent); r.u4(0); r.u4(sort); r.u4(id); r.u4(folder ? 1 : 0); r.raw(Dss(name)); return r.b;
}
std::string PlaylistEntryRow(uint32_t entry_index, uint32_t track_id, uint32_t playlist_id) {
	LE r; r.u4(entry_index); r.u4(track_id); r.u4(playlist_id); return r.b;
}
std::string ArtworkRow(uint32_t id, const std::string &path) {
	LE r; r.u4(id); r.raw(Dss(path)); return r.b;
}

//===--------------------------------------------------------------------===//
// Page layout
//===--------------------------------------------------------------------===//
// Distribute rows across pages: heap grows forward from HEAP; the row index
// grows backward in groups of 16 (0x24 bytes each). Returns the number of pages.
std::vector<std::vector<int>> PackRows(const std::vector<std::string> &rows) {
	std::vector<std::vector<int>> pages;
	std::vector<int> cur;
	uint32_t heap = HEAP;
	for (int i = 0; i < (int)rows.size(); i++) {
		int n = (int)cur.size() + 1;
		uint32_t groups = (n + 15) / 16;
		uint32_t need = heap + (uint32_t)rows[i].size() + groups * 0x24;
		if (!cur.empty() && need > PAGE) {
			pages.push_back(cur); cur.clear(); heap = HEAP;
		}
		cur.push_back(i);
		heap += (uint32_t)rows[i].size();
	}
	pages.push_back(cur); // always at least one page (may be empty)
	return pages;
}

// Serialize one 4096-byte data page.
std::string EmitPage(uint32_t page_index, uint32_t type, uint32_t next_page, uint32_t seq,
                     const std::vector<std::string> &rows, const std::vector<int> &row_ids) {
	std::string page(PAGE, '\0');
	auto put = [&](uint32_t off, uint32_t v, int n) {
		for (int i = 0; i < n; i++) page[off + i] = (char)((v >> (8 * i)) & 0xff);
	};
	// header
	put(4, page_index, 4);
	put(8, type, 4);
	put(12, next_page, 4);
	put(16, seq, 4);
	uint32_t r = (uint32_t)row_ids.size();
	uint32_t packed = (r & 0x1fff) | ((r & 0x7ff) << 13); // num_row_offsets(13) | num_rows(11)
	put(24, packed, 3);
	page[27] = (char)0x24; // page_flags: data page
	// heap + offsets
	uint32_t heap = HEAP;
	std::vector<uint16_t> offs;
	for (int id : row_ids) {
		offs.push_back((uint16_t)(heap - HEAP));
		const std::string &body = rows[id];
		for (size_t k = 0; k < body.size(); k++) page[heap + k] = body[k];
		heap += (uint32_t)body.size();
	}
	uint32_t used = heap - HEAP;
	uint32_t groups = (r + 15) / 16;
	put(28, PAGE - HEAP - used - groups * 0x24, 2); // free_size
	put(30, used, 2);                                // used_size
	// backward row index
	for (uint32_t g = 0; g < groups; g++) {
		uint32_t base = PAGE - g * 0x24;
		uint16_t present = 0;
		for (int j = 0; j < 16; j++) {
			uint32_t idx = g * 16 + j;
			if (idx < r) {
				put(base - 6 - 2 * j, offs[idx], 2);
				present |= (1u << j);
			}
		}
		put(base - 4, present, 2);
	}
	return page;
}

//===--------------------------------------------------------------------===//
// Whole-database builder
//===--------------------------------------------------------------------===//
struct Table { uint32_t type; std::vector<std::string> rows; };

std::string BuildPdb(std::vector<RbTrack> &tracks,
                     std::vector<std::pair<std::string, std::string>> &art_files) {
	// Intern id-tables.
	std::map<std::string, uint32_t> artists, albums, genres, labels, keys;
	auto intern = [](std::map<std::string, uint32_t> &m, const std::string &k) -> uint32_t {
		if (k.empty()) return 0;
		auto it = m.find(k);
		if (it != m.end()) return it->second;
		uint32_t id = (uint32_t)m.size() + 1;
		m[k] = id; return id;
	};
	for (uint32_t i = 0; i < tracks.size(); i++) {
		auto &t = tracks[i];
		t.id = i + 1;
		t.artist_id = intern(artists, t.artist);
		t.album_id = intern(albums, t.album);
		t.genre_id = intern(genres, t.genre);
		t.label_id = intern(labels, t.label);
		t.key_id = intern(keys, t.key);
		char buf[80];
		std::snprintf(buf, sizeof(buf), "/PIONEER/USBANLZ/P%03u/%08X/ANLZ0000.DAT", t.id % 1000, t.id);
		t.analyze_path = buf;
		t.content_path = "/Contents/" + t.filename;
	}

	// Table rows (20 tables, type == index).
	std::vector<Table> tabs(20);
	for (uint32_t i = 0; i < 20; i++) tabs[i].type = i;

	// Artwork (table type 13): dedup identical images, assign ids, write files to
	// /PIONEER/Artwork, set each track's artwork_id. Must run before TrackRow().
	std::map<std::string, uint32_t> art_ids; // bytes -> id
	for (auto &t : tracks) {
		if (t.artwork.empty()) continue;
		auto it = art_ids.find(t.artwork);
		if (it == art_ids.end()) {
			uint32_t aid = (uint32_t)art_ids.size() + 1;
			art_ids[t.artwork] = aid;
			const char *ext = (t.artwork.size() > 1 && (uint8_t)t.artwork[0] == 0x89) ? "png" : "jpg";
			char ap[96];
			std::snprintf(ap, sizeof(ap), "/PIONEER/Artwork/%05u.%s", aid, ext);
			tabs[13].rows.push_back(ArtworkRow(aid, ap));
			art_files.push_back({std::string(ap), t.artwork});
			t.artwork_id = aid;
		} else {
			t.artwork_id = it->second;
		}
	}

	for (auto &t : tracks) tabs[0].rows.push_back(TrackRow(t));
	auto emitNamed = [](std::map<std::string, uint32_t> &m, std::vector<std::string> &out,
	                    const std::function<std::string(uint32_t, const std::string &)> &enc) {
		std::vector<std::pair<uint32_t, std::string>> v;
		for (auto &kv : m) v.push_back({kv.second, kv.first});
		std::sort(v.begin(), v.end());
		for (auto &p : v) out.push_back(enc(p.first, p.second));
	};
	emitNamed(genres, tabs[1].rows, GenreRow);
	emitNamed(artists, tabs[2].rows, ArtistRow);
	{ // albums need artist_id -> look it up per album name's first track (approx 0)
		std::vector<std::pair<uint32_t, std::string>> v;
		for (auto &kv : albums) v.push_back({kv.second, kv.first});
		std::sort(v.begin(), v.end());
		for (auto &p : v) tabs[3].rows.push_back(AlbumRow(p.first, 0, p.second));
	}
	emitNamed(labels, tabs[4].rows, LabelRow);
	emitNamed(keys, tabs[5].rows, KeyRow);
	// colors: none for now
	// One "All Tracks" playlist so tracks browse from the playlist menu.
	tabs[7].rows.push_back(PlaylistTreeRow(1, 0, 0, false, "All Tracks"));
	for (uint32_t i = 0; i < tracks.size(); i++)
		tabs[8].rows.push_back(PlaylistEntryRow(i + 1, tracks[i].id, 1));
	// User playlists (from per-track membership). Root-level, id 2..N.
	std::map<std::string, uint32_t> pl_ids;                            // name -> tree id
	std::map<uint32_t, std::vector<std::pair<int, uint32_t>>> pl_ent;  // id -> [(sort, track_id)]
	for (auto &t : tracks)
		for (auto &pl : t.playlists) {
			auto it = pl_ids.find(pl.first);
			uint32_t pid = (it == pl_ids.end()) ? (pl_ids[pl.first] = (uint32_t)pl_ids.size() + 2) : it->second;
			pl_ent[pid].push_back({pl.second, t.id});
		}
	for (auto &kv : pl_ids)
		tabs[7].rows.push_back(PlaylistTreeRow(kv.second, 0, kv.second, false, kv.first));
	for (auto &kv : pl_ent) {
		auto ents = kv.second;
		std::sort(ents.begin(), ents.end());
		uint32_t idx = 1;
		for (auto &e : ents) tabs[8].rows.push_back(PlaylistEntryRow(idx++, e.second, kv.first));
	}

	// Pass 1: pack each table into pages and assign global indices (page 0 = header).
	struct TP { uint32_t type; std::vector<std::vector<int>> pages; uint32_t first, last; };
	std::vector<TP> tps;
	uint32_t next_index = 1;
	for (auto &tab : tabs) {
		TP tp; tp.type = tab.type;
		tp.pages = PackRows(tab.rows);
		tp.first = next_index;
		tp.last = next_index + (uint32_t)tp.pages.size() - 1;
		next_index = tp.last + 1;
		tps.push_back(std::move(tp));
	}
	uint32_t total_pages = next_index; // index just past the last data page

	// Pass 2: emit header page + data pages.
	std::string out(PAGE, '\0');
	auto hput = [&](uint32_t off, uint32_t v, int n) {
		for (int i = 0; i < n; i++) out[off + i] = (char)((v >> (8 * i)) & 0xff);
	};
	hput(0, 0, 4);
	hput(4, PAGE, 4);
	hput(8, 20, 4);
	hput(12, total_pages, 4); // next_unused_page
	hput(16, 0, 4);
	hput(20, 1, 4);           // sequence
	uint32_t po = 0x1c;
	for (size_t i = 0; i < tps.size(); i++) {
		hput(po, tps[i].type, 4);
		hput(po + 4, 0, 4);           // empty_candidate
		hput(po + 8, tps[i].first, 4);
		hput(po + 12, tps[i].last, 4);
		po += 16;
	}

	for (size_t ti = 0; ti < tps.size(); ti++) {
		auto &tp = tps[ti];
		for (size_t pi = 0; pi < tp.pages.size(); pi++) {
			uint32_t idx = tp.first + (uint32_t)pi;
			uint32_t next = (pi + 1 < tp.pages.size()) ? idx + 1 : total_pages; // past end
			out += EmitPage(idx, tp.type, next, 1, tabs[ti].rows, tp.pages[pi]);
		}
	}
	return out;
}

uint16_t FileTypeFromPath(const std::string &p) {
	auto dot = p.find_last_of('.');
	std::string e = (dot == std::string::npos) ? "" : p.substr(dot + 1);
	for (auto &c : e) c = (char)tolower((unsigned char)c);
	if (e == "mp3") return 1;
	if (e == "m4a" || e == "mp4" || e == "aac") return 4;
	if (e == "flac") return 5;
	if (e == "wav") return 11;
	if (e == "aiff" || e == "aif") return 12;
	return 0;
}

} // namespace

//===--------------------------------------------------------------------===//
// CopyFunction plumbing
//===--------------------------------------------------------------------===//
struct RbBindData : public FunctionData {
	std::map<std::string, idx_t> col; // field name -> input column index
	unique_ptr<FunctionData> Copy() const override { return make_uniq<RbBindData>(*this); }
	bool Equals(const FunctionData &o) const override { return col == ((const RbBindData &)o).col; }
};

static unique_ptr<FunctionData> RbBind(ClientContext &, CopyFunctionBindInput &,
                                       const vector<Identifier> &names, const vector<LogicalType> &) {
	auto bd = make_uniq<RbBindData>();
	for (idx_t i = 0; i < names.size(); i++) {
		std::string n = names[i].GetIdentifierName();
		for (auto &c : n) c = (char)tolower((unsigned char)c);
		bd->col[n] = i;
	}
	return std::move(bd);
}

static unique_ptr<GlobalFunctionData> RbInitGlobal(ClientContext &, FunctionData &, const string &file_path) {
	auto gs = make_uniq<RbGlobalState>();
	gs->usb_root = file_path;
	return std::move(gs);
}

static unique_ptr<LocalFunctionData> RbInitLocal(ExecutionContext &, FunctionData &) {
	return make_uniq<LocalFunctionData>();
}

static void RbSink(ExecutionContext &, FunctionData &bind, GlobalFunctionData &gstate,
                   LocalFunctionData &, DataChunk &input) {
	auto &gs = gstate.Cast<RbGlobalState>();
	auto &bd = bind.Cast<RbBindData>();
	input.Flatten();
	auto S = [&](const char *f, idx_t r, const std::string &def = "") -> std::string {
		auto it = bd.col.find(f);
		if (it == bd.col.end()) return def;
		Value v = input.data[it->second].GetValue(r);
		return v.IsNull() ? def : v.ToString();
	};
	auto D = [&](const char *f, idx_t r) -> double {
		auto it = bd.col.find(f);
		if (it == bd.col.end()) return 0;
		Value v = input.data[it->second].GetValue(r);
		return v.IsNull() ? 0 : v.GetValue<double>();
	};
	auto LD = [&](const char *f, idx_t r) -> std::vector<double> {
		std::vector<double> o; auto it = bd.col.find(f); if (it == bd.col.end()) return o;
		Value v = input.data[it->second].GetValue(r); if (v.IsNull()) return o;
		for (auto &c : ListValue::GetChildren(v)) if (!c.IsNull()) o.push_back(c.GetValue<double>());
		return o;
	};
	auto LU = [&](const char *f, idx_t r) -> std::vector<uint8_t> {
		std::vector<uint8_t> o; auto it = bd.col.find(f); if (it == bd.col.end()) return o;
		Value v = input.data[it->second].GetValue(r); if (v.IsNull()) return o;
		for (auto &c : ListValue::GetChildren(v)) if (!c.IsNull()) o.push_back(c.GetValue<uint8_t>());
		return o;
	};
	// LIST(STRUCT(t, color, comment)) -> memory cues
	auto LC = [&](const char *f, idx_t r) -> std::vector<AnlzCue> {
		std::vector<AnlzCue> o; auto it = bd.col.find(f); if (it == bd.col.end()) return o;
		Value v = input.data[it->second].GetValue(r); if (v.IsNull()) return o;
		for (auto &cue : ListValue::GetChildren(v)) {
			if (cue.IsNull()) continue;
			auto &ch = StructValue::GetChildren(cue);
			AnlzCue ac;
			if (ch.size() > 0 && !ch[0].IsNull()) ac.time_ms = (uint32_t)ch[0].GetValue<int64_t>();
			if (ch.size() > 1 && !ch[1].IsNull()) ac.color = (uint32_t)ch[1].GetValue<int64_t>();
			if (ch.size() > 2 && !ch[2].IsNull()) ac.comment = ch[2].ToString();
			o.push_back(ac);
		}
		return o;
	};
	auto B = [&](const char *f, idx_t r) -> std::string {   // BLOB -> raw bytes
		auto it = bd.col.find(f); if (it == bd.col.end()) return "";
		Value v = input.data[it->second].GetValue(r); if (v.IsNull()) return "";
		return StringValue::Get(v);
	};
	auto LP = [&](const char *f, idx_t r) -> std::vector<std::pair<std::string, int>> {
		std::vector<std::pair<std::string, int>> o; auto it = bd.col.find(f); if (it == bd.col.end()) return o;
		Value v = input.data[it->second].GetValue(r); if (v.IsNull()) return o;
		for (auto &pl : ListValue::GetChildren(v)) {
			if (pl.IsNull()) continue;
			auto &ch = StructValue::GetChildren(pl);
			std::string name = (ch.size() > 0 && !ch[0].IsNull()) ? ch[0].ToString() : "";
			int sort = (ch.size() > 1 && !ch[1].IsNull()) ? (int)ch[1].GetValue<int64_t>() : 0;
			if (!name.empty()) o.push_back({name, sort});
		}
		return o;
	};
	for (idx_t r = 0; r < input.size(); r++) {
		RbTrack t;
		t.title = S("title", r); t.artist = S("artist", r); t.album = S("album", r);
		t.genre = S("genre", r); t.label = S("label", r);
		t.key = S("key", r); if (t.key.empty()) t.key = S("key_camelot", r);
		t.file_path = S("path", r); if (t.file_path.empty()) t.file_path = S("file_path", r);
		t.filename = S("filename", r);
		if (t.filename.empty()) { auto s = t.file_path.find_last_of('/'); t.filename = s==std::string::npos?t.file_path:t.file_path.substr(s+1); }
		if (t.title.empty()) t.title = t.filename;
		t.bpm = D("bpm", r);
		t.duration_sec = (int64_t)D("duration_sec", r); if (!t.duration_sec) t.duration_sec = (int64_t)D("duration", r);
		t.file_size = (int64_t)D("file_size", r); if (!t.file_size) t.file_size = (int64_t)D("size_bytes", r);
		t.bitrate = (int64_t)D("bitrate", r); if (!t.bitrate) t.bitrate = (int64_t)D("bitrate_kbps", r);
		t.sample_rate = (int64_t)D("sample_rate", r);
		t.track_number = (int32_t)D("track_number", r);
		t.year = (int32_t)D("year", r);
		t.rating = (int32_t)D("rating", r);
		t.file_type = FileTypeFromPath(t.file_path);
		t.beats = LD("beats", r);
		t.downbeat = (int32_t)D("downbeat_index", r);
		t.wf_height = LU("height", r); t.wf_low = LU("low", r);
		t.wf_mid = LU("mid", r); t.wf_high = LU("high", r);
		t.cues = LC("cues", r);
		t.artwork = B("art", r);
		t.playlists = LP("playlists", r);
		gs.tracks.push_back(std::move(t));
	}
}

static void RbCombine(ExecutionContext &, FunctionData &, GlobalFunctionData &, LocalFunctionData &) {}

static void RbFinalize(ClientContext &, FunctionData &, GlobalFunctionData &gstate) {
	auto &gs = gstate.Cast<RbGlobalState>();
	namespace fs = std::filesystem;
	fs::create_directories(fs::path(gs.usb_root) / "PIONEER" / "rekordbox");
	fs::create_directories(fs::path(gs.usb_root) / "PIONEER" / "USBANLZ");
	std::vector<std::pair<std::string, std::string>> art_files;
	std::string pdb = BuildPdb(gs.tracks, art_files);
	std::ofstream out(fs::path(gs.usb_root) / "PIONEER" / "rekordbox" / "export.pdb", std::ios::binary);
	out.write(pdb.data(), (std::streamsize)pdb.size());
	// Cover-art files under /PIONEER/Artwork (paths recorded in the artwork table).
	for (auto &af : art_files) {
		std::error_code aec;
		auto p = fs::path(gs.usb_root) / af.first.substr(1);  // strip leading '/'
		fs::create_directories(p.parent_path(), aec);
		std::ofstream(p, std::ios::binary).write(af.second.data(), (std::streamsize)af.second.size());
	}
	for (auto &t : gs.tracks) {
		std::error_code ec;
		// 1. Copy the audio onto the USB under /Contents (what file_path now points at).
		if (!t.file_path.empty()) {
			auto dst = fs::path(gs.usb_root) / "Contents" / t.filename;
			fs::create_directories(dst.parent_path(), ec);
			fs::copy_file(t.file_path, dst, fs::copy_options::overwrite_existing, ec);
		}
		// 2. ANLZ .DAT (+ .EXT) where analyze_path points; PPTH = the /Contents/ path.
		char dir[80];
		std::snprintf(dir, sizeof(dir), "PIONEER/USBANLZ/P%03u/%08X", t.id % 1000, t.id);
		auto d = fs::path(gs.usb_root) / dir;
		fs::create_directories(d, ec);
		if (t.beats.empty()) continue; // no analysis provided -> pdb + audio only
		std::string dat = BuildAnlzDatBytes(t.content_path, t.beats, t.bpm, t.downbeat,
		                                    t.wf_height, t.wf_low, t.wf_mid, t.wf_high, t.cues);
		std::ofstream(d / "ANLZ0000.DAT", std::ios::binary).write(dat.data(), (std::streamsize)dat.size());
		if (!t.wf_height.empty()) {
			std::string ext = BuildAnlzExtBytes(t.content_path, t.wf_height, t.wf_low, t.wf_mid, t.wf_high, t.cues);
			std::ofstream(d / "ANLZ0000.EXT", std::ios::binary).write(ext.data(), (std::streamsize)ext.size());
		}
	}
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
