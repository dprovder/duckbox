// Waveform DSP: split decoded audio into low/mid/high bands and reduce to
// per-column energies at rekordbox's native 150 columns/second.
//
// Bands (RBJ biquad, 2nd order): low = LP 200 Hz, high = HP 2000 Hz,
// mid = band-pass 200..2000 Hz. Overall height = per-column peak amplitude.
// Scaling is absolute (not per-track normalized) so quiet passages read quiet;
// the exact gain is a calibration knob (real rekordbox values need a reference
// export to match byte-for-byte — see NOTES).

#include "waveform.hpp"

#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"

#include <algorithm>
#include <cmath>

namespace duckdb {
namespace {

constexpr double COLS_PER_SEC = 150.0;

// One RBJ biquad section.
struct Biquad {
	double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
	double z1 = 0, z2 = 0;
	inline float process(float x) {
		double y = b0 * x + z1;
		z1 = b1 * x - a1 * y + z2;
		z2 = b2 * x - a2 * y;
		return (float)y;
	}
	static Biquad LowPass(double fc, double fs, double q = 0.707) {
		double w = 2 * M_PI * fc / fs, cw = std::cos(w), sw = std::sin(w), al = sw / (2 * q);
		double a0 = 1 + al;
		Biquad f;
		f.b0 = (1 - cw) / 2 / a0; f.b1 = (1 - cw) / a0; f.b2 = f.b0;
		f.a1 = (-2 * cw) / a0; f.a2 = (1 - al) / a0;
		return f;
	}
	static Biquad HighPass(double fc, double fs, double q = 0.707) {
		double w = 2 * M_PI * fc / fs, cw = std::cos(w), sw = std::sin(w), al = sw / (2 * q);
		double a0 = 1 + al;
		Biquad f;
		f.b0 = (1 + cw) / 2 / a0; f.b1 = -(1 + cw) / a0; f.b2 = f.b0;
		f.a1 = (-2 * cw) / a0; f.a2 = (1 - al) / a0;
		return f;
	}
};

std::vector<float> Filter(const std::vector<float> &x, Biquad f) {
	std::vector<float> y(x.size());
	for (size_t i = 0; i < x.size(); i++) y[i] = f.process(x[i]);
	return y;
}

uint8_t Clamp(double v, int hi) {
	if (v < 0) v = 0;
	if (v > hi) v = hi;
	return (uint8_t)std::lround(v);
}

} // namespace

Waveform ComputeWaveform(const rbx::Audio &a) {
	Waveform w;
	if (!a.ok || a.samples.empty()) return w;
	const double fs = a.sample_rate;
	const auto &s = a.samples;

	// Band-split the whole signal.
	auto lowsig = Filter(s, Biquad::LowPass(200.0, fs));
	auto midsig = Filter(Filter(s, Biquad::HighPass(200.0, fs)), Biquad::LowPass(2000.0, fs));
	auto highsig = Filter(s, Biquad::HighPass(2000.0, fs));

	int cols = (int)std::llround((double)s.size() / fs * COLS_PER_SEC);
	if (cols < 1) cols = 1;
	w.cols = cols;
	w.height.reserve(cols); w.low.reserve(cols); w.mid.reserve(cols); w.high.reserve(cols);

	auto rms = [](const std::vector<float> &v, size_t lo, size_t hi) {
		double acc = 0; for (size_t i = lo; i < hi; i++) acc += (double)v[i] * v[i];
		return std::sqrt(acc / std::max<size_t>(1, hi - lo));
	};

	// Gain knobs (calibration): amplitude ~[0,1] -> height 0..31, band RMS -> 0..127.
	const double H_GAIN = 31.0 * 1.4, B_GAIN = 127.0 * 3.0;
	size_t n = s.size();
	for (int c = 0; c < cols; c++) {
		size_t lo = (size_t)((uint64_t)c * n / cols);
		size_t hi = (size_t)((uint64_t)(c + 1) * n / cols);
		if (hi <= lo) hi = std::min(n, lo + 1);
		float peak = 0;
		for (size_t i = lo; i < hi; i++) peak = std::max(peak, std::fabs(s[i]));
		w.height.push_back(Clamp(peak * H_GAIN, 31));
		w.low.push_back(Clamp(rms(lowsig, lo, hi) * B_GAIN, 127));
		w.mid.push_back(Clamp(rms(midsig, lo, hi) * B_GAIN, 127));
		w.high.push_back(Clamp(rms(highsig, lo, hi) * B_GAIN, 127));
	}
	w.ok = true;
	return w;
}

// rb_waveform(VARCHAR) -> STRUCT(cols INT, height UTINYINT[], low/mid/high UTINYINT[]).
void RbWaveformFun(DataChunk &args, ExpressionState &state, Vector &result) {
	for (idx_t r = 0; r < args.size(); r++) {
		Value pv = args.data[0].GetValue(r);
		Waveform w;
		if (!pv.IsNull()) w = ComputeWaveform(rbx::DecodeMono(pv.ToString(), 44100));
		auto u8list = [&](const std::vector<uint8_t> &v) {
			vector<Value> out; out.reserve(v.size());
			for (uint8_t x : v) out.push_back(Value::UTINYINT(x));
			return Value::LIST(LogicalType::UTINYINT, std::move(out));
		};
		child_list_t<Value> f;
		f.emplace_back("cols", Value::INTEGER(w.cols));
		f.emplace_back("height", u8list(w.height));
		f.emplace_back("low", u8list(w.low));
		f.emplace_back("mid", u8list(w.mid));
		f.emplace_back("high", u8list(w.high));
		result.SetValue(r, Value::STRUCT(std::move(f)));
	}
}

} // namespace duckdb
