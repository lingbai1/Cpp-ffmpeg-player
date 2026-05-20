#pragma once
extern "C" {
#include<libavformat/avformat.h>
}
#include<stdint.h>
class FrameItem {
public:
    
    AVFrame* frame;
    int serial;     // 用于标识帧所属的解码周期，帮助丢弃过时帧
    int64_t pts;
    double pts_seconds;
    int64_t duration;
};