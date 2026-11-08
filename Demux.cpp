#include "Demux.h"
#include "VideoState.h"
#include <cerrno>

namespace {
const int kMaxNetworkRetries = 3;

bool is_recoverable_network_error(int error_code) {
    return error_code == AVERROR(EAGAIN) ||
           error_code == AVERROR(ETIMEDOUT) ||
           error_code == AVERROR(ECONNRESET) ||
           error_code == AVERROR(ECONNREFUSED) ||
           error_code == AVERROR(EPIPE) ||
           error_code == AVERROR(ENETDOWN) ||
           error_code == AVERROR(ENETUNREACH) ||
           error_code == AVERROR(EHOSTUNREACH) ||
           error_code == AVERROR(EIO);
}

bool wait_before_retry(VideoState* is, int delay_ms) {
    for (int waited = 0; waited < delay_ms; waited += 50) {
        if (is->quit || is->abort_request) return false;
        SDL_Delay(50);
    }
    return true;
}
}


int Demux_Thread(void *arg)
{
    VideoState* is = static_cast<VideoState*>(arg);
    AVPacket* packet = av_packet_alloc();
    int network_failures = 0;

    while (!is->quit) {
        int read_ret = 0;
        uint64_t packet_serial = 0;
        {
            std::lock_guard<std::mutex> seek_lk(is->seek_mutex);
            if (is->pause) {
                SDL_Delay(10);
                continue;
            }
            packet_serial = is->seek_serial.load();
            std::lock_guard<std::mutex> lk(is->format_mutex);
            read_ret = av_read_frame(is->formatContext, packet);
        }

        if (read_ret < 0) {
            if (is->is_network_source && is_recoverable_network_error(read_ret) &&
                network_failures < kMaxNetworkRetries) {
                ++network_failures;
                is->network_reconnecting = true;
                is->network_error_code = read_ret;

                char errbuf[128];
                av_strerror(read_ret, errbuf, sizeof(errbuf));
                av_log(NULL, AV_LOG_WARNING,
                       "Network read failed (%s), retry %d/%d\n",
                       errbuf, network_failures, kMaxNetworkRetries);

                av_packet_unref(packet);
                if (!wait_before_retry(is, 500 * (1 << (network_failures - 1)))) {
                    break;
                }
                continue;
            }

            if (read_ret == AVERROR_EOF && !is->is_network_source) {
                // 保留 demux 线程，seek 后仍需要由它从新的文件位置继续读包。
                is->eof = true;
                av_packet_unref(packet);
                SDL_Delay(20);
                continue;
            }

            is->network_reconnecting = false;
            if (is->is_network_source && read_ret != AVERROR_EOF && !is->quit) {
                is->network_failed = true;
                is->network_error_code = read_ret;
            }
            if (!is->quit) is->eof = true;
            is->packetQueue->abort();
            is->audioPacketQueue->abort();
            break;
        }

        if (packet->stream_index == is->VideoStreamIndex) {
            if (!is->packetQueue->push_blocking(
                    packet, &is->seek_serial, packet_serial)) {
                av_packet_unref(packet);
                if (is->quit) break;
                continue;
            }
        } else if (packet->stream_index == is->AudioStreamIndex) {
            if (!is->audioPacketQueue->push_blocking(
                    packet, &is->seek_serial, packet_serial)) {
                av_packet_unref(packet);
                if (is->quit) break;
                continue;
            }
        }

        network_failures = 0;
        is->network_reconnecting = false;
        is->network_error_code = 0;

        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    return 0;
}