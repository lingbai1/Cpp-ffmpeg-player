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
}
#include<libavutil/pixdesc.h>

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
#include"VideoState.h"
#include"PacketQueue.h"
#include"FrameQueue.h"
#include"PCMQueue.h"
#include"Demux.h"
#include"Decoder_video.h"
#include"Decoder_audio.h"
#include"AudioBuffer.h"
#include"FrameItem.h"

static enum AVPixelFormat choose_software_pix_fmt(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) 
{
    if (!pix_fmts)
    return AV_PIX_FMT_NONE;
    const enum AVPixelFormat* p = pix_fmts;
    while (*p != AV_PIX_FMT_NONE) 
    {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*p);
        if (desc) {
            if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) 
            {
                return *p;
            }
        } else {
            return *p;
        }
        ++p;
    }
    return pix_fmts[0];
}


class SDLGuard {
public:
    SDLGuard() : initialized(false) {}

    bool init(Uint32 flags) {
        if (SDL_Init(flags) != 0) {
            return false;
        }
        initialized = true;
        return true;
    }

    ~SDLGuard() {
        if (initialized) {
            SDL_Quit();
        }
    }

private:
    bool initialized;
};

static double g_drag_ratio = -1.0;

static double get_total_seconds(VideoState* is) {
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

static bool get_seek_ratio_from_mouse(VideoState* is, int x, int y, double& ratio) {
    if (!is || !is->renderer) return false;
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(is->renderer, &w, &h);
    if (w <= 0 || h <= 0) return false;
    const int bar_height = 8;
    if (y < h - bar_height || y > h) return false;
    ratio = std::min(std::max(x / (double)w, 0.0), 1.0);
    return true;
}

static bool is_on_progress_bar(VideoState* is, int y) {
    if (!is || !is->renderer) return false;
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(is->renderer, &w, &h);
    if (w <= 0 || h <= 0) return false;
    const int bar_height = 8;
    return y >= h - bar_height && y <= h;
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

    if (is->audio_buffer.buffer && is->audio_buffer.size > 0) {
        int avail = is->audio_buffer.size - is->audio_buffer.read_offset;
        if (avail > 0) {
            int to_copy = std::min(len, avail);
            memcpy(stream, is->audio_buffer.buffer + is->audio_buffer.read_offset, to_copy);
            is->audio_buffer.read_offset += to_copy;
            filled += to_copy;
            if (is->audio_buffer.read_offset >= is->audio_buffer.size) {
                av_free(is->audio_buffer.buffer);
                is->audio_buffer.buffer = nullptr;
                is->audio_buffer.size = 0;
                is->audio_buffer.read_offset = 0;
            }
        }
    }

    while (filled < len) {
        std::vector<uint8_t> chunk = is->pcmQueue->pop_for(0);
        if (chunk.empty()) break;

        int data_size = (int)chunk.size();
        int remain = len - filled;

        if (data_size <= remain) {
            memcpy(stream + filled, chunk.data(), data_size);
            filled += data_size;
        } else {
            memcpy(stream + filled, chunk.data(), remain);
            int leftover = data_size - remain;
            uint8_t* buf = (uint8_t*)av_malloc(leftover);
            if (buf) {
                memset(buf, 0, leftover);
                memcpy(buf, chunk.data() + remain, leftover);
                if (is->audio_buffer.buffer) av_free(is->audio_buffer.buffer);
                is->audio_buffer.buffer = buf;
                is->audio_buffer.size = leftover;
                is->audio_buffer.read_offset = 0;
            }
            filled = len;
        }
    }

  
    int bytes_per_second = is->sample_rate * is->channels * av_get_bytes_per_sample(is->sample_fmt);
    if (bytes_per_second > 0) {
        is->audioClock += (double)filled / (double)bytes_per_second;
    }
}




void render_frame(VideoState* is) {
    const double SYNC_THRESHOLD = 0.03;
    const double DROP_THRESHOLD = -0.12;

    FrameItem item = is->video_frameQueue->pop_for(10);
    if (!item.frame) 
    {
        return;
    }

    AVFrame* frame = item.frame;

    if (frame->width <= 0 || frame->height <= 0 || !frame->data[0]) {
        av_frame_free(&frame);
        return;
    }

    double frame_pts_seconds = 0.0;
    if (frame->pts != AV_NOPTS_VALUE) {
        frame_pts_seconds = frame->pts * av_q2d(is->videoStream->time_base);
    } else if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
        frame_pts_seconds = frame->best_effort_timestamp * av_q2d(is->videoStream->time_base);
    }

    double diff = frame_pts_seconds - is->audioClock;

    if (diff < DROP_THRESHOLD) {
        // std::cout << "Dropping frame, diff: " << diff << std::endl;
        av_frame_free(&frame);
        return;
    }

    if (diff > SYNC_THRESHOLD) {
        double delay = std::min(diff, 0.1);
        std::cout << "Delaying for " << delay << "s, diff: " << diff << std::endl;
        SDL_Delay((Uint32)(delay * 1000));
    }

    if (!is->sws_ctx||is->last_width != frame->width || is->last_height != frame->height || is->last_pix_fmt != (AVPixelFormat)frame->format) {
        is->sws_ctx = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format,
                                     frame->width, frame->height, AV_PIX_FMT_YUV420P,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
        is->last_width = frame->width;
        is->last_height = frame->height;
        is->last_pix_fmt = (AVPixelFormat)frame->format;
    }
    
    
    if (!is->texture||is->last_width != frame->width || is->last_height != frame->height) {
        if(is->texture) 
        {
            SDL_DestroyTexture(is->texture);
            is->texture = nullptr;
        }
        is->texture = SDL_CreateTexture(is->renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, frame->width, frame->height);                                                                                                                          
    }

    SDL_UpdateYUVTexture(is->texture, nullptr,
                         frame->data[0], frame->linesize[0],
                         frame->data[1], frame->linesize[1],
                         frame->data[2], frame->linesize[2]);

    SDL_SetRenderDrawColor(is->renderer, 0, 0, 0, 255);
    SDL_RenderClear(is->renderer);

    int w = 0, h = 0;
    SDL_GetRendererOutputSize(is->renderer, &w, &h);
    if (w > 0 && h > 0 && is->last_width > 0 && is->last_height > 0) {
        SDL_Rect rect;
        double win_ratio = (double)w / h;
        double vid_ratio = (double)is->last_width / is->last_height;
        if (win_ratio > vid_ratio) {
            rect.h = h;
            rect.w = h * vid_ratio;
            rect.x = (w - rect.w) / 2;
            rect.y = 0;
        } else {
            rect.w = w;
            rect.h = w / vid_ratio;
            rect.x = 0;
            rect.y = (h - rect.h) / 2;
        }
        SDL_RenderCopy(is->renderer, is->texture, nullptr, &rect);
    } else {
        SDL_RenderCopy(is->renderer, is->texture, nullptr, nullptr);
    }

    
    double total_seconds = get_total_seconds(is);

    double current_seconds = std::max(0.0, is->audioClock);
    double ratio = total_seconds > 0.0 ? std::min(current_seconds / total_seconds, 1.0) : 0.0;
    if (g_drag_ratio >= 0.0) {
        ratio = g_drag_ratio;
    }

    if (w > 0 && h > 0) {
        int bar_height = 8;
        SDL_Rect bg = {0, h - bar_height, w, bar_height};
        SDL_Rect fg = {0, h - bar_height, (int)(w * ratio), bar_height};

        SDL_SetRenderDrawColor(is->renderer, 60, 60, 60, 255);
        SDL_RenderFillRect(is->renderer, &bg);      
        SDL_SetRenderDrawColor(is->renderer, 0, 160, 255, 255);
        SDL_RenderFillRect(is->renderer, &fg);
    }
    SDL_RenderPresent(is->renderer);

    av_frame_free(&frame);
    is->videoClock = frame_pts_seconds;
}

void seek(VideoState* is, double pos_seconds) {
    if (!is) return;

    bool was_paused = is->pause;
    is->pause = true;

    int64_t seek_target = (int64_t)(pos_seconds * AV_TIME_BASE);

    int seek_ret = 0;
    {
        std::lock_guard<std::mutex> lk(is->format_mutex);
        seek_ret = av_seek_frame(is->formatContext, -1, seek_target, AVSEEK_FLAG_BACKWARD);
    }

    if (seek_ret < 0) {
        char errbuf[128];       
        av_strerror(seek_ret, errbuf, sizeof(errbuf));
        av_log(NULL, AV_LOG_ERROR, "Seek failed: %s\n", errbuf);
        is->pause = was_paused;
        return;
    }

    if (is->videoCodecContext) avcodec_flush_buffers(is->videoCodecContext);
    if (is->audioCodecContext) avcodec_flush_buffers(is->audioCodecContext);

    is->packetQueue->flush();
    is->audioPacketQueue->flush();
    is->video_frameQueue->flush();
    is->pcmQueue->flush();

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
    is->pause = was_paused;
}

int main(int argc, char* argv[])
 {
    if (argc < 2) {
        av_log(NULL, AV_LOG_ERROR, "Please provide a source file\n");
        return -1;
    }

    const char* src = argv[1];
    if (!src) {
        av_log(NULL, AV_LOG_ERROR, "Please provide a source file\n");
        return -1;
    }
    SDLGuard sdl_guard;
    if (!sdl_guard.init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        av_log(NULL, AV_LOG_ERROR, "Could not initialize SDL: %s\n", SDL_GetError());
        return -1;
    }

    std::unique_ptr<VideoState> is(new VideoState());

    is->window = SDL_CreateWindow("Player", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 640, 480, SDL_WINDOW_SHOWN|SDL_WINDOW_RESIZABLE);
    if (!is->window) {
        av_log(NULL, AV_LOG_ERROR, "Could not create window: %s\n", SDL_GetError());
        return -1;
    }

    is->renderer = SDL_CreateRenderer(is->window, -1, 0);
    if (!is->renderer) {
        av_log(NULL, AV_LOG_ERROR, "Could not create renderer: %s\n", SDL_GetError());
        return -1;
    }

    int ret = avformat_open_input(&is->formatContext, src, nullptr, nullptr);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        av_log(NULL, AV_LOG_ERROR, "Could not open source file %s: %s\n", src, errbuf);
        return -1;
    }

    int v_stream_index = av_find_best_stream(is->formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (v_stream_index < 0) {
        char errbuf[128];
        av_strerror(v_stream_index, errbuf, sizeof(errbuf));
        av_log(NULL, AV_LOG_ERROR, "Could not find video stream: %s\n", errbuf);
        return -1;
    }
    is->VideoStreamIndex = v_stream_index;

    int a_stream_index = av_find_best_stream(is->formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    is->AudioStreamIndex = a_stream_index;

    const AVCodec* vcodec = avcodec_find_decoder(is->formatContext->streams[v_stream_index]->codecpar->codec_id);
    AVCodecContext* vcodecContext = avcodec_alloc_context3(vcodec);
    if (!vcodec || !vcodecContext) 
    {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate video codec\n");
        return -1;
    }
    avcodec_parameters_to_context(vcodecContext, is->formatContext->streams[v_stream_index]->codecpar);
    vcodecContext->get_format = choose_software_pix_fmt;
    ret = avcodec_open2(vcodecContext, vcodec, nullptr);
    if (ret < 0) 
    {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        av_log(NULL, AV_LOG_ERROR, "Could not open video codec: %s\n", errbuf);
        avcodec_free_context(&vcodecContext);
        return -1;
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
                wanted_spec.userdata = is.get();

                is->audio_dev = SDL_OpenAudioDevice(nullptr, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_ANY_CHANGE);
                if (is->audio_dev == 0) {
                    av_log(NULL, AV_LOG_ERROR, "SDL_OpenAudioDevice error: %s\n", SDL_GetError());
                } else {
                    is->sample_rate = spec.freq;
                    is->channels = spec.channels;
                    is->sample_fmt = AV_SAMPLE_FMT_S16;
                    SDL_PauseAudioDevice(is->audio_dev, 0); // start playing audio
                }
            }
        }
    }

    is->packetQueue.reset(new PacketQueue(500, 8 * 1024 * 1024));
    is->audioPacketQueue.reset(new PacketQueue(200, 2 * 1024 * 1024));
    is->video_frameQueue.reset(new FrameQueue(32));
    is->pcmQueue.reset(new PCMQueue());


    is->demuxThread = SDL_CreateThread(Demux_Thread, "demux", is.get());
    is->videoThread = SDL_CreateThread(Decoder_video_thread, "video", is.get());
    is->audioThread = SDL_CreateThread(Decoder_audio_thread, "audio", is.get());

    SDL_Event e;
    bool dragging = false;          // 是否正在拖动进度条
    double drag_ratio = 0.0;        // 当前拖动位置对应的播放比例
    while (!is->quit) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) 
            {
                is->quit = true;
            } 
            else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT)
            {
                double ratio = 0.0;
                if (get_seek_ratio_from_mouse(is.get(), e.button.x, e.button.y, ratio)) {
                    dragging = true;
                    drag_ratio = ratio;
                    g_drag_ratio = ratio;
                }
            }
            else if (e.type == SDL_MOUSEMOTION && dragging)
            {
                double ratio = 0.0;
                if (get_seek_ratio_from_mouse(is.get(), e.motion.x, e.motion.y, ratio)) {
                    drag_ratio = ratio;
                    g_drag_ratio = ratio;
                }
            }
            else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT)
            {
                if (dragging) {
                    dragging = false;
                    g_drag_ratio = -1.0;
                    double total_seconds = get_total_seconds(is.get());
                    if (total_seconds > 0.0) {
                        seek(is.get(), drag_ratio * total_seconds);
                    }
                }
            }
            else if (e.type == SDL_KEYDOWN)
             {
                switch (e.key.keysym.sym) 
                {
                    case SDLK_SPACE:
                        is->pause = !is->pause;
                        if (is->audio_dev != 0)
                        {
                            SDL_PauseAudioDevice(is->audio_dev, is->pause ? 1 : 0);
                        }
                        break;
                    case SDLK_RIGHT:
                        seek(is.get(), is->audioClock + 10);
                        break;
                    case SDLK_LEFT:
                        seek(is.get(), std::max(0.0, is->audioClock - 10));
                        break;
                }
            }
        }

        if (!is->pause) {
            render_frame(is.get());
        }

        SDL_Delay(10);
    }

    is->quit = true;
    is->packetQueue->abort();
    is->audioPacketQueue->abort();
    is->video_frameQueue->abort();
    is->pcmQueue->abort();

    int thread_ret = 0;
    if (is->demuxThread) 
    {
        SDL_WaitThread(is->demuxThread, &thread_ret);
    }
    if (is->videoThread) 
    {
        SDL_WaitThread(is->videoThread, &thread_ret);
    }
    if (is->audioThread) 
    {
        SDL_WaitThread(is->audioThread, &thread_ret);
    }

    return 0;
}