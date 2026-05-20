#include "Demux.h"
#include "VideoState.h"



int Demux_Thread(void *arg)
{
    VideoState* is = static_cast<VideoState*>(arg);
    AVPacket* packet = av_packet_alloc();

    while (!is->quit) {
        if (is->pause) {
            SDL_Delay(10);
            continue;
        }
        
        int read_ret = 0;
        {
            std::lock_guard<std::mutex> lk(is->format_mutex);
            read_ret = av_read_frame(is->formatContext, packet);
        }

        if (read_ret < 0) {
            is->packetQueue->abort();
            is->audioPacketQueue->abort();
            break;
        }

        if (packet->stream_index == is->VideoStreamIndex) {
            if (!is->packetQueue->push_blocking(packet)) {
                av_packet_unref(packet);
                break;
            }
        } else if (packet->stream_index == is->AudioStreamIndex) {
            if (!is->audioPacketQueue->push_blocking(packet)) {
                av_packet_unref(packet);
                break;
            }
        }
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    return 0;
}