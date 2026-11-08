#pragma once

#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>

struct PCMItem {
    std::vector<uint8_t> data;
    double pts_seconds;
    bool has_pts;
};

class PCMQueue {
public:
    PCMQueue() : abort_request(false)
     {}
    ~PCMQueue() { flush(); }

    void push(std::vector<uint8_t>&& data, double pts_seconds, bool has_pts);

    PCMItem pop_for(int ms);

    void abort();

    void reset_abort();

    void flush();

    size_t size();

private:
    std::deque<PCMItem> q;
    std::mutex mtx;
    std::condition_variable cond;
    bool abort_request;
};