#include "FrameQueue.h"
#include <chrono>

    bool FrameQueue::push_blocking(AVFrame* src) {
        if (!src) return false;
        AVFrame* fcopy = av_frame_alloc();
        if (!fcopy) return false;
        if (av_frame_ref(fcopy, src) < 0) {
            av_frame_free(&fcopy);
            return false;
        }

        std::unique_lock<std::mutex> lk(mutex);
        while (!abort_request && nb_frames >= capacity) {
            cond.wait_for(lk, std::chrono::milliseconds(50));
        }
        if (abort_request) {
            av_frame_free(&fcopy);
            return false;
        }

        FrameItem item;
        item.frame = fcopy;
        item.serial = 0;
        int64_t best_pts = AV_NOPTS_VALUE;
        if (src->best_effort_timestamp != AV_NOPTS_VALUE) best_pts = src->best_effort_timestamp;
        else if (src->pts != AV_NOPTS_VALUE) best_pts = src->pts;
        item.pts = best_pts;
        item.pts_seconds = 0.0;
        item.duration = src->duration > 0 ? src->duration : 0;

        q.push_back(std::move(item));
        nb_frames++;
        cond.notify_one();
        return true;
    }

    FrameItem FrameQueue::pop_for(int ms) {
        std::unique_lock<std::mutex> lk(mutex);
        if (!cond.wait_for(lk, std::chrono::milliseconds(ms), [&]{ return abort_request || nb_frames > 0; })) {
            return FrameItem{nullptr, 0, 0, 0.0, 0};
        }
        if (abort_request && nb_frames == 0) return FrameItem{nullptr, 0, 0, 0.0, 0};
        FrameItem item = std::move(q.front());
        q.pop_front();
        nb_frames--;
        cond.notify_one();
        return item;
    }

    void FrameQueue::flush() 
    {
        std::lock_guard<std::mutex> lk(mutex);
        while (!q.empty()) {
            FrameItem it = std::move(q.front());
            q.pop_front();
            if (it.frame) {
                av_frame_free(&it.frame);
            }
        }
        nb_frames = 0;
        cond.notify_all();
    }

    void FrameQueue::abort() 
    {
        std::lock_guard<std::mutex> lk(mutex);
        abort_request = true;
        cond.notify_all();
    }
    
    void FrameQueue::destroy() 
    { 
        flush(); 
    }

    int FrameQueue::size() {
        std::lock_guard<std::mutex> lk(mutex);
        return nb_frames;
    }
