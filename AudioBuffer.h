#pragma once
#include<stdint.h>
class AudioBuffer {
public:
    uint8_t* buffer;
    int size;
    int read_offset;       
    double pts_seconds;
    int64_t duration;
};