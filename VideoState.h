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

    SDL_Thread* demuxThread;
    SDL_Thread* videoThread;
    SDL_Thread* audioThread;

    std::mutex mutex;
    std::mutex format_mutex;
    std::unique_ptr<PCMQueue> pcmQueue;
    AudioBuffer audio_buffer;

    bool pause;
    bool quit;

    VideoState();
    ~VideoState();
};
