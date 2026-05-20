#include "Decoder_video.h"
#include "VideoState.h"

int Decoder_video_thread(void* arg) {
    VideoState* is = static_cast<VideoState*>(arg);
    AVPacket* packet = nullptr;

    while (!is->quit) {
        if (is->pause) {
            SDL_Delay(10);
            continue;
        }

        packet = is->packetQueue->pop_for(100);
        if (!packet) continue;

        if (packet->stream_index != is->VideoStreamIndex) {
            av_packet_free(&packet);
            continue;
        }

        int ret = avcodec_send_packet(is->videoCodecContext, packet);
        av_packet_free(&packet);
        if (ret < 0) continue;

        AVFrame* frame = av_frame_alloc();
        while (ret >= 0) {
            ret = avcodec_receive_frame(is->videoCodecContext, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;
            if (frame->pts == AV_NOPTS_VALUE && frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                frame->pts = frame->best_effort_timestamp;
            }

            if (!is->video_frameQueue->push_blocking(frame)) {
                av_frame_free(&frame);
                return 0;
            }
            av_frame_unref(frame);
        }
        av_frame_free(&frame);
    }

    return 0;
}
