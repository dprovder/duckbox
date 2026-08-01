//===----------------------------------------------------------------------===//
// anlz_index.hpp — where a CDJ looks for a track's analysis.
//
// A player does not trust `analyze_path`. It derives the analysis folder from
// the track's USB-relative file path and reads only what sits exactly there:
//
//     PIONEER/USBANLZ/P<xxx>/<NNNNNNNN>/ANLZ0000.DAT
//
// NNNNNNNN is an index into 200003 slots, recovered by probing a CDJ-2000NXS:
//
//     index = ((H0 + sum(c_i * W_i)) mod 2^32) mod 200003
//
// with i the 0-based offset from the END of the path and W_i a per-position
// constant. Confirmed against hardware: the player independently chose 00000378
// for /Contents/TE3F.mp3 and 00011063 for /Contents/TC2D.mp3, both as predicted.
//
// Only the weights for the last three characters before ".mp3" are known as
// exact 32-bit values, so the index is computable for names of the form
// T<c><c><c>.mp3 -- everything else in the path folds into H0 and would need
// re-solving. IndexFor() returns false for anything else; callers fall back to
// ui/duckbox-learn, which reads the folder back off a drive the player has seen.
//
// P<xxx> is a second, unsolved hash (not a function of the index or of the raw
// 32-bit value; searched every shift 0-31 against moduli 2-400). It does not
// need solving: write the analysis into all 128 P000-P07F folders at the
// computed index and the player finds whichever it picked. Confirmed working.
//
// See docs/ANLZ_PATH.md and docs/anlz_index.json.
//===----------------------------------------------------------------------===//
#pragma once

#include <cstdint>
#include <string>

namespace duckdb {
namespace anlz {

constexpr uint32_t kModulus = 200003;
constexpr int kPFolders = 128;  // P000..P07F, covering the unsolved P hash

// Solved for the "/Contents/T<c><c><c>.mp3" shape; the fixed prefix, the 'T'
// and the ".mp3" are all absorbed into H0.
constexpr uint32_t kH0 = 0xF87C8290u;
constexpr uint32_t kW4 = 223688027u;
constexpr uint32_t kW5 = 1674353160u;
constexpr uint32_t kW6 = 2093151154u;

// True when `usb_path` matches the shape the constants were solved for.
inline bool IsComputable(const std::string &usb_path) {
	static const std::string kPrefix = "/Contents/";
	if (usb_path.size() != kPrefix.size() + 8) return false;
	if (usb_path.compare(0, kPrefix.size(), kPrefix) != 0) return false;
	if (usb_path[kPrefix.size()] != 'T') return false;
	return usb_path.compare(usb_path.size() - 4, 4, ".mp3") == 0;
}

// Analysis-folder index for a USB-relative path, e.g. "/Contents/TA1B.mp3".
// Returns false when the path is outside the solved shape.
inline bool IndexFor(const std::string &usb_path, uint32_t &out) {
	if (!IsComputable(usb_path)) return false;
	const uint32_t w[3] = {kW4, kW5, kW6};
	uint32_t sum = kH0;
	for (int o = 4; o <= 6; o++) {
		sum += (uint32_t)(unsigned char)usb_path[usb_path.size() - 1 - o] * w[o - 4];
	}
	out = sum % kModulus;  // sum already wrapped mod 2^32 by unsigned arithmetic
	return true;
}

} // namespace anlz
} // namespace duckdb
