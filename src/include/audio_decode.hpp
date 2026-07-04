#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace rbx {

// Decoded mono audio at a fixed sample rate, for analysis.
struct Audio {
	std::vector<float> samples; // interleaved mono
	int sample_rate = 0;
	double duration_sec = 0.0;
	bool ok = false;
	std::string error;
};

// Decode any FFmpeg-supported file to mono float PCM at `target_rate`.
// Mirrors what our standalone `kfcli` did (ffmpeg -ac 1 -ar 44100 -f s16le),
// but in-process via libav* so the extension is self-contained.
Audio DecodeMono(const std::string &path, int target_rate = 44100);

// Extract the embedded cover art (attached picture) from a media file, as the
// raw encoded image bytes (JPEG/PNG). Empty if the file has no artwork.
std::vector<uint8_t> ExtractArtwork(const std::string &path);

} // namespace rbx
