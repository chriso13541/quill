#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace quill {

// Minimal fixed-size thread pool. Pending tasks are dropped on destruction;
// running tasks are expected to finish quickly (network code polls a cancel flag).
class ThreadPool {
public:
    explicit ThreadPool(int n) {
        if (n < 1) n = 1;
        for (int i = 0; i < n; i++) threads_.emplace_back([this] { run(); });
    }
    ~ThreadPool() {
        decltype(queue_) dropped;
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
            dropped.swap(queue_);
        }
        dropped.clear(); // outside the lock: a dropped job may own things whose destructors take other locks
        cv_.notify_all();
        for (auto& t : threads_) t.join();
    }
    void submit(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (stop_) return;
            queue_.push_back(std::move(fn));
        }
        cv_.notify_one();
    }

private:
    void run() {
        for (;;) {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
                if (stop_) return;
                fn = std::move(queue_.front());
                queue_.pop_front();
            }
            fn();
        }
    }
    std::vector<std::thread>             threads_;
    std::deque<std::function<void()>>    queue_;
    std::mutex                           mu_;
    std::condition_variable              cv_;
    bool                                 stop_ = false;
};

} // namespace quill
