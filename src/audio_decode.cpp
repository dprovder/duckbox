#include "audio_decode.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
}

namespace rbx {

// Standard libav* decode -> resample to mono float @ target_rate.
// Trimmed for readability; production code should check every return value.
Audio DecodeMono(const std::string &path, int target_rate) {
	Audio out;
	AVFormatContext *fmt = nullptr;
	if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
		out.error = "cannot open " + path; return out;
	}
	avformat_find_stream_info(fmt, nullptr);
	int astream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	if (astream < 0) { out.error = "no audio stream"; avformat_close_input(&fmt); return out; }

	AVCodecParameters *par = fmt->streams[astream]->codecpar;
	const AVCodec *codec = avcodec_find_decoder(par->codec_id);
	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	avcodec_parameters_to_context(ctx, par);
	avcodec_open2(ctx, codec, nullptr);

	SwrContext *swr = swr_alloc();
	AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
	av_opt_set_chlayout(swr, "in_chlayout", &ctx->ch_layout, 0);
	av_opt_set_chlayout(swr, "out_chlayout", &mono, 0);
	av_opt_set_int(swr, "in_sample_rate", ctx->sample_rate, 0);
	av_opt_set_int(swr, "out_sample_rate", target_rate, 0);
	av_opt_set_sample_fmt(swr, "in_sample_fmt", ctx->sample_fmt, 0);
	av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
	swr_init(swr);

	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	std::vector<float> buf(8192);
	while (av_read_frame(fmt, pkt) >= 0) {
		if (pkt->stream_index == astream && avcodec_send_packet(ctx, pkt) == 0) {
			while (avcodec_receive_frame(ctx, frame) == 0) {
				int max = swr_get_out_samples(swr, frame->nb_samples);
				if ((int)buf.size() < max) buf.resize(max);
				uint8_t *o = (uint8_t *)buf.data();
				int n = swr_convert(swr, &o, max,
				                    (const uint8_t **)frame->data, frame->nb_samples);
				out.samples.insert(out.samples.end(), buf.begin(), buf.begin() + n);
			}
		}
		av_packet_unref(pkt);
	}
	out.sample_rate = target_rate;
	out.duration_sec = out.samples.size() / (double)target_rate;
	out.ok = !out.samples.empty();

	av_frame_free(&frame); av_packet_free(&pkt);
	swr_free(&swr); avcodec_free_context(&ctx); avformat_close_input(&fmt);
	return out;
}

// Return the raw bytes of the embedded cover art (the attached-picture stream),
// or empty if there is none. attached_pic is a pre-populated AVPacket on the stream.
std::vector<uint8_t> ExtractArtwork(const std::string &path) {
	std::vector<uint8_t> out;
	AVFormatContext *fmt = nullptr;
	if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
		return out;
	}
	avformat_find_stream_info(fmt, nullptr);
	for (unsigned i = 0; i < fmt->nb_streams; i++) {
		AVStream *st = fmt->streams[i];
		if (st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
			const AVPacket &pic = st->attached_pic;
			if (pic.data && pic.size > 0) {
				out.assign(pic.data, pic.data + pic.size);
			}
			break;
		}
	}
	avformat_close_input(&fmt);
	return out;
}

} // namespace rbx
