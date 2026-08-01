// ANLZ writer. Emits rekordbox track-analysis files as BLOBs:
//   rb_anlz_dat(...) -> ANLZ0000.DAT : PPTH, PQTZ, PWAV, PWV2, PVBR
//   rb_anlz_ext(...) -> ANLZ0000.EXT : PPTH, PWV3, PWV4, PWV5
// Both are projections of the stored beatgrid + waveform arrays. Tag/bit layouts
// per Deep Symmetry (rekordbox-export-analysis) + rekordcrate. Structurally
// validated by round-tripping through specs/rekordbox_anlz.ksy.
//
// Byte-exact fidelity to native rekordbox output (color scaling, PVBR contents)
// needs a reference export to calibrate — PVBR in particular is not fully
// reverse-engineered and is written best-effort here.

#include "anlz_writer.hpp"
#include "audio_decode.hpp"

#include "duckdb/common/types/value.hpp"

#include <sys/stat.h>
#include <string>
#include <vector>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace duckdb {
namespace {

//===--------------------------------------------------------------------===//
// Byte assembly + tag framing
//===--------------------------------------------------------------------===//
struct BE {
	std::string b;
	void u1(uint8_t v) { b.push_back((char)v); }
	void u2(uint16_t v) { u1(v >> 8); u1(v & 0xff); }
	void u4(uint32_t v) { u2(v >> 16); u2(v & 0xffff); }
	void raw(const std::string &s) { b += s; }
	void fourcc(const char *s) { b.append(s, 4); }
	void utf16be(const std::string &s) { for (unsigned char c : s) { u1(0); u1(c); } }
};

void PutTag(BE &out, const char *fourcc, uint32_t len_header, const std::string &body) {
	out.fourcc(fourcc);
	out.u4(len_header);
	out.u4(12 + (uint32_t)body.size());
	out.raw(body);
}

std::string Frame(const std::string &sections) {
	const uint32_t len_header = 0x1C; // PMAI: magic+lenh+lenf + 16 pad
	BE f;
	f.fourcc("PMAI");
	f.u4(len_header);
	f.u4(len_header + (uint32_t)sections.size());
	// The rest of the header is "padding" the kaitai spec skips outright, so
	// readers never surface it — but every real analysis file carries the same
	// three values here (36/36 .DAT and .EXT files checked), and we were writing
	// zeros. The player appears to need them before it will use the analysis.
	f.u4(0x00000001);
	f.u4(0x00010000);
	f.u4(0x00010000);
	while (f.b.size() < len_header) f.u1(0);
	f.raw(sections);
	return f.b;
}

//===--------------------------------------------------------------------===//
// Resampling helpers (waveform arrays are at 150 col/s; tags need fixed counts)
//===--------------------------------------------------------------------===//
// Average value of src over the range mapping to destination column c.
double RangeMean(const std::vector<uint8_t> &src, int c, int dst) {
	int n = (int)src.size();
	if (n == 0) return 0;
	size_t lo = (size_t)((int64_t)c * n / dst), hi = (size_t)((int64_t)(c + 1) * n / dst);
	if (hi <= lo) hi = std::min((size_t)n, lo + 1);
	double acc = 0; for (size_t i = lo; i < hi; i++) acc += src[i];
	return acc / (double)(hi - lo);
}
double RangeMax(const std::vector<uint8_t> &src, int c, int dst) {
	int n = (int)src.size();
	if (n == 0) return 0;
	size_t lo = (size_t)((int64_t)c * n / dst), hi = (size_t)((int64_t)(c + 1) * n / dst);
	if (hi <= lo) hi = std::min((size_t)n, lo + 1);
	double m = 0; for (size_t i = lo; i < hi; i++) m = std::max(m, (double)src[i]);
	return m;
}
// 0..7 brightness from the high-band share of energy at column c.
uint8_t Whiteness(double low, double mid, double high) {
	double tot = low + mid + high;
	if (tot <= 0) return 0;
	int w = (int)std::lround(high / tot * 7.0);
	return (uint8_t)std::clamp(w, 0, 7);
}

// The absolute high-band share is the wrong scale for this field. Bass-heavy
// material puts 10-15% of its energy above the crossover, so Whiteness() maps
// nearly every column to 0-1 and a CDJ draws the detail waveform almost black
// on black -- present, correctly located, and invisible. Real exports sit at
// 6-7 for most of a track (measured over a CDJ-written USB):
//
//     w        0     1     2     3     4     5     6     7
//     share  .063  .054  .063  .073  .070  .110  .231  .337
//
// Rank each column against the rest of its own track and place it in that
// distribution, so the shape comes from relative brightness and the levels
// come from real hardware. Self-normalising, so it survives any band scaling.
std::vector<uint8_t> WhitenessSeries(const std::vector<uint8_t> &lo, const std::vector<uint8_t> &mi,
                                     const std::vector<uint8_t> &hi) {
	static const double kCdf[8] = {0.0627, 0.1171, 0.1799, 0.2524, 0.3220, 0.4321, 0.6631, 1.0};
	size_t n = std::min({lo.size(), mi.size(), hi.size()});
	std::vector<double> share(n);
	std::vector<size_t> order;
	order.reserve(n);
	// Silence carries no brightness to rank. Real exports write 0xe0 there
	// (whiteness 7, height 0), so hand those columns 7 and rank the rest.
	std::vector<uint8_t> out(n, 7);
	for (size_t i = 0; i < n; i++) {
		double tot = (double)lo[i] + mi[i] + hi[i];
		if (tot <= 0) continue;
		share[i] = ((double)mi[i] + hi[i]) / tot;
		order.push_back(i);
	}
	std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return share[a] < share[b]; });
	size_t m = order.size();
	for (size_t r = 0; r < m; r++) {
		double pct = m > 1 ? (double)r / (double)(m - 1) : 1.0;
		int w = 0;
		while (w < 7 && pct > kCdf[w]) w++;
		out[order[r]] = (uint8_t)w;
	}
	return out;
}

//===--------------------------------------------------------------------===//
// Tag bodies
//===--------------------------------------------------------------------===//
std::string BuildPath(const std::string &path) {
	BE body;
	// The length counts a UTF-16 NUL terminator, so one has to be written: a
	// CDJ reads exactly this many bytes, and without the NUL it runs two bytes
	// into the next tag header, derails the parse and throws the whole file
	// away -- it then re-analyses the track and writes its own ANLZ beside
	// ours. Real exports and CDJ-written files both end 00 00 here.
	body.u4((uint32_t)(path.size() * 2 + 2));
	body.utf16be(path);
	body.u2(0);
	return body.b;
}

std::string BuildBeatGrid(const std::vector<double> &beats, double bpm, int downbeat) {
	BE body;
	body.u4(0);
	body.u4(0x80000);
	body.u4((uint32_t)beats.size());
	uint16_t tempo = (uint16_t)std::lround(bpm * 100.0);
	int phase = (downbeat >= 0) ? (downbeat % 4) : 0;
	for (size_t i = 0; i < beats.size(); i++) {
		int num = (((int)i - phase) % 4 + 4) % 4 + 1;
		body.u2((uint16_t)num);
		body.u2(tempo);
		body.u4((uint32_t)std::lround(beats[i] * 1000.0));
	}
	return body.b;
}

// Stored heights are RMS*(31/0.7) and run hotter than rekordbox's: our detail
// waveform averaged 12.8/31 against 7.7 in real exports over comparable
// material. Trim on the way out rather than re-analysing the whole library.
constexpr double WAVE_GAIN = 0.6;

// PWAV (400 cols) / PWV2 (100 cols): mono preview. byte = whiteness<<5 | height,
// height is `hbits` low bits (5 for PWAV, 4 for PWV2).
std::string BuildMonoPreview(const std::vector<uint8_t> &h, const std::vector<uint8_t> &lo,
                             const std::vector<uint8_t> &mi, const std::vector<uint8_t> &hi,
                             int cols, int hbits) {
	int hmax = (1 << hbits) - 1;
	BE body;
	body.u4((uint32_t)cols);
	body.u4(0x10000);
	for (int c = 0; c < cols; c++) {
		// Mean, not peak: taking the max of each window saturates dense music (our
		// PWAV averaged 28.7/31 where real exports average 14.9 over the same
		// material, i.e. a solid block instead of a shape).
		// The window mean alone lands the preview at real levels; the extra detail
		// trim is not wanted here (mean+trim came out at 8.0 vs 14.9 in real files).
		int height = (int)std::lround(RangeMean(h, c, cols) / 31.0 * hmax);
		height = std::clamp(height, 0, hmax);
		if (hbits >= 5) {
			uint8_t w = Whiteness(RangeMean(lo, c, cols), RangeMean(mi, c, cols), RangeMean(hi, c, cols));
			body.u1((uint8_t)((w << 5) | (height & 0x1f)));
		} else {
			body.u1((uint8_t)(height & 0x0f)); // PWV2: 4-bit height only
		}
	}
	return body.b;
}

// PWV3 mono detail: 1 byte/col at native 150/s. byte = whiteness<<5 | height(5).
std::string BuildMonoDetail(const std::vector<uint8_t> &h, const std::vector<uint8_t> &lo,
                            const std::vector<uint8_t> &mi, const std::vector<uint8_t> &hi) {
	int n = (int)h.size();
	BE body;
	body.u4(1);            // len_entry_bytes
	body.u4((uint32_t)n);  // len_entries
	body.u4(0x00960000);   // unknown
	auto white = WhitenessSeries(lo, mi, hi);
	for (int c = 0; c < n; c++) {
		uint8_t w = c < (int)white.size() ? white[c] : 0;
		int hh = (int)std::lround(h[c] * WAVE_GAIN);
		body.u1((uint8_t)((w << 5) | (std::clamp(hh, 0, 31) & 0x1f)));
	}
	return body.b;
}

// PWV4 color preview: 6 bytes/col over 1200 cols.
// [unknown1, unknown2, energy_bottom_half, energy_low, energy_mid, energy_high]
std::string BuildColorPreview(const std::vector<uint8_t> &h, const std::vector<uint8_t> &lo,
                              const std::vector<uint8_t> &mi, const std::vector<uint8_t> &hi) {
	const int COLS = 1200;
	BE body;
	body.u4(6);
	body.u4((uint32_t)COLS);
	body.u4(0x00000000); // PWV4 third word is 0 (verified vs real rekordbox .EXT)
	for (int c = 0; c < COLS; c++) {
		double L = RangeMean(lo, c, COLS), M = RangeMean(mi, c, COLS), H = RangeMean(hi, c, COLS);
		auto b127to255 = [](double v) { return (uint8_t)std::clamp((int)std::lround(v * 2.0), 0, 255); };
		body.u1(Whiteness(L, M, H) * 36); // unknown1 ~ whiteness scaled
		body.u1(0);                        // unknown2
		body.u1(b127to255((L + M) / 2));   // energy_bottom_half
		body.u1(b127to255(L));             // energy_bottom_third
		body.u1(b127to255(M));             // energy_mid_third
		body.u1(b127to255(H));             // energy_top_third
	}
	return body.b;
}

// PWV5 color detail: 2 bytes/col at native 150/s. Big-endian bits:
// red(3)<<13 | green(3)<<10 | blue(3)<<7 | height(5)<<2 | unused(2).
std::string BuildColorDetail(const std::vector<uint8_t> &h, const std::vector<uint8_t> &lo,
                             const std::vector<uint8_t> &mi, const std::vector<uint8_t> &hi) {
	int n = (int)h.size();
	BE body;
	body.u4(2);
	body.u4((uint32_t)n);
	body.u4(0x00960305);
	auto to3 = [](uint8_t v) { return (uint16_t)std::clamp((int)std::lround(v / 127.0 * 7.0), 0, 7); };
	for (int c = 0; c < n; c++) {
		// rekordbox maps red<-low/bass, blue<-high (verified vs test/reference/).
		uint16_t red = to3(lo[c]);
		uint16_t grn = to3(mi[c]);
		uint16_t blu = to3(hi[c]);
		uint16_t hgt = std::min<int>(h[c], 31);
		body.u2((uint16_t)((red << 13) | (grn << 10) | (blu << 7) | (hgt << 2)));
	}
	return body.b;
}

// PVBR: seek index. Real rekordbox exports carry a 1608-byte body (402 u4 words)
// and it is entirely ZERO for the reference demo track — so we match that exactly
// (the tag isn't fully reverse-engineered, and AAC seeks via the MP4 container).
std::string BuildVbr(const std::string &) {
	BE body;
	for (int i = 0; i < 402; i++) body.u4(0);
	return body.b;
}

// PCOB cue list (type 0 = memory cues, 1 = hot cues) with PCPT entries. Empty
// cues reproduces the original byte-identical placeholder (num=0, 0xffffffff).
std::string BuildCue(uint32_t type, const std::vector<AnlzCue> &cues = {}) {
	BE b;
	b.u4(type);                                              // cue_list_type
	b.u2(0);                                                 // (pad)
	b.u2((uint16_t)cues.size());                             // num_cues
	b.u4(cues.empty() ? 0xffffffff : (uint32_t)cues.size()); // memory_count
	for (size_t i = 0; i < cues.size(); i++) {
		BE e;
		e.fourcc("PCPT");
		e.u4(0x1c);          // len_header
		e.u4(0x38);          // len_entry (56)
		e.u4(0);             // hot_cue: 0 = memory cue
		e.u4(0);             // status
		e.u4(0x00010000);    // (always 0x10000)
		e.u2(i == 0 ? 0xffff : (uint16_t)i);                          // order_first
		e.u2(i + 1 == cues.size() ? 0xffff : (uint16_t)(i + 1));      // order_last
		e.u1(1);             // cue_entry_type: 1 = memory cue (point)
		e.u1(0); e.u1(0); e.u1(0);
		e.u4(cues[i].time_ms);
		e.u4(0xffffffff);    // loop_time (not a loop)
		for (int k = 0; k < 16; k++) e.u1(0);
		b.raw(e.b);
	}
	return b.b;
}

// PCO2 extended cue list (nxs2/CDJ-3000): adds colour + comment per cue.
std::string BuildCue2(uint32_t type, const std::vector<AnlzCue> &cues = {}) {
	BE b;
	b.u4(type);                     // cue_list_type
	b.u2((uint16_t)cues.size());    // num_cues
	b.u2(0);                        // (pad)
	for (auto &c : cues) {
		std::string cmt;            // UTF-16BE comment + trailing NUL
		for (unsigned char ch : c.comment) { cmt.push_back(0); cmt.push_back((char)ch); }
		cmt.push_back(0); cmt.push_back(0);
		uint32_t len_comment = (uint32_t)cmt.size();
		BE e;
		e.fourcc("PCP2");
		e.u4(0x10);                 // len_header
		e.u4(48 + len_comment);     // len_entry
		e.u4(0);                    // hot_cue: 0 = memory cue
		e.u1(1);                    // cue_entry_type: point
		e.u1(0); e.u1(0); e.u1(0);
		e.u4(c.time_ms);
		e.u4(0xffffffff);           // loop_time
		e.u1(0);                    // color_id
		for (int k = 0; k < 7; k++) e.u1(0);
		e.u2(0); e.u2(0);           // loop numerator / denominator
		e.u4(len_comment);
		e.raw(cmt);
		e.u1(0);                                    // color_code
		e.u1((c.color >> 16) & 0xff);               // red
		e.u1((c.color >> 8) & 0xff);                // green
		e.u1(c.color & 0xff);                       // blue
		b.raw(e.b);
	}
	return b.b;
}

} // namespace

//===--------------------------------------------------------------------===//
// Plain-C++ builders (shared by the UDFs and the COPY/USB writer)
//===--------------------------------------------------------------------===//
std::string BuildAnlzDatBytes(const std::string &path, const std::vector<double> &beats, double bpm,
                              int downbeat, const std::vector<uint8_t> &h, const std::vector<uint8_t> &lo,
                              const std::vector<uint8_t> &mi, const std::vector<uint8_t> &hi,
                              const std::vector<AnlzCue> &cues) {
	// Tag order matches a real rekordbox export.
	BE sec;
	PutTag(sec, "PPTH", 0x10, BuildPath(path));
	PutTag(sec, "PVBR", 0x10, BuildVbr(path));
	PutTag(sec, "PQTZ", 0x18, BuildBeatGrid(beats, bpm, downbeat));
	PutTag(sec, "PWAV", 0x14, BuildMonoPreview(h, lo, mi, hi, 400, 5));
	PutTag(sec, "PWV2", 0x14, BuildMonoPreview(h, lo, mi, hi, 100, 4));
	PutTag(sec, "PCOB", 0x18, BuildCue(1));        // hot cues (empty)
	PutTag(sec, "PCOB", 0x18, BuildCue(0, cues));  // memory cues
	return Frame(sec.b);
}

std::string BuildAnlzExtBytes(const std::string &path, const std::vector<uint8_t> &h,
                              const std::vector<uint8_t> &lo, const std::vector<uint8_t> &mi,
                              const std::vector<uint8_t> &hi, const std::vector<AnlzCue> &cues) {
	BE sec;
	PutTag(sec, "PPTH", 0x10, BuildPath(path));
	PutTag(sec, "PWV3", 0x18, BuildMonoDetail(h, lo, mi, hi));
	// NOTE: a CDJ-2000NXS-validated .EXT contains ONLY PPTH + PWV3. The extra tags
	// below (PCOB/PCO2 extended cues, and the CDJ-3000-era PWV4/PWV5 colour
	// waveforms) are newer than the NXS firmware and appear to trip it (E-8709
	// COMMUNICATION ERROR when the player scans the ANLZ). Memory cues still ship
	// via PCOB in the .DAT. Re-enable once NXS loading is confirmed / for CDJ-3000.
	(void)cues;
	// PutTag(sec, "PCOB", 0x18, BuildCue(1));
	// PutTag(sec, "PCOB", 0x18, BuildCue(0, cues));
	// PutTag(sec, "PCO2", 0x14, BuildCue2(1));
	// PutTag(sec, "PCO2", 0x14, BuildCue2(0, cues));
	// PutTag(sec, "PWV5", 0x18, BuildColorDetail(h, lo, mi, hi));
	// PutTag(sec, "PWV4", 0x18, BuildColorPreview(h, lo, mi, hi));
	return Frame(sec.b);
}

//===--------------------------------------------------------------------===//
// UDFs
//===--------------------------------------------------------------------===//
static std::vector<uint8_t> U8Vec(const Value &v) {
	std::vector<uint8_t> out;
	if (v.IsNull()) return out;
	for (auto &c : ListValue::GetChildren(v))
		if (!c.IsNull()) out.push_back(c.GetValue<uint8_t>());
	return out;
}
static std::vector<double> DblVec(const Value &v) {
	std::vector<double> out;
	if (v.IsNull()) return out;
	for (auto &c : ListValue::GetChildren(v))
		if (!c.IsNull()) out.push_back(c.GetValue<double>());
	return out;
}

// rb_anlz_dat(path, beats[], bpm, downbeat, height[], low[], mid[], high[]) -> BLOB
void RbAnlzDatFun(DataChunk &args, ExpressionState &state, Vector &result) {
	for (idx_t r = 0; r < args.size(); r++) {
		Value pv = args.data[0].GetValue(r);
		if (pv.IsNull()) { result.SetValue(r, Value(LogicalType::BLOB)); continue; }
		std::string path = pv.ToString();
		auto beats = DblVec(args.data[1].GetValue(r));
		Value bpmv = args.data[2].GetValue(r), dbv = args.data[3].GetValue(r);
		double bpm = bpmv.IsNull() ? 0 : bpmv.GetValue<double>();
		int downbeat = dbv.IsNull() ? 0 : dbv.GetValue<int32_t>();
		auto h = U8Vec(args.data[4].GetValue(r)), lo = U8Vec(args.data[5].GetValue(r));
		auto mi = U8Vec(args.data[6].GetValue(r)), hi = U8Vec(args.data[7].GetValue(r));
		std::string bytes = BuildAnlzDatBytes(path, beats, bpm, downbeat, h, lo, mi, hi);
		result.SetValue(r, Value::BLOB(const_data_ptr_cast(bytes.data()), bytes.size()));
	}
}

// rb_anlz_ext(path, height[], low[], mid[], high[]) -> BLOB
void RbAnlzExtFun(DataChunk &args, ExpressionState &state, Vector &result) {
	for (idx_t r = 0; r < args.size(); r++) {
		Value pv = args.data[0].GetValue(r);
		if (pv.IsNull()) { result.SetValue(r, Value(LogicalType::BLOB)); continue; }
		std::string path = pv.ToString();
		auto h = U8Vec(args.data[1].GetValue(r)), lo = U8Vec(args.data[2].GetValue(r));
		auto mi = U8Vec(args.data[3].GetValue(r)), hi = U8Vec(args.data[4].GetValue(r));
		std::string bytes = BuildAnlzExtBytes(path, h, lo, mi, hi);
		result.SetValue(r, Value::BLOB(const_data_ptr_cast(bytes.data()), bytes.size()));
	}
}

} // namespace duckdb
