//===----------------------------------------------------------------------===//
// anlz_index.hpp — where a CDJ looks for a track's analysis.
//
// A player does not trust `analyze_path`. It derives the analysis folder from
// the track's USB-relative file path and reads only what sits exactly there:
//
//     PIONEER/USBANLZ/P<xxx>/<NNNNNNNN>/ANLZ0000.DAT
//
// Both halves are computed from one 32-bit hash of that path, taken over its
// UTF-16 code units, two rounds a character:
//
//     h = 0
//     for c in utf16(path):
//         h = h * 23497 + c
//         h = h * 37813 + c
//     index = h mod 200003
//
// The double round is why every rolling-hash fit failed: per character the
// recurrence is h*888492061 + c*37814, not h*M + c.
//
// P is not a second hash. It is seven bits gathered out of the index --
// 16, 13, 9, 7, 6, 2, 0 -- packed in that order. A bit scatter, so it looked
// unrelated to the index under every shift-and-modulus search.
//
// Recovered from analyzer::CreateAnlzFileFolderPath in rekordbox 7 and checked
// two ways: it reproduces every harvested rekordbox address, and it reproduces
// both folders a CDJ-2000NXS chose on its own (P014/00000378 for
// /Contents/TE3F.mp3, P045/00011063 for /Contents/TC2D.mp3). See docs/ANLZ_PATH.md.
//===----------------------------------------------------------------------===//
#pragma once

#include <cstdint>
#include <string>

namespace duckdb {
namespace anlz {

constexpr uint32_t kModulus = 200003;
constexpr int kPFolders = 128;  // P000..P07F

// The hash runs over UTF-16 code units, so a non-BMP character contributes its
// surrogate pair and an accented one contributes whatever form the path is in.
// Feed the path exactly as it is written to the drive.
inline uint32_t PathHash(const std::string &utf8_path) {
	uint32_t h = 0;
	auto round = [&h](uint32_t c) {
		h = h * 23497u + c;
		h = h * 37813u + c;
	};
	for (size_t i = 0; i < utf8_path.size();) {
		unsigned char b = (unsigned char)utf8_path[i];
		uint32_t cp;
		int n;
		if (b < 0x80)             { cp = b;         n = 1; }
		else if ((b & 0xE0) == 0xC0) { cp = b & 0x1F;  n = 2; }
		else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F;  n = 3; }
		else if ((b & 0xF8) == 0xF0) { cp = b & 0x07;  n = 4; }
		else                      { cp = 0xFFFD;    n = 1; }
		if (i + (size_t)n > utf8_path.size()) { cp = 0xFFFD; n = 1; }
		for (int k = 1; k < n; k++) cp = (cp << 6) | ((unsigned char)utf8_path[i + k] & 0x3F);
		i += (size_t)n;
		if (cp >= 0x10000) {           // surrogate pair, as UTF-16 would encode it
			cp -= 0x10000;
			round(0xD800 + (cp >> 10));
			round(0xDC00 + (cp & 0x3FF));
		} else {
			round(cp);
		}
	}
	return h;
}

// The analysis index for a USB-relative path such as "/Contents/TA1B.mp3".
inline uint32_t IndexFor(const std::string &usb_path) { return PathHash(usb_path) % kModulus; }

// The P folder, gathered out of the index.
inline int PFolderFor(uint32_t index) {
	return (int)((index & 1u) | ((index >> 1) & 2u) | ((index >> 4) & 4u) | ((index >> 4) & 8u) |
	             ((index >> 5) & 0x10u) | ((index >> 8) & 0x20u) | ((index >> 10) & 0x40u));
}

} // namespace anlz
} // namespace duckdb
