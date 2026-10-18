// BackgroundWorker: one worker thread running queued jobs in order, so slow
// work (decoding images, running ffmpeg, searching a big folder tree) never
// blocks the window. Jobs must not touch UI or GPU state: they compute a
// result into data that outlives the job (shared_ptr) and the UI thread
// picks it up on a later frame.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace cryptvault {

class BackgroundWorker {
public:
    BackgroundWorker() : thread_([this] { Loop(); }) {}
    ~BackgroundWorker() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
            jobs_.clear();
        }
        wake_.notify_all();
        thread_.join();  // waits for the job in flight (they are short and bounded)
    }
    BackgroundWorker(const BackgroundWorker&) = delete;
    BackgroundWorker& operator=(const BackgroundWorker&) = delete;

    void Post(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs_.push_back(std::move(job));
        }
        wake_.notify_one();
    }

    // Drops jobs that haven't started yet (e.g. after navigating to another
    // folder); a job already running finishes normally.
    void CancelPending() {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.clear();
    }

    size_t Pending() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return jobs_.size();
    }

private:
    void Loop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
                if (stop_) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            job();
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> jobs_;
    bool stop_ = false;
    std::thread thread_;  // declared last: starts after everything above is constructed
};

}  // namespace cryptvault
