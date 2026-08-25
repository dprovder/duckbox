#pragma once
#include "duckdb.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {

// A cue point to write into the ANLZ cue lists (exported as a memory cue).
struct AnlzCue {
	uint32_t time_ms = 0;
	uint32_t color = 0;      // packed 0xRRGGBB (0 = none)
	std::string comment;     // ASCII/UTF-8; written as UTF-16BE in PCO2
};

// Plain-C++ ANLZ builders (shared by the UDFs and the COPY/USB writer).
std::string BuildAnlzDatBytes(const std::string &path, const std::vector<double> &beats, double bpm,
                              int downbeat, const std::vector<uint8_t> &height,
                              const std::vector<uint8_t> &low, const std::vector<uint8_t> &mid,
                              const std::vector<uint8_t> &high,
                              const std::vector<AnlzCue> &cues = {});
// `colour` adds the CDJ-3000-era tags (extended cues + the two colour waveforms).
// A CDJ-2000NXS rejects them -- see the note in BuildAnlzExtBytes -- so it is off
// by default and reached with COPY ... (FORMAT rekordbox, colour true).
std::string BuildAnlzExtBytes(const std::string &path, const std::vector<uint8_t> &height,
                              const std::vector<uint8_t> &low, const std::vector<uint8_t> &mid,
                              const std::vector<uint8_t> &high,
                              const std::vector<AnlzCue> &cues = {}, bool colour = false);

// rb_anlz_dat(path, beats DOUBLE[], bpm, downbeat, height[], low[], mid[], high[])
//   -> BLOB : ANLZ0000.DAT (PPTH, PQTZ, PWAV, PWV2, PVBR).
// rb_anlz_ext(path, height[], low[], mid[], high[])
//   -> BLOB : ANLZ0000.EXT (PPTH, PWV3, PWV4, PWV5 color waveforms).
// Both are pure projections of the stored beatgrid + waveform arrays, validated
// by round-tripping through the vendored Kaitai parser.
void RbAnlzDatFun(DataChunk &args, ExpressionState &state, Vector &result);
void RbAnlzExtFun(DataChunk &args, ExpressionState &state, Vector &result);

} // namespace duckdb
