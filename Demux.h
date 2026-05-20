#pragma once
extern "C" {
#include<libavformat/avformat.h>
#include<SDL2/SDL.h>
}

#include<mutex>
#include<condition_variable>

int Demux_Thread(void *arg);

