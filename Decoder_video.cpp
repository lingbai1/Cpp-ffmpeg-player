#include "Decoder_video.h"
#include "VideoState.h"
#include <vector>

int Decoder_video_thread(void* arg) {
    VideoState* is = static_cast<VideoState*>(arg);
    AVPacket* packet = nullptr;
    AVFrame* frame = av_frame_alloc();
    if (!frame) return -1;

    while (!is->quit) {
        if (is->pause) {
            SDL_Delay(10);
            continue;
        }

        const uint64_t packet_serial = is->seek_serial.load();
        packet = is->packetQueue->pop_for(100);
        if (!packet) continue;

        if (packet->stream_index != is->VideoStreamIndex) {
            av_packet_free(&packet);
            continue;
        }

        int ret = 0;
        std::vector<AVFrame*> decoded_frames;
        {
            std::lock_guard<std::mutex> seek_lk(is->seek_mutex);
            std::lock_guard<std::mutex> decode_lk(is->decode_mutex);
            if (packet_serial != is->seek_serial.load() || is->pause) {
                av_packet_free(&packet);
                continue;
            }
            ret = avcodec_send_packet(is->videoCodecContext, packet);
            av_packet_free(&packet);
            if (ret < 0) continue;

            while (ret >= 0 && !is->quit) {
                ret = avcodec_receive_frame(is->videoCodecContext, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) break;

                AVFrame* decoded = av_frame_alloc();
                if (!decoded || av_frame_ref(decoded, frame) < 0) {
                    av_frame_free(&decoded);
                    break;
                }
                if (decoded->pts == AV_NOPTS_VALUE &&
                    decoded->best_effort_timestamp != AV_NOPTS_VALUE) {
                    decoded->pts = decoded->best_effort_timestamp;
                }
                decoded_frames.push_back(decoded);
                av_frame_unref(frame);
            }
        }

        if (packet_serial != is->seek_serial.load() || is->pause) {
            for (AVFrame* decoded : decoded_frames) av_frame_free(&decoded);
            continue;
        }
        for (size_t i = 0; i < decoded_frames.size(); ++i) {
            AVFrame* decoded = decoded_frames[i];
            if (!is->video_frameQueue->push_blocking(decoded)) {
                av_frame_free(&decoded);
                for (size_t j = i + 1; j < decoded_frames.size(); ++j) {
                    av_frame_free(&decoded_frames[j]);
                }
                av_frame_free(&frame);
                return 0;
            }
            av_frame_free(&decoded);
        }
    }

    av_frame_free(&frame);
    return 0;
}
