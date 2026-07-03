#pragma once
#include "duckdb.hpp"
#include "audio_decode.hpp"
#include <cstdint>
#include <vector>

namespace duckdb {

// Per-column waveform data at rekordbox's native 150 columns/second. Every ANLZ
// waveform tag (PWAV/PWV2 mono preview, PWV3 mono detail, PWV4 color preview,
// PWV5 color detail, PWV6/7 3-band) is a projection of these arrays.
struct Waveform {
	bool ok = false;
	int32_t cols = 0;
	std::vector<uint8_t> height; // 0..31  overall amplitude
	std::vector<uint8_t> low;    // 0..127 low-band energy   (0-200 Hz)   -> blue
	std::vector<uint8_t> mid;    // 0..127 mid-band energy   (200-2k Hz)  -> green/amber
	std::vector<uint8_t> high;   // 0..127 high-band energy  (>2k Hz)     -> white/red
};

// Compute the 150 col/s waveform from decoded mono audio via a 3-band split.
Waveform ComputeWaveform(const rbx::Audio &audio);

// rb_waveform(VARCHAR) -> STRUCT(cols, height[], low[], mid[], high[]).
void RbWaveformFun(DataChunk &args, ExpressionState &state, Vector &result);

} // namespace duckdb
