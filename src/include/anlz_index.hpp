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

// Position-from-the-end weights. These are properties of the position, not of
// the filename, so they are shared by every template below.
constexpr uint32_t kW4 = 223688027u;
constexpr uint32_t kW5 = 1674353160u;
constexpr uint32_t kW6 = 2093151154u;

// H0 absorbs everything fixed about a template: the "/Contents/" prefix, the
// leading 'T' and the extension. It is only ever pinned to a residue class mod
// kModulus -- observations narrow the 32-bit lift but cannot cross the class,
// because candidates form an arithmetic progression of step kModulus and
// intersecting them can never go below it. Only a sample that straddles a 2^32
// wrap discriminates, so the surviving set stays wide unless heavily sampled.
//
// Stored as (first candidate, how many), the set being contiguous. Where the
// survivors disagree the caller gets every distinct index and writes them all;
// measured over the whole namespace that is at most two.
struct Template {
	const char *ext;      // 4 chars including the dot
	uint32_t h0_first;    // lowest surviving H0
	uint32_t h0_count;    // candidates, each h0_first + n*kModulus
};

// .mp3: 39 samples -> 5 candidates, unanimous on every name in the namespace.
// .m4a: 7 samples  -> 6414 candidates, unanimous on ~2/3 of names, else 2-way.
constexpr Template kTemplates[] = {
    {".mp3", 0xF87C8290u, 5},
    {".m4a", 0x18A627A9u, 6414},
};

inline const Template *TemplateFor(const std::string &usb_path) {
	static const std::string kPrefix = "/Contents/";
	if (usb_path.size() != kPrefix.size() + 8) return nullptr;
	if (usb_path.compare(0, kPrefix.size(), kPrefix) != 0) return nullptr;
	if (usb_path[kPrefix.size()] != 'T') return nullptr;
	for (auto &t : kTemplates) {
		if (usb_path.compare(usb_path.size() - 4, 4, t.ext) == 0) return &t;
	}
	return nullptr;
}

inline bool IsComputable(const std::string &usb_path) { return TemplateFor(usb_path) != nullptr; }

// Every index the analysis might need to live at, for a USB-relative path such
// as "/Contents/TA1B.mp3". Returns how many were written to `out` (0 when the
// path is outside every solved template, 1 when the constant is pinned well
// enough to agree, 2 when it is not). Writing all of them is what makes the
// address correct despite an incompletely lifted H0.
inline int IndexCandidates(const std::string &usb_path, uint32_t out[2]) {
	const Template *t = TemplateFor(usb_path);
	if (!t) return 0;
	const uint32_t w[3] = {kW4, kW5, kW6};
	uint32_t sig = 0;
	for (int o = 4; o <= 6; o++) {
		sig += (uint32_t)(unsigned char)usb_path[usb_path.size() - 1 - o] * w[o - 4];
	}
	int n = 0;
	for (uint32_t i = 0; i < t->h0_count; i++) {
		uint32_t idx = (t->h0_first + i * kModulus + sig) % kModulus;
		bool seen = false;
		for (int j = 0; j < n; j++) if (out[j] == idx) seen = true;
		if (!seen) {
			if (n == 2) return 2;  // bounded by measurement; never observed
			out[n++] = idx;
		}
	}
	return n;
}

// Convenience for callers that only need one: true when the index is unambiguous.
inline bool IndexFor(const std::string &usb_path, uint32_t &out) {
	uint32_t c[2];
	if (IndexCandidates(usb_path, c) != 1) return false;
	out = c[0];
	return true;
}

} // namespace anlz
} // namespace duckdb
