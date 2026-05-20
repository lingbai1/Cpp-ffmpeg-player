#pragma once

#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>

class PCMQueue {
public:
    PCMQueue() : abort_request(false)
     {}
    ~PCMQueue() { flush(); }

    void push(std::vector<uint8_t>&& data);

    std::vector<uint8_t> pop_for(int ms);

    void abort();

    void flush();

    size_t size();

private:
    std::deque<std::vector<uint8_t>> q;
    std::mutex mtx;
    std::condition_variable cond;
    bool abort_request;
};