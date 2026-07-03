#pragma once
#include "duckdb.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {

// Plain-C++ ANLZ builders (shared by the UDFs and the COPY/USB writer).
std::string BuildAnlzDatBytes(const std::string &path, const std::vector<double> &beats, double bpm,
                              int downbeat, const std::vector<uint8_t> &height,
                              const std::vector<uint8_t> &low, const std::vector<uint8_t> &mid,
                              const std::vector<uint8_t> &high);
std::string BuildAnlzExtBytes(const std::string &path, const std::vector<uint8_t> &height,
                              const std::vector<uint8_t> &low, const std::vector<uint8_t> &mid,
                              const std::vector<uint8_t> &high);

// rb_anlz_dat(path, beats DOUBLE[], bpm, downbeat, height[], low[], mid[], high[])
//   -> BLOB : ANLZ0000.DAT (PPTH, PQTZ, PWAV, PWV2, PVBR).
// rb_anlz_ext(path, height[], low[], mid[], high[])
//   -> BLOB : ANLZ0000.EXT (PPTH, PWV3, PWV4, PWV5 color waveforms).
// Both are pure projections of the stored beatgrid + waveform arrays, validated
// by round-tripping through the vendored Kaitai parser.
void RbAnlzDatFun(DataChunk &args, ExpressionState &state, Vector &result);
void RbAnlzExtFun(DataChunk &args, ExpressionState &state, Vector &result);

} // namespace duckdb
