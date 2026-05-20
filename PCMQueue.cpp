#include "PCMQueue.h"
#include <chrono>

void PCMQueue::push(std::vector<uint8_t>&& data) {
    std::lock_guard<std::mutex> lk(mtx);
    if (abort_request) return;
    q.push_back(std::move(data));
    cond.notify_one();
}

std::vector<uint8_t> PCMQueue::pop_for(int ms)
{
    std::unique_lock<std::mutex> lk(mtx);
    if (!cond.wait_for(lk, std::chrono::milliseconds(ms), [&]{ return abort_request || !q.empty(); })) {
        return {};
    }
    if (abort_request && q.empty()) return {};
    std::vector<uint8_t> v = std::move(q.front());
    q.pop_front();
    return v;
}

void PCMQueue::abort()
{
    std::lock_guard<std::mutex> lk(mtx);
    abort_request = true;
    cond.notify_all();
}

void PCMQueue::flush()
{
    std::lock_guard<std::mutex> lk(mtx);
    q.clear();
    cond.notify_all();
}

size_t PCMQueue::size()
{
    std::lock_guard<std::mutex> lk(mtx);
    return q.size();
}
