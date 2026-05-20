#include "PacketQueue.h"
#include <chrono>

    bool PacketQueue::push_blocking(AVPacket* src_pkt)
     {
        std::unique_lock<std::mutex> lk(mtx);
        while (!abort_request && (nb_packets >= max_packets || size_bytes >= (int)max_bytes)) {
            cond.wait_for(lk, std::chrono::milliseconds(50));
        }
        if (abort_request) return false;

        AVPacket* pcopy = av_packet_alloc();
        if (!pcopy) return false;
        if (av_packet_ref(pcopy, src_pkt) < 0) {
            av_packet_free(&pcopy);
            return false;
        }
        q.push_back(pcopy);
        nb_packets++;
        size_bytes += pcopy->size;
        duration += pcopy->duration;
        cond.notify_one();
        return true;
    }

    AVPacket* PacketQueue::pop_for(int ms) {
        std::unique_lock<std::mutex> lk(mtx);
        
        if (!cond.wait_for(lk, std::chrono::milliseconds(ms), [&]{ return abort_request || !q.empty(); })) {
            return nullptr;
        }
        if (abort_request && q.empty()) return nullptr;
        AVPacket* pkt = q.front(); q.pop_front();
        nb_packets--;
        size_bytes -= pkt->size;
        duration -= pkt->duration;
        cond.notify_one();
        return pkt;
    }

    void PacketQueue::flush() {
        std::lock_guard<std::mutex> lk(mtx);
        while (!q.empty()) {
            AVPacket* p = q.front(); q.pop_front();
            av_packet_unref(p);
            av_packet_free(&p);
        }
        nb_packets = 0;
        size_bytes = 0;
        duration = 0;
        cond.notify_all();
    }

    void PacketQueue::abort() {
        std::lock_guard<std::mutex> lk(mtx);
        abort_request = true;
        cond.notify_all();
    }

    void PacketQueue::destroy() { flush(); }

