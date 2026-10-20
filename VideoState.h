#pragma once
extern "C" {
    #include<libavformat/avformat.h>
    #include<libavcodec/avcodec.h>
    #include<libavutil/avutil.h>
    #include<libavutil/pixdesc.h>
    #include<SDL2/SDL.h>
    #include<libswscale/swscale.h>
    #include<libswresample/swresample.h>
}
#include<memory>
#include<mutex>
#include<atomic>
#include"PacketQueue.h"
#include"FrameQueue.h"
#include"PCMQueue.h"
#include"AudioBuffer.h"

class VideoState {
    public:
    AVFormatContext* formatContext;
    AVCodecContext* audioCodecContext;
    AVCodecContext* videoCodecContext;
    int VideoStreamIndex;
    int AudioStreamIndex;
    AVStream* videoStream;
    AVStream* audioStream;

    std::unique_ptr<PacketQueue> packetQueue;
    std::unique_ptr<PacketQueue> audioPacketQueue;
    std::unique_ptr<FrameQueue> video_frameQueue;

    int64_t sample_rate;
    int channels;
    int64_t channel_layout; 
    AVSampleFormat sample_fmt;

    double audioClock;
    double videoClock;

    int last_width;
    int last_height;
    AVPixelFormat last_pix_fmt;

    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* texture;
    SwsContext* sws_ctx;        // 色彩转换上下文
    SwrContext* swr_ctx;        // 重采样上下文
    SDL_AudioDeviceID audio_dev;
    bool audio_enabled;
    int volume;

    AVFrame* yuv_frame;         // 复用的 YUV420P 转换帧，避免每帧分配

    SDL_Thread* demuxThread;
    SDL_Thread* videoThread;
    SDL_Thread* audioThread;

    std::mutex mutex;
    std::mutex format_mutex;
    std::unique_ptr<PCMQueue> pcmQueue;
    AudioBuffer audio_buffer;

    bool pause;
    bool quit;
    // 中止请求：置位后 FFmpeg 中断回调会让阻塞的网络 IO / 打开流程尽快返回
    std::atomic<bool> abort_request;
    // 播放到流末尾（Demux 线程读取结束时置位，用于自动连播）
    std::atomic<bool> eof;
    // 网络异常状态：仅网络源使用，供 UI 展示重连和最终失败状态
    std::atomic<bool> is_network_source;
    std::atomic<bool> network_reconnecting;
    std::atomic<bool> network_failed;
    std::atomic<int> network_error_code;

    VideoState();
    ~VideoState();
};
