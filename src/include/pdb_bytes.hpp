// Pure byte-level primitives for writing DeviceSQL (.pdb) structures.
// Deliberately free of any DuckDB dependency so they can be unit-tested directly
// against reference vectors (see test/pdb_bytes_test.cpp, which checks these
// against the expected outputs published by the kimtore/rex project).
#pragma once

#include <cstdint>
#include <string>

namespace rbx {

// Little-endian byte assembler (the .pdb is little-endian).
struct LE {
	std::string b;
	void u1(uint8_t v) { b.push_back((char)v); }
	void u2(uint16_t v) { u1(v & 0xff); u1(v >> 8); }
	void u4(uint32_t v) { u2(v & 0xffff); u2(v >> 16); }
	void raw(const std::string &s) { b += s; }
};

inline std::string Utf16LE(const std::string &s) {
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

inline bool IsAscii(const std::string &s) {
	for (unsigned char c : s) if (c >= 0x80) return false;
	return true;
}

// True when Dss() will emit a long-form string (0x40 ascii or 0x90 UTF-16), whose
// payload sits 4 bytes past the start and therefore needs 4-byte alignment.
inline bool IsLongForm(const std::string &s) { return !IsAscii(s) || s.size() > 126; }

// DeviceSQL string: short ASCII (odd flag), long ASCII (0x40), or long UTF-16LE (0x90).
inline std::string Dss(const std::string &s) {
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

// An ISRC is stored with the UTF-16 kind byte but holds ASCII: a 0x03 marker,
// the code, then a NUL. Length counts the 4-byte header plus those two bytes.
inline std::string DssIsrc(const std::string &s) {
	if (s.empty()) return Dss("");
	LE o;
	o.u1(0x90); o.u2((uint16_t)(s.size() + 6)); o.u1(0);
	o.u1(0x03); o.raw(s); o.u1(0);
	return o.b;
}

// Artist/album name offsets shift when the name needs the long form, so the
// string still starts on a 4-byte boundary (real exports use 0x0c / 0x18).
inline std::string ArtistRowBytes(uint32_t id, uint16_t index_shift, const std::string &name) {
	LE r; r.u2(0x60); r.u2(index_shift); r.u4(id); r.u1(0x03);
	if (IsLongForm(name)) { r.u1(0x0c); r.u2(0); } else { r.u1(0x0a); }
	r.raw(Dss(name));
	return r.b;
}

} // namespace rbx
