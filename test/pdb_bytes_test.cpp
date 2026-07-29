// Checks our DeviceSQL byte encoders against the reference vectors published by
// the kimtore/rex project (pkg/rekordbox/dstring/string_test.go and
// pkg/rekordbox/artist/artist_test.go), which were themselves derived from real
// rekordbox exports.
//
//   c++ -std=c++17 -I src/include test/pdb_bytes_test.cpp -o /tmp/pdb_bytes_test && /tmp/pdb_bytes_test
#include "pdb_bytes.hpp"

#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;

static void Check(const char *name, const std::string &got, const std::vector<uint8_t> &want) {
	std::string w((const char *)want.data(), want.size());
	if (got == w) { printf("  PASS  %s\n", name); return; }
	failures++;
	printf("  FAIL  %s\n        want (%zu):", name, w.size());
	for (unsigned char c : w) printf(" %02x", c);
	printf("\n        got  (%zu):", got.size());
	for (unsigned char c : got) printf(" %02x", c);
	printf("\n");
}

int main() {
	using namespace rbx;
	printf("DeviceSQL encoders vs kimtore/rex reference vectors\n");

	// dstring: empty string
	Check("short ascii, empty", Dss(""), {0x03});

	// dstring: short ascii
	Check("short ascii", Dss("/PIONEER/USBANLZ/P05B/0001069F/ANLZ0000.DAT"),
	      {0x59, 0x2f, 0x50, 0x49, 0x4f, 0x4e, 0x45, 0x45, 0x52, 0x2f, 0x55, 0x53,
	       0x42, 0x41, 0x4e, 0x4c, 0x5a, 0x2f, 0x50, 0x30, 0x35, 0x42, 0x2f, 0x30,
	       0x30, 0x30, 0x31, 0x30, 0x36, 0x39, 0x46, 0x2f, 0x41, 0x4e, 0x4c, 0x5a,
	       0x30, 0x30, 0x30, 0x30, 0x2e, 0x44, 0x41, 0x54});

	// dstring: long UTF-16LE (non-ASCII forces the 0x90 form)
	Check("long utf16le", Dss("R\xc3\xb8""dh\xc3\xa5""d feat. Vril"),
	      {0x90, 0x26, 0x00, 0x00, 0x52, 0x00, 0xf8, 0x00, 0x64, 0x00, 0x68, 0x00,
	       0xe5, 0x00, 0x64, 0x00, 0x20, 0x00, 0x66, 0x00, 0x65, 0x00, 0x61, 0x00,
	       0x74, 0x00, 0x2e, 0x00, 0x20, 0x00, 0x56, 0x00, 0x72, 0x00, 0x69, 0x00,
	       0x6c, 0x00});

	// dstring: ISRC (0x90 kind byte but ASCII payload)
	Check("isrc", DssIsrc("GBJX38209003"),
	      {0x90, 0x12, 0x00, 0x00, 0x03, 0x47, 0x42, 0x4a, 0x58, 0x33, 0x38, 0x32,
	       0x30, 0x39, 0x30, 0x30, 0x33, 0x00});

	// artist row
	Check("artist row", ArtistRowBytes(118, 0x40, "Totally Enormous Extinct Dinosaurs"),
	      {0x60, 0x00, 0x40, 0x00, 0x76, 0x00, 0x00, 0x00, 0x03, 0x0a, 0x47, 0x54,
	       0x6f, 0x74, 0x61, 0x6c, 0x6c, 0x79, 0x20, 0x45, 0x6e, 0x6f, 0x72, 0x6d,
	       0x6f, 0x75, 0x73, 0x20, 0x45, 0x78, 0x74, 0x69, 0x6e, 0x63, 0x74, 0x20,
	       0x44, 0x69, 0x6e, 0x6f, 0x73, 0x61, 0x75, 0x72, 0x73});

	// Our own invariant: long-form strings must be 4-byte alignable, and a
	// non-ASCII artist name moves the offset to 0x0c so the payload stays aligned.
	std::string a = ArtistRowBytes(1, 0, "Andr\xc3\xa9s");
	Check("artist row, non-ascii name shifts offset to 0x0c",
	      std::string(1, a[9]), {0x0c});

	// Full track-row string layout, against the expected offsets in rex's
	// TestTrack_MarshalBinary. Note rex's OWN code fails this assertion: their
	// expected bytes (taken from real rekordbox) pad comment(0xec) -> title(0xf0)
	// so the UTF-16 title is 4-byte aligned, but their encoder packs it at 0xed.
	// Our layout rule reproduces the padded, aligned offsets.
	{
		std::string strs[21] = {
			"GBJX38209003", "", "2", "2", "", "", "", "ON", "", "", "2022-07-27",
			"", "", "", "/PIONEER/USBANLZ/P03A/0000339E/ANLZ0000.DAT", "2022-07-27",
			"", "Wir Leben F\xc3\xbc""r Die Nacht", "",
			"Dax J - Wir Leben Fur Die Nacht.flac",
			"/meteor/techno/Dax J - Wir Leben Fur Die Nacht.flac"};
		const uint16_t want[21] = {0x88, 0x9a, 0x9b, 0x9d, 0x9f, 0xa0, 0xa1, 0xa2,
		                           0xa5, 0xa6, 0xa7, 0xb2, 0xb3, 0xb4, 0xb5, 0xe1,
		                           0xec, 0xf0, 0x122, 0x123, 0x148};
		std::string blob; uint32_t base = 0x5e + 21 * 2; bool ok = true;
		for (int i = 0; i < 21; i++) {
			std::string enc = (i == 0) ? DssIsrc(strs[i]) : Dss(strs[i]);
			bool longform = (i == 0) ? !strs[i].empty() : IsLongForm(strs[i]);
			if (longform) while ((base + blob.size()) % 4) blob += '\0';
			if ((uint16_t)(base + blob.size()) != want[i]) {
				printf("  FAIL  track string layout: slot %d at 0x%x, want 0x%x\n",
				       i, (unsigned)(base + blob.size()), want[i]);
				ok = false; failures++; break;
			}
			blob += enc;
		}
		if (ok) printf("  PASS  track row string offsets (rex expected vector; rex's own code fails this)\n");
	}

	printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
	return failures ? 1 : 0;
}
