#include "Decoder_audio.h"
#include "VideoState.h"

int Decoder_audio_thread(void* arg) {
	VideoState* is = static_cast<VideoState*>(arg);
	AVPacket* packet = nullptr;
	AVFrame* frame = av_frame_alloc();

	while (!is->quit) {
		if (is->pause) {
			SDL_Delay(10);
			continue;
		}

		packet = is->audioPacketQueue->pop_for(100);
		if (!packet) continue;

		if (packet->stream_index != is->AudioStreamIndex) {
			av_packet_free(&packet);
			continue;
		}

		int ret = avcodec_send_packet(is->audioCodecContext, packet);
		av_packet_free(&packet);
		if (ret < 0) continue;

		while (ret >= 0) {
			ret = avcodec_receive_frame(is->audioCodecContext, frame);
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
			if (ret < 0) break;

			int64_t in_ch_layout = frame->channel_layout ? frame->channel_layout : av_get_default_channel_layout(frame->channels);
			int64_t out_ch_layout = is->channel_layout;
			int out_rate = (int)is->sample_rate;
			AVSampleFormat out_fmt = is->sample_fmt;
			int out_channels = is->channels;

			if (!is->swr_ctx) {
				is->swr_ctx = swr_alloc_set_opts(nullptr, out_ch_layout, out_fmt, out_rate,
										 in_ch_layout, (AVSampleFormat)frame->format, frame->sample_rate,
										 0, nullptr);
				if (!is->swr_ctx || swr_init(is->swr_ctx) < 0) {
					if (is->swr_ctx) {
						swr_free(&is->swr_ctx);
						is->swr_ctx = nullptr;
					}
					av_log(NULL, AV_LOG_ERROR, "Failed to init SwrContext for audio conversion\n");
					continue;
				}
			}

			int64_t delay = swr_get_delay(is->swr_ctx, frame->sample_rate);
			int out_samples = av_rescale_rnd(delay + frame->nb_samples, out_rate, frame->sample_rate, AV_ROUND_UP);

			uint8_t** converted = nullptr;
			int converted_linesize = 0;
			if (av_samples_alloc_array_and_samples(&converted, &converted_linesize, out_channels, out_samples, out_fmt, 0) < 0) {
				continue;
			}

			int converted_samples = swr_convert(is->swr_ctx, converted, out_samples, (const uint8_t**)frame->data, frame->nb_samples);
			if (converted_samples < 0) {
				av_freep(&converted[0]);
				av_freep(&converted);
				continue;
			}

			int data_size = av_samples_get_buffer_size(&converted_linesize, out_channels, converted_samples, out_fmt, 1);
			if (data_size > 0) {
				std::vector<uint8_t> v(data_size);
				memcpy(v.data(), converted[0], data_size);
				is->pcmQueue->push(std::move(v));
			}

			av_freep(&converted[0]);
			av_freep(&converted);
			av_frame_unref(frame);
		}
	}

	av_frame_free(&frame);
	return 0;
}


