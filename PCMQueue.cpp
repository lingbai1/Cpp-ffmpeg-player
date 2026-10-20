#include "PCMQueue.h"
#include <chrono>

void PCMQueue::push(std::vector<uint8_t>&& data, double pts_seconds, bool has_pts) {
    std::lock_guard<std::mutex> lk(mtx);
    if (abort_request) return;
    q.push_back(PCMItem{std::move(data), pts_seconds, has_pts});
    cond.notify_one();
}

PCMItem PCMQueue::pop_for(int ms)
{
    std::unique_lock<std::mutex> lk(mtx);
    if (!cond.wait_for(lk, std::chrono::milliseconds(ms), [&]{ return abort_request || !q.empty(); })) {
        return PCMItem{};
    }
    if (abort_request && q.empty()) return PCMItem{};
    PCMItem item = std::move(q.front());
    q.pop_front();
    return item;
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
