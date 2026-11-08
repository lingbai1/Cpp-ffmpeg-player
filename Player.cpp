extern "C"{
    #include<libavformat/avformat.h>
    #include<libavcodec/avcodec.h>
    #include<libavutil/avutil.h>
    #include<libavutil/pixdesc.h>
    #include<SDL2/SDL.h>
    #include<libavutil/fifo.h>
    #include<libswscale/swscale.h>
    #include<libswresample/swresample.h>
    #include<libavutil/imgutils.h>
    #include<libavutil/avstring.h>
    #include<libavutil/opt.h>
}

#include<iostream>
#include<thread>
#include<mutex>
#include<condition_variable>
#include<deque>
#include<chrono>
#include<vector>
#include<algorithm>
#include<memory>
#include<cstring>
#include<cstdio>
#include<cmath>
#include"VideoState.h"
#include"PacketQueue.h"
#include"FrameQueue.h"
#include"PCMQueue.h"
#include"Demux.h"
#include"Decoder_video.h"
#include"Decoder_audio.h"
#include"AudioBuffer.h"
#include"FrameItem.h"
#include"Player.h"

// 全局初始化标志（确保只初始化一次）
static bool g_sdl_initialized = false;
static bool g_network_initialized = false;

bool sdl_global_init() {
    // 网络协议注册（http/https/rtsp/rtmp/udp 等），必须在任何网络打开之前调用
    if (!g_network_initialized) {
        avformat_network_init();
        g_network_initialized = true;
    }
    if (g_sdl_initialized) return true;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not initialize SDL: %s\n", SDL_GetError());
        return false;
    }
    g_sdl_initialized = true;
    return true;
}

double get_total_seconds(VideoState* is) {
    if (!is || !is->formatContext) return 0.0;
    if (is->formatContext->duration > 0 && is->formatContext->duration != AV_NOPTS_VALUE) {
        return (double)is->formatContext->duration / (double)AV_TIME_BASE;
    }
    if (is->videoStream && is->videoStream->duration > 0 && is->videoStream->duration != AV_NOPTS_VALUE) {
        return is->videoStream->duration * av_q2d(is->videoStream->time_base);
    }
    if (is->audioStream && is->audioStream->duration > 0 && is->audioStream->duration != AV_NOPTS_VALUE) {
        return is->audioStream->duration * av_q2d(is->audioStream->time_base);
    }
    return 0.0;
}

void call_audio_back(void* userdata, Uint8* stream, int len) {
    VideoState* is = static_cast<VideoState*>(userdata);
    if (!is) 
    {
        memset(stream, 0, len);
        return;
    }

    memset(stream, 0, len);
    std::lock_guard<std::mutex> lk(is->mutex);

    int filled = 0;
    int bytes_per_second = is->sample_rate * is->channels * av_get_bytes_per_sample(is->sample_fmt);

    auto advance_audio_clock = [&](int bytes) {
        if (bytes_per_second > 0) {
            is->audioClock += (double)bytes / (double)bytes_per_second;
        }
    };

    if (is->audio_buffer.buffer && is->audio_buffer.size > 0) {
        int avail = is->audio_buffer.size - is->audio_buffer.read_offset;
        if (avail > 0) {
            int to_copy = std::min(len, avail);
            SDL_MixAudioFormat(stream, is->audio_buffer.buffer + is->audio_buffer.read_offset,
                               AUDIO_S16SYS, to_copy, is->volume);
            is->audio_buffer.read_offset += to_copy;
            filled += to_copy;
            advance_audio_clock(to_copy);
            if (is->audio_buffer.read_offset >= is->audio_buffer.size) {
                av_free(is->audio_buffer.buffer);
                is->audio_buffer.buffer = nullptr;
                is->audio_buffer.size = 0;
                is->audio_buffer.read_offset = 0;
            }
        }
    }

    while (filled < len) {
        PCMItem item = is->pcmQueue->pop_for(0);
        if (item.data.empty()) break;

        if (item.has_pts && filled == 0) {
            is->audioClock = item.pts_seconds;
        }

        int data_size = (int)item.data.size();
        int remain = len - filled;

        if (data_size <= remain) {
            SDL_MixAudioFormat(stream + filled, item.data.data(),
                               AUDIO_S16SYS, data_size, is->volume);
            filled += data_size;
            advance_audio_clock(data_size);
        } else {
            SDL_MixAudioFormat(stream + filled, item.data.data(),
                               AUDIO_S16SYS, remain, is->volume);
            int leftover = data_size - remain;
            uint8_t* buf = (uint8_t*)av_malloc(leftover);
            if (buf) {
                memset(buf, 0, leftover);
                memcpy(buf, item.data.data() + remain, leftover);
                if (is->audio_buffer.buffer) av_free(is->audio_buffer.buffer);
                is->audio_buffer.buffer = buf;
                is->audio_buffer.size = leftover;
                is->audio_buffer.read_offset = 0;
            }
            filled = len;
            advance_audio_clock(remain);
        }
    }
}




void render_frame(VideoState* is) {
    if (!is || is->quit) return;
    if (!is->video_frameQueue || !is->renderer) return;
    if (!is->videoStream || !is->formatContext) return;

    const double SYNC_THRESHOLD = 0.03;
    const double DROP_THRESHOLD = -0.12;

    FrameItem item = is->video_frameQueue->pop_for(10);
    if (!item.frame) 
    {
        return;
    }

    AVFrame* frame = item.frame;

    if (frame->width <= 0 || frame->height <= 0) {
        av_frame_free(&frame);
        return;
    }

    double frame_pts_seconds = 0.0;
    if (!frame->data[0]) {
        av_frame_free(&frame);
        return;
    }
    if (frame->pts != AV_NOPTS_VALUE) {
        frame_pts_seconds = frame->pts * av_q2d(is->videoStream->time_base);
    } else if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
        frame_pts_seconds = frame->best_effort_timestamp * av_q2d(is->videoStream->time_base);
    }

    // 有音频时保持音画同步；无音频时直接按视频解码结果渲染，不等待音频时钟。
    if (is->audio_enabled) {
        double audio_clock;
        {
            std::lock_guard<std::mutex> lk(is->mutex);
            audio_clock = is->audioClock;
        }

        int dropped_frames = 0;
        double diff = frame_pts_seconds - audio_clock;
        while (diff < DROP_THRESHOLD && dropped_frames < 8) {
            av_frame_free(&frame);
            FrameItem next_item = is->video_frameQueue->pop_for(0);
            if (!next_item.frame) return;
            frame = next_item.frame;
            if (!frame->data[0]) {
                av_frame_free(&frame);
                return;
            }
            if (frame->pts != AV_NOPTS_VALUE) {
                frame_pts_seconds = frame->pts * av_q2d(is->videoStream->time_base);
            } else if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                frame_pts_seconds = frame->best_effort_timestamp * av_q2d(is->videoStream->time_base);
            }
            diff = frame_pts_seconds - audio_clock;
            ++dropped_frames;
        }

        if (diff < DROP_THRESHOLD) {
            av_frame_free(&frame);
            return;
        }
        if (diff > SYNC_THRESHOLD) {
            SDL_Delay((Uint32)(std::min(diff, 0.1) * 1000));
        }
    }

    AVFrame* src_frame = frame;

    // ---- Step 3: 更新 sws_ctx（源格式 → YUV420P） ----
    AVPixelFormat src_fmt = (AVPixelFormat)src_frame->format;
    bool video_size_changed = is->last_width != src_frame->width ||
                              is->last_height != src_frame->height;
    if (!is->sws_ctx || video_size_changed || is->last_pix_fmt != src_fmt) {
        if (is->sws_ctx) sws_freeContext(is->sws_ctx);
        is->sws_ctx = sws_getContext(src_frame->width, src_frame->height, src_fmt,
                                     src_frame->width, src_frame->height, AV_PIX_FMT_YUV420P,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
        is->last_width  = src_frame->width;
        is->last_height = src_frame->height;
        is->last_pix_fmt = src_fmt;

        // 重新分配 YUV420P 目标帧
        if (is->yuv_frame) av_frame_free(&is->yuv_frame);
        is->yuv_frame = av_frame_alloc();
        if (is->yuv_frame) {
            is->yuv_frame->format = AV_PIX_FMT_YUV420P;
            is->yuv_frame->width  = src_frame->width;
            is->yuv_frame->height = src_frame->height;
            int buf_ret = av_frame_get_buffer(is->yuv_frame, 0);
            if (buf_ret < 0) {
                av_frame_free(&is->yuv_frame);
                is->yuv_frame = nullptr;
            }
        }
    }

    // ---- Step 4: sws_scale 将源帧转为 YUV420P ----
    if (!is->sws_ctx || !is->yuv_frame || !is->yuv_frame->data[0]) {
        av_frame_free(&frame);
        return;
    }
    sws_scale(is->sws_ctx,
              src_frame->data, src_frame->linesize,
              0, src_frame->height,
              is->yuv_frame->data, is->yuv_frame->linesize);

    // ---- Step 5: 更新 SDL 纹理并渲染 ----
    if (!is->texture || video_size_changed) {
        if (is->texture) {
            SDL_DestroyTexture(is->texture);
            is->texture = nullptr;
        }
        if (!is->renderer) {
            av_frame_free(&frame);
            return;
        }
        is->texture = SDL_CreateTexture(is->renderer, SDL_PIXELFORMAT_IYUV,
                                        SDL_TEXTUREACCESS_STREAMING,
                                        src_frame->width, src_frame->height);
    }

    if (is->texture) {
        SDL_UpdateYUVTexture(is->texture, nullptr,
                             is->yuv_frame->data[0], is->yuv_frame->linesize[0],
                             is->yuv_frame->data[1], is->yuv_frame->linesize[1],
                             is->yuv_frame->data[2], is->yuv_frame->linesize[2]);
    }

    if (!is->renderer) {
        av_frame_free(&frame);
        return;
    }

    SDL_SetRenderDrawColor(is->renderer, 0, 0, 0, 255);
    SDL_RenderClear(is->renderer);

    int w = 0, h = 0;
    SDL_GetRendererOutputSize(is->renderer, &w, &h);
    if (w > 0 && h > 0 && is->last_width > 0 && is->last_height > 0) {
        SDL_Rect rect;
        AVRational sample_aspect_ratio = {1, 1};
        {
            std::lock_guard<std::mutex> lk(is->format_mutex);
            if (is->formatContext && is->videoStream) {
                sample_aspect_ratio = av_guess_sample_aspect_ratio(
                    is->formatContext, is->videoStream, src_frame);
            } else if (src_frame->sample_aspect_ratio.num > 0 &&
                       src_frame->sample_aspect_ratio.den > 0) {
                sample_aspect_ratio = src_frame->sample_aspect_ratio;
            }
        }
        double pixel_aspect = av_q2d(sample_aspect_ratio);
        if (pixel_aspect <= 0.0 || !std::isfinite(pixel_aspect)) {
            pixel_aspect = 1.0;
        }
        double win_ratio = (double)w / h;
        double vid_ratio = ((double)is->last_width / is->last_height) * pixel_aspect;
        if (win_ratio > vid_ratio) {
            rect.h = h;
            rect.w = (int)(h * vid_ratio);
            rect.x = (w - rect.w) / 2;
            rect.y = 0;
        } else {
            rect.w = w;
            rect.h = (int)(w / vid_ratio);
            rect.x = 0;
            rect.y = (h - rect.h) / 2;
        }
        SDL_RenderCopy(is->renderer, is->texture, nullptr, &rect);
    } else {
        SDL_RenderCopy(is->renderer, is->texture, nullptr, nullptr);
    }

    SDL_RenderPresent(is->renderer);

    // ---- 清理 ----
    av_frame_free(&frame);
    is->videoClock = frame_pts_seconds;
}

void seek(VideoState* is, double pos_seconds) {
    if (!is) return;

    pos_seconds = std::max(0.0, pos_seconds);
    bool was_paused = is->pause;

    std::lock_guard<std::mutex> seek_lk(is->seek_mutex);
    is->seek_serial.fetch_add(1);
    is->pause = true;

    // 等待当前 demux/decode 操作结束后再 flush，避免旧 GOP 与新位置交错。
    {
        std::lock_guard<std::mutex> decode_lk(is->decode_mutex);
        if (is->videoCodecContext) avcodec_flush_buffers(is->videoCodecContext);
        if (is->audioCodecContext) avcodec_flush_buffers(is->audioCodecContext);
    }

    int seek_stream = -1;
    int64_t seek_target = (int64_t)(pos_seconds * AV_TIME_BASE);
    if (is->videoStream && is->VideoStreamIndex >= 0) {
        // H.264 必须从视频关键帧开始，不能让 av_seek_frame 选择音频流的索引点。
        seek_stream = is->VideoStreamIndex;
        seek_target = av_rescale_q(
            (int64_t)(pos_seconds * AV_TIME_BASE),
            AV_TIME_BASE_Q,
            is->videoStream->time_base);
    }
    int seek_ret = 0;
    {
        std::lock_guard<std::mutex> lk(is->format_mutex);
        seek_ret = av_seek_frame(is->formatContext, seek_stream, seek_target,
                                 AVSEEK_FLAG_BACKWARD);
    }

    if (seek_ret < 0) {
        char errbuf[128];       
        av_strerror(seek_ret, errbuf, sizeof(errbuf));
        av_log(NULL, AV_LOG_ERROR, "Seek failed: %s\n", errbuf);
        is->pause = was_paused;
        return;
    }

    is->packetQueue->flush();
    is->audioPacketQueue->flush();
    is->video_frameQueue->flush();
    is->pcmQueue->flush();
    is->packetQueue->reset_abort();
    is->audioPacketQueue->reset_abort();
    is->video_frameQueue->reset_abort();
    is->pcmQueue->reset_abort();

    {
        std::lock_guard<std::mutex> lk(is->mutex);
        if (is->audio_buffer.buffer) {
            av_free(is->audio_buffer.buffer);
            is->audio_buffer.buffer = nullptr;
        }
        is->audio_buffer.size = 0;
        is->audio_buffer.read_offset = 0;
    }

    is->audioClock = pos_seconds;
    is->videoClock = pos_seconds;
    is->eof = false;
    is->pause = was_paused;
}

// 判断是否为网络 URL（本地文件路径不含 scheme）
static bool is_network_url(const char* url) {
    if (!url) return false;
    if (strstr(url, "://")) return true;
    static const char* schemes[] = {
        "http:", "https:", "rtsp:", "rtmp:", "udp:", "rtp:", "srt:", "mms:", "mmsh:", "ftp:"
    };
    for (const char* s : schemes) {
        if (av_stristart(url, s, nullptr)) return true;
    }
    return false;
}

// FFmpeg 中断回调：abort_request 置位时让阻塞的网络 IO / 打开流程尽快返回
static int interrupt_cb(void* opaque) {
    VideoState* is = static_cast<VideoState*>(opaque);
    if (!is) return 0;
    return is->abort_request ? 1 : 0;
}

bool player_open(VideoState* is, const char* url) {
    if (!is || !url) return false;
    if (!is->window || !is->renderer) {
        av_log(NULL, AV_LOG_ERROR, "player_open: window/renderer must be set before calling\n");
        return false;
    }

    is->abort_request = false;
    is->eof = false;
    is->audio_enabled = false;

    bool is_net = is_network_url(url);
    is->is_network_source = is_net;
    is->network_reconnecting = false;
    is->network_failed = false;
    is->network_error_code = 0;

    // ---- 打开输入（支持本地文件与网络流） ----
    // 先分配上下文，以便挂上中断回调（网络异常时保证打开流程可被打断）
    AVFormatContext* ctx = avformat_alloc_context();
    if (!ctx) return false;
    ctx->interrupt_callback.callback = interrupt_cb;
    ctx->interrupt_callback.opaque = is;
    is->formatContext = ctx;

    AVDictionary* opts = nullptr;
    if (is_net) {
        // 读写超时（µs）：避免网络不通时打开流程无限阻塞
        av_dict_set_int(&opts, "rw_timeout", 10 * AV_TIME_BASE, 0);
        av_dict_set_int(&opts, "timeout",    10 * AV_TIME_BASE, 0);
        av_dict_set_int(&opts, "stimeout",   10 * AV_TIME_BASE, 0);   // RTSP 专用
        // 断线自动重连
        av_dict_set(&opts, "reconnect", "1", 0);
        av_dict_set(&opts, "reconnect_streamed", "1", 0);
        av_dict_set(&opts, "reconnect_delay_max", "5", 0);
        // 伪装 User-Agent，避免部分服务器拒绝 FFmpeg 默认 UA
        av_dict_set(&opts, "user_agent", "Player/2.0 (FFmpeg)", 0);

        // RTSP 默认走 TCP 传输，穿透 NAT / 弱网环境更稳定
        if (av_stristart(url, "rtsp:", nullptr)) {
            av_dict_set(&opts, "rtsp_transport", "tcp", 0);
        }

        // 限制探测数据量，加快网络流打开速度
        av_opt_set_int(ctx, "probesize", 5 * 1024 * 1024, 0);
        av_opt_set_int(ctx, "analyzeduration", 5 * AV_TIME_BASE, 0);

        // 直播类协议关闭缓冲，降低延迟
        static const char* live_protos[] = { "udp:", "rtp:", "rtsp:", "rtmp:", "srt:" };
        for (const char* p : live_protos) {
            if (av_stristart(url, p, nullptr)) {
                ctx->flags |= AVFMT_FLAG_NOBUFFER;
                break;
            }
        }
    }

    int ret = avformat_open_input(&is->formatContext, url, nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0 || !is->formatContext) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        av_log(NULL, AV_LOG_ERROR, "Could not open source %s: %s\n", url, errbuf);
        is->formatContext = nullptr;
        return false;
    }

    // 打开期间被中止（切歌/关闭）
    if (is->abort_request) {
        avformat_close_input(&is->formatContext);
        return false;
    }

    // 获取流信息
    ret = avformat_find_stream_info(is->formatContext, nullptr);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        av_log(NULL, AV_LOG_WARNING, "Could not find stream info: %s\n", errbuf);
    }

    int v_stream_index = av_find_best_stream(is->formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (v_stream_index < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not find video stream\n");
        avformat_close_input(&is->formatContext);
        return false;
    }
    is->VideoStreamIndex = v_stream_index;

    int a_stream_index = av_find_best_stream(is->formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    is->AudioStreamIndex = a_stream_index;

    const AVCodec* vcodec = avcodec_find_decoder(is->formatContext->streams[v_stream_index]->codecpar->codec_id);
    AVCodecContext* vcodecContext = avcodec_alloc_context3(vcodec);
    if (!vcodec || !vcodecContext) 
    {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate video codec\n");
        avformat_close_input(&is->formatContext);
        return false;
    }
    avcodec_parameters_to_context(vcodecContext, is->formatContext->streams[v_stream_index]->codecpar);

    ret = avcodec_open2(vcodecContext, vcodec, nullptr);
    if (ret < 0) 
    {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        av_log(NULL, AV_LOG_ERROR, "Could not open video codec: %s\n", errbuf);
        avcodec_free_context(&vcodecContext);
        avformat_close_input(&is->formatContext);
        return false;
    }
    is->videoCodecContext = vcodecContext;
    is->videoStream = is->formatContext->streams[v_stream_index];

    if (a_stream_index >= 0) {
        const AVCodec* acodec = avcodec_find_decoder(is->formatContext->streams[a_stream_index]->codecpar->codec_id);
        AVCodecContext* acodecContext = avcodec_alloc_context3(acodec);
        if (acodec && acodecContext) {
            avcodec_parameters_to_context(acodecContext, is->formatContext->streams[a_stream_index]->codecpar);
            ret = avcodec_open2(acodecContext, acodec, nullptr);
            if (ret < 0) {
                char errbuf[128];
                av_strerror(ret, errbuf, sizeof(errbuf));
                av_log(NULL, AV_LOG_ERROR, "Could not open audio codec: %s\n", errbuf);
                avcodec_free_context(&acodecContext);
                acodecContext = nullptr;
            } else {
                is->audioCodecContext = acodecContext;
                is->audioStream = is->formatContext->streams[a_stream_index];
                if (acodecContext->sample_rate > 0) {
                    is->sample_rate = acodecContext->sample_rate;
                }
                if (acodecContext->channels > 0) {
                    is->channels = acodecContext->channels;
                }
                if (acodecContext->channel_layout) {
                    is->channel_layout = acodecContext->channel_layout;
                } else if (is->channels > 0) {
                    is->channel_layout = av_get_default_channel_layout(is->channels);
                }
                
                SDL_AudioSpec wanted_spec, spec;
                wanted_spec.freq = is->sample_rate;
                wanted_spec.format = AUDIO_S16SYS;
                wanted_spec.channels = is->channels;
                wanted_spec.silence = 0;
                wanted_spec.samples = FFMAX(512, 2048);
                wanted_spec.callback = call_audio_back;
                wanted_spec.userdata = is;

                is->audio_dev = SDL_OpenAudioDevice(nullptr, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_ANY_CHANGE);
                if (is->audio_dev == 0) {
                    av_log(NULL, AV_LOG_ERROR, "SDL_OpenAudioDevice error: %s\n", SDL_GetError());
                } else {
                    is->sample_rate = spec.freq;
                    is->channels = spec.channels;
                    is->sample_fmt = AV_SAMPLE_FMT_S16;
                    is->audio_enabled = true;
                    SDL_PauseAudioDevice(is->audio_dev, 0); // start playing audio
                }
            }
        }
    }

    is->packetQueue.reset(new PacketQueue(500, 8 * 1024 * 1024));
    is->audioPacketQueue.reset(new PacketQueue(200, 2 * 1024 * 1024));
    is->video_frameQueue.reset(new FrameQueue(32));
    is->pcmQueue.reset(new PCMQueue());

    // 启动工作线程
    is->quit = false;
    is->demuxThread = SDL_CreateThread(Demux_Thread, "demux", is);
    is->videoThread = SDL_CreateThread(Decoder_video_thread, "video", is);
    if (is->audio_enabled) {
        is->audioThread = SDL_CreateThread(Decoder_audio_thread, "audio", is);
    }

    return true;
}

void player_close(VideoState* is) {
    if (!is) return;

    // 0. 置中止标志：让 FFmpeg 中断回调立刻生效，阻塞在 av_read_frame 的线程会尽快返回
    is->abort_request = true;

    // 1. 通知所有线程退出
    is->quit = true;
    if (is->packetQueue)      is->packetQueue->abort();
    if (is->audioPacketQueue) is->audioPacketQueue->abort();
    if (is->video_frameQueue) is->video_frameQueue->abort();
    if (is->pcmQueue)         is->pcmQueue->abort();

    // 2. 等待线程结束
    int thread_ret = 0;
    if (is->demuxThread) {
        SDL_WaitThread(is->demuxThread, &thread_ret);
        is->demuxThread = nullptr;
    }
    if (is->videoThread) {
        SDL_WaitThread(is->videoThread, &thread_ret);
        is->videoThread = nullptr;
    }
    if (is->audioThread) {
        SDL_WaitThread(is->audioThread, &thread_ret);
        is->audioThread = nullptr;
    }

    // 3. 关闭音频设备
    if (is->audio_dev != 0) {
        SDL_CloseAudioDevice(is->audio_dev);
        is->audio_dev = 0;
    }

    // 4. 释放编解码器
    if (is->videoCodecContext) {
        avcodec_free_context(&is->videoCodecContext);
        is->videoCodecContext = nullptr;
    }
    if (is->audioCodecContext) {
        avcodec_free_context(&is->audioCodecContext);
        is->audioCodecContext = nullptr;
    }

    // 5. 关闭格式上下文
    if (is->formatContext) {
        avformat_close_input(&is->formatContext);
        is->formatContext = nullptr;
    }

    // 6. 销毁 SDL 渲染资源（Qt 集成模式下由 player_close 统一管理）
    if (is->texture) {
        SDL_DestroyTexture(is->texture);
        is->texture = nullptr;
    }
    if (is->renderer) {
        SDL_DestroyRenderer(is->renderer);
        is->renderer = nullptr;
    }
    if (is->window) {
        SDL_DestroyWindow(is->window);
        is->window = nullptr;
    }

    // 7. 释放转换上下文
    if (is->sws_ctx) {
        sws_freeContext(is->sws_ctx);
        is->sws_ctx = nullptr;
    }
    if (is->swr_ctx) {
        swr_free(&is->swr_ctx);
        is->swr_ctx = nullptr;
    }

    // 8. 清理音频缓冲
    if (is->audio_buffer.buffer) {
        av_free(is->audio_buffer.buffer);
        is->audio_buffer.buffer = nullptr;
    }
    is->audio_buffer.size = 0;
    is->audio_buffer.read_offset = 0;

    // 9. 释放 YUV 转换帧
    if (is->yuv_frame) {
        av_frame_free(&is->yuv_frame);
        is->yuv_frame = nullptr;
    }

    // 重置状态字段
    is->last_width = 0;
    is->last_height = 0;
    is->last_pix_fmt = AV_PIX_FMT_NONE;
    is->audioClock = 0.0;
    is->videoClock = 0.0;
    is->audio_enabled = false;
    is->VideoStreamIndex = -1;
    is->AudioStreamIndex = -1;
    is->videoStream = nullptr;
    is->audioStream = nullptr;
}

void set_volume(VideoState* is, int volume_percent) {
    if (!is) return;
    volume_percent = std::max(0, std::min(volume_percent, 100));
    std::lock_guard<std::mutex> lk(is->mutex);
    is->volume = volume_percent * SDL_MIX_MAXVOLUME / 100;
}