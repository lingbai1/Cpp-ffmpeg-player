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

class PacketQueue{
    
    public:
    PacketQueue(int max_packets = 1000, size_t max_bytes = 10 * 1024 * 1024)
        : nb_packets(0), size_bytes(0), duration(0), abort_request(false),
          max_packets(max_packets), max_bytes(max_bytes) 
          {}

    ~PacketQueue()
     { destroy(); }

    bool push_blocking(AVPacket* src_pkt);
     

    AVPacket* pop_for(int ms) ;

    void flush() ;
        

    void abort();

    void destroy() ;

private:
    std::deque<AVPacket*> q;
    int nb_packets;
    int size_bytes;
    int64_t duration;       
    std::mutex mtx;
    std::condition_variable cond;
    bool abort_request;
    int max_packets;
    size_t max_bytes;
};