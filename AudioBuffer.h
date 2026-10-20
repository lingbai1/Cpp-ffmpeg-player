#pragma once
#include<stdint.h>
class AudioBuffer {
public:
    uint8_t* buffer;
    int size;
    int read_offset;                    //偏移量，表示已经读取了多少数据
    double pts_seconds;                 //音频的pts时间，单位为秒
    int64_t duration;                   //音频的持续时间，单位为微秒
};