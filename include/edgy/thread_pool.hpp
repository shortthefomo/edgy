#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace edgy {

class ThreadPool
{
public:
    explicit ThreadPool(std::size_t n)
    {
        if (n == 0)
            n = 1;
        workers_.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            workers_.emplace_back([this] {
                for (;;)
                {
                    std::function<void()> job;
                    {
                        std::unique_lock lock(mutex_);
                        cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
                        if (stop_ && jobs_.empty())
                            return;
                        job = std::move(jobs_.front());
                        jobs_.pop();
                    }
                    job();
                }
            });
        }
    }

    ~ThreadPool()
    {
        shutdown();
    }

    void
    shutdown()
    {
        {
            std::lock_guard lock(mutex_);
            if (stop_)
            {
                // Already shutting down; still join below if workers remain.
            }
            else
                stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_)
        {
            if (t.joinable())
                t.join();
        }
        workers_.clear();
    }

    ThreadPool(ThreadPool const&) = delete;
    ThreadPool&
    operator=(ThreadPool const&) = delete;

    template <class F>
    auto
    submit(F&& f) -> std::future<std::invoke_result_t<F>>
    {
        using R = std::invoke_result_t<F>;
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
        auto fut = task->get_future();
        {
            std::lock_guard lock(mutex_);
            if (stop_)
                throw std::runtime_error("ThreadPool is stopped");
            pending_.fetch_add(1, std::memory_order_relaxed);
            jobs_.emplace([this, task] {
                (*task)();
                pending_.fetch_sub(1, std::memory_order_relaxed);
            });
        }
        cv_.notify_one();
        return fut;
    }

    [[nodiscard]] std::size_t
    size() const
    {
        return workers_.size();
    }

    [[nodiscard]] std::size_t
    pending() const
    {
        return pending_.load(std::memory_order_relaxed);
    }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> jobs_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_{false};
    std::atomic<std::size_t> pending_{0};
};

}  // namespace edgy
