#pragma once

#include<deque>
#include<mutex>
#include<condition_variable>
#include<stdint.h>
extern "C" {
    #include<libavformat/avformat.h>
    #include<libavcodec/avcodec.h>
    #include<libavutil/avutil.h>
    #include<libavutil/pixdesc.h>
    #include<SDL2/SDL.h>
    #include<libswscale/swscale.h>
    #include<libswresample/swresample.h>
}
#include"FrameItem.h"
class FrameQueue
{
    public:
    FrameQueue(int capacity = 16) : capacity(capacity), abort_request(false), nb_frames(0) {}
    ~FrameQueue() { destroy(); }

    bool push_blocking(AVFrame* src) ;

    FrameItem pop_for(int ms) ;

    void flush() ;

    void abort() ;

    void destroy();

    int size();

private:
    std::deque<FrameItem> q;
    int capacity;
    bool abort_request;
    std::mutex mutex;
    std::condition_variable cond;
    int nb_frames;
};