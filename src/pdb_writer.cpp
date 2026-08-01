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

#include "pdb_static_tables.inc"
#include "pdb_companion.inc"     // MYSETTING.DAT bytes  // track-independent tables (columns/colors/…)
#include "pdb_bytes.hpp"

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

// Byte-level DeviceSQL primitives now live in include/pdb_bytes.hpp so they can
// be unit-tested against reference vectors (test/pdb_bytes_test.cpp).
using rbx::LE; using rbx::Utf16LE; using rbx::IsAscii; using rbx::IsLongForm; using rbx::Dss;

//===--------------------------------------------------------------------===//
// Row encoders
//===--------------------------------------------------------------------===//
std::string TrackRow(const RbTrack &t) {
	LE h;
	h.u2(0x24);                        // 00 subtype
	h.u2(0);                           // 02 index_shift
	// Modern rekordbox writes 0xC0700 here; we wrote 0 (copied from a 2014 export).
	h.u4(0x000C0700);                  // 04 bitmask
	h.u4(t.sample_rate ? (uint32_t)t.sample_rate : 44100); // 08 sample_rate (rekordbox never writes 0)
	h.u4(0);                           // 0c composer_id
	h.u4((uint32_t)t.file_size);       // 10 file_size
	// @0x14 is unique per row in every real export (an analysis/sort id); @0x18 and
	// @0x1a are nonzero and constant within a file. All three are 0 in our rows,
	// which no real export ever has — mirror the shape instead.
	h.u4(25248 + t.id);                // 14 per-row id
	h.u2(58750);                       // 18 (constant within an export)
	h.u2(38964);                       // 1a (constant within an export)
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
	h.u2(41);                          // 56 always 41 in real exports (we had 29)
	h.u1((uint8_t)t.color_id);         // 58 color_id
	h.u1((uint8_t)t.rating);           // 59 rating
	h.u2(t.file_type);                 // 5a file_type
	h.u2(3);                           // 5c u7 (always 3)

	// 21 strings (index -> field). Offsets are relative to row_base.
	std::string strs[21];
	for (auto &s : strs) s = ""; // default empty
	// Analysis flags: "1" = not analysed, "2" = analysed (confirmed against a
	// rekordbox 7 export made with Key on and Phrase off, which wrote "2" and "1").
	strs[2] = "2";               // key analysed
	strs[3] = "1";               // phrase analysis not performed
	strs[6] = "ON";              // publish/kuvo
	strs[7] = "ON";              // autoload_hot_cues
	strs[10] = "2025-01-01";     // date_added (rekordbox always writes a YYYY-MM-DD)
	strs[14] = t.analyze_path;   // analyze_path
	strs[15] = "2025-01-01";     // analyze_date
	strs[17] = t.title;          // title
	strs[19] = t.filename;       // filename
	strs[20] = t.content_path;   // file_path — the USB-relative /Contents/ path

	std::string blob;
	uint16_t ofs[21];
	uint32_t base = 0x5e + 21 * 2; // = 0x88, where the string blob begins
	for (int i = 0; i < 21; i++) {
		// Long-form strings (0x40 ascii / 0x90 UTF-16) must start on a 4-byte
		// boundary within the row: their payload begins 4 bytes in, so the player
		// reads it with aligned 32-bit loads. Misaligning one faults the main CPU
		// and hangs it (surfaces as E-8709 on the deck). Real exports pad to keep
		// this invariant — 95 of 95 UTF-16 strings sampled are 4-byte aligned.
		if (IsLongForm(strs[i]))
			while ((base + blob.size()) % 4) blob += '\0';
		ofs[i] = (uint16_t)(base + blob.size());
		blob += Dss(strs[i]);
	}
	LE row;
	row.raw(h.b);
	for (int i = 0; i < 21; i++) row.u2(ofs[i]);
	row.raw(blob);
	return row.b;
}

// All row encoders live in pdb_bytes.hpp and are unit-tested against byte vectors
// taken from real rekordbox exports (test/pdb_bytes_test.cpp).
std::string ArtistRow(uint32_t id, const std::string &name) { return rbx::ArtistRowBytes(id, 0, name); }
std::string AlbumRow(uint32_t id, uint32_t artist_id, const std::string &name) {
	return rbx::AlbumRowBytes(id, 0, artist_id, name);
}
std::string GenreRow(uint32_t id, const std::string &name) { return rbx::GenreRowBytes(id, name); }
std::string LabelRow(uint32_t id, const std::string &name) { return rbx::LabelRowBytes(id, name); }
std::string KeyRow(uint32_t id, const std::string &name) { return rbx::KeyRowBytes(id, name); }
std::string ColorRow(uint32_t id, const std::string &name) { return rbx::ColorRowBytes(id, name); }
std::string PlaylistTreeRow(uint32_t id, uint32_t parent, uint32_t sort, bool folder, const std::string &name) {
	return rbx::PlaylistTreeRowBytes(id, parent, sort, folder, name);
}
std::string PlaylistEntryRow(uint32_t entry_index, uint32_t track_id, uint32_t playlist_id) {
	return rbx::PlaylistEntryRowBytes(entry_index, track_id, playlist_id);
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
		uint32_t sz = ((uint32_t)rows[i].size() + 3) & ~3u; // rows are 4-byte aligned
		uint32_t need = heap + sz + groups * 0x24;
		if (!cur.empty() && need > PAGE) {
			pages.push_back(cur); cur.clear(); heap = HEAP;
		}
		cur.push_back(i);
		heap += sz;
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
	// page_flags: real rekordbox uses 0x34 for tracks(0) + history(19), 0x24 otherwise.
	page[27] = (type == 0 || type == 19) ? (char)0x34 : (char)0x24;
	// transaction_row_count / transaction_row_index: 0x1fff = "last transaction
	// failed" — the sentinel rekordbox always writes so the player skips any
	// transaction replay. (We previously wrote 1/0 = a valid committed txn, which
	// can make the firmware attempt recovery against a nonexistent log.)
	put(0x20, 0x1fff, 2);
	put(0x22, 0x1fff, 2);
	// heap + offsets
	uint32_t heap = HEAP;
	std::vector<uint16_t> offs;
	// Rows of the "big" types (tracks/artists/albums) begin with a u2 subtype and
	// a u2 index_shift = (row's slot in this page's row index) << 5. Real rekordbox
	// sets it per row; the firmware uses it to enumerate the table, so leaving it 0
	// on every row makes the whole table read as empty on the player.
	bool has_index_shift = (type == 0 || type == 2 || type == 3);
	uint32_t slot = 0;
	for (int id : row_ids) {
		offs.push_back((uint16_t)(heap - HEAP));
		const std::string &body = rows[id];
		for (size_t k = 0; k < body.size(); k++) page[heap + k] = body[k];
		if (has_index_shift) {
			uint16_t ish = (uint16_t)(slot << 5);
			page[heap + 2] = (char)(ish & 0xff);
			page[heap + 3] = (char)((ish >> 8) & 0xff);
		}
		heap += (uint32_t)body.size();
		// Pad so the next row also starts on a 4-byte boundary. String offsets are
		// relative to the row base, so a misaligned row undoes the in-row alignment
		// and puts UTF-16 payloads on odd addresses (see IsLongForm in TrackRow).
		// Every row in every real export starts 4-byte aligned (203/203, 37/37).
		while ((heap - HEAP) % 4) heap++;
		slot++;
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
		put(base - 4, present, 2); // row-presence bitmap
		put(base - 2, present, 2); // real rekordbox duplicates it in the trailing u16
	}
	return page;
}

// The empty "strange" page real rekordbox puts first in every table; the data
// pages follow via next_page. Firmware navigates first_page -> here -> next.
std::string EmitStrangePage(uint32_t page_index, uint32_t type, uint32_t next_page,
                            uint32_t first_data_page,
                            const std::vector<uint32_t> &free_pages = {}) {
	std::string page(PAGE, '\0');
	auto put = [&](uint32_t off, uint32_t v, int n) {
		for (int i = 0; i < n; i++) page[off + i] = (char)((v >> (8 * i)) & 0xff);
	};
	put(4, page_index, 4);
	put(8, type, 4);
	put(12, next_page, 4);
	put(16, 1, 4);          // @0x10 transaction id
	page[0x1b] = (char)0x64; // page_flags: strange/non-data page
	put(0x20, 0x1fff, 2);
	put(0x22, 0x1fff, 2);   // num_rows_large = sentinel
	put(0x24, 0x03ec, 2);
	put(0x26, 0, 2);
	// THE index (strange) page carries a navigation structure the CDJ firmware uses
	// to enumerate the table — crate-digger/kaitai skip these pages (they read via
	// the data-page linked list), so this was invisible, but WITHOUT it the player
	// finds zero rows and the table browses empty. Real rekordbox fills the whole
	// heap. Layout for a table with <=1 data page (verified byte-exact vs two
	// CDJ-validated exports):
	//   u32 page_index | u32 first_data_page (0x03ffffff if none) | u32 0x03ffffff |
	//   u32 0 | u32 word5 | N x u32 entry | 0x1ffffff8 fill | 20 trailing zero bytes
	// word5's low u16 is the entry count; each entry is (page_index << 3) and names a
	// data page of this table that still has room (a free-space list). 0x1ffffff8 is
	// the null slot (== 0x03ffffff << 3). Decoded from six real exports: e.g. entries
	// [0x80,0x170,0xf60] >> 3 = pages [16,46,492], exactly that table's data pages.
	put(HEAP + 0, page_index, 4);
	put(HEAP + 4, first_data_page ? first_data_page : 0x03ffffff, 4);
	put(HEAP + 8, 0x03ffffff, 4);
	put(HEAP + 12, 0, 4);
	put(HEAP + 16, 0x1fff0000 | (uint32_t)(free_pages.size() & 0xffff), 4);
	uint32_t o = HEAP + 20;
	for (uint32_t fp : free_pages) { put(o, fp << 3, 4); o += 4; }
	for (; o + 4 <= PAGE - 20; o += 4) put(o, 0x1ffffff8, 4);
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
	// (Auto "All Tracks" playlist temporarily disabled — the CDJ-validated
	// reference exports have zero playlists yet browse fine, so testing whether
	// our playlist tables are what break the browse.)
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

	// Track-independent metadata tables the CDJ firmware needs for browsing
	// (columns/menu, colors, and two undocumented tables + a history header),
	// lifted verbatim from a real rekordbox export.
	for (auto &st : STATIC_TABS) {
		int off = 0;
		for (int i = 0; i < st.n; i++) {
			tabs[st.type].rows.push_back(std::string((const char *)st.data + off, (size_t)st.lens[i]));
			off += st.lens[i];
		}
	}

	// Pass 1: pack each table into pages and assign global indices (page 0 = header).
	// Layout per table mirrors real rekordbox: an empty "strange" index page, then
	// data pages (only if the table has rows), and every table gets an all-zero
	// "empty candidate" page (allocated from a pool at the end of the file) that its
	// header entry points at — the firmware reads empty_candidate to seed its page
	// cache, and a 0 there would alias the file-header page (page 0) and crash it.
	// CANONICAL rekordbox page layout. Table i's index ("strange") page is ALWAYS at
	// page 2i+1 and its first data page at 2i+2, so pages 1..40 are a fixed reserved
	// region for the 20 tables; extra data pages are appended from page 41 on. This
	// holds in every real export examined (six files from four independent sources,
	// 41 to 720 pages) INCLUDING for empty tables, which keep their reserved slot.
	// The firmware evidently locates a table's index page arithmetically, so packing
	// pages densely (skipping empty tables) makes it read the wrong page and hang the
	// main CPU -> E-8709 COMMUNICATION ERROR on the player.
	struct TP {
		uint32_t type; std::vector<std::vector<int>> pages;
		uint32_t first, last, empty; std::vector<uint32_t> idx; // idx: page index per data page
	};
	std::vector<TP> tps;
	const uint32_t NTAB = (uint32_t)tabs.size();      // 20
	uint32_t next_free = 2 * NTAB + 1;                // = 41, first page past the reserved region
	for (uint32_t i = 0; i < NTAB; i++) {
		TP tp; tp.type = tabs[i].type;
		tp.first = 2 * i + 1;                         // index page (fixed slot)
		uint32_t reserved = 2 * i + 2;                // its first data page (fixed slot)
		if (tabs[i].rows.empty()) {
			tp.last = tp.first;                       // empty table: index page only
			tp.empty = reserved;                      // reserved slot doubles as empty_candidate
		} else {
			tp.pages = PackRows(tabs[i].rows);
			tp.idx.push_back(reserved);
			for (size_t k = 1; k < tp.pages.size(); k++) tp.idx.push_back(next_free++);
			tp.last = tp.idx.back();
			tp.empty = next_free++;                   // a zeroed page, as real exports do
		}
		tps.push_back(std::move(tp));
	}
	uint32_t total_pages = next_free;

	// Pass 2: emit header page + data pages.
	std::string out(PAGE, '\0');
	auto hput = [&](uint32_t off, uint32_t v, int n) {
		for (int i = 0; i < n; i++) out[off + i] = (char)((v >> (8 * i)) & 0xff);
	};
	hput(0, 0, 4);
	hput(4, PAGE, 4);
	hput(8, 20, 4);
	hput(12, total_pages, 4); // next_unused_page
	// Never 0 in any real export (observed 1, 4 or 5 across six files); readers
	// ignore this field but the player appears to require it.
	hput(16, 1, 4);
	hput(20, 1, 4);           // sequence
	uint32_t po = 0x1c;
	for (size_t i = 0; i < tps.size(); i++) {
		hput(po, tps[i].type, 4);
		hput(po + 4, tps[i].empty, 4); // empty_candidate: a real all-zero page
		hput(po + 8, tps[i].first, 4);
		hput(po + 12, tps[i].last, 4);
		po += 16;
	}

	// Pages are placed at fixed indices, so build the file as a page array (any slot
	// never written stays an all-zero page, exactly as in real exports).
	std::vector<std::string> pages(total_pages, std::string(PAGE, '\0'));
	for (size_t ti = 0; ti < tps.size(); ti++) {
		auto &tp = tps[ti];
		uint32_t after_strange = tp.pages.empty() ? tp.empty : tp.idx[0];
		uint32_t first_data = tp.pages.empty() ? 0 : tp.idx[0];
		pages[tp.first] = EmitStrangePage(tp.first, tp.type, after_strange, first_data, tp.idx);
		for (size_t pi = 0; pi < tp.pages.size(); pi++) {
			uint32_t idx = tp.idx[pi];
			uint32_t next = (pi + 1 < tp.pages.size()) ? tp.idx[pi + 1] : tp.empty;
			pages[idx] = EmitPage(idx, tp.type, next, 1, tabs[ti].rows, tp.pages[pi]);
		}
	}
	for (uint32_t i = 1; i < total_pages; i++) out += pages[i];
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
		// Real rekordbox never writes file_size/bitrate/sample_rate as 0; a CDJ
		// treats a zero-size track as a missing file and hides it from browse.
		// rb_deck doesn't carry these, so derive them: stat the (still-local) file
		// for the byte count, estimate bitrate from size/duration, default 44.1kHz.
		if (t.file_size == 0 && !t.file_path.empty()) {
			std::error_code ec;
			auto sz = std::filesystem::file_size(t.file_path, ec);
			if (!ec) t.file_size = (int64_t)sz;
		}
		if (t.bitrate == 0 && t.file_size > 0 && t.duration_sec > 0)
			t.bitrate = (int64_t)(t.file_size * 8 / (t.duration_sec * 1000)); // kbps
		if (t.sample_rate == 0) t.sample_rate = 44100;
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
	// A real export also carries these; players expect the full skeleton.
	fs::create_directories(fs::path(gs.usb_root) / "PIONEER" / "Artwork");
	fs::create_directories(fs::path(gs.usb_root) / "PIONEER" / "CDJ");
	fs::create_directories(fs::path(gs.usb_root) / "PIONEER" / "MPJ");
	{
		auto msp = fs::path(gs.usb_root) / "PIONEER" / "MYSETTING.DAT";
		std::ofstream(msp, std::ios::binary)
		    .write((const char *)MYSETTING_DAT, (std::streamsize)sizeof(MYSETTING_DAT));
		// USBMNG.DAT: PMNG header + PTBL header over a 200003-entry slot table.
		std::string mng(400058, '\0');
		auto be = [&](size_t o, uint32_t v, int n) {
			for (int i = 0; i < n; i++) mng[o + i] = (char)((v >> (8 * (n - 1 - i))) & 0xff);
		};
		mng.replace(0, 4, "PMNG"); be(4, 0x20, 4); be(8, 400058, 4); be(12, 1, 4);
		mng.replace(32, 4, "PTBL"); be(36, 0x14, 4); be(40, 400026, 4); be(44, 2, 4); be(48, 200003, 4);
		std::ofstream(fs::path(gs.usb_root) / "PIONEER" / "USBANLZ" / "USBMNG.DAT", std::ios::binary)
		    .write(mng.data(), (std::streamsize)mng.size());
	}
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
