#pragma once
/**
 * @file ThreadPool.hpp
 * @brief Fixed-size thread pool for dispatching log processing tasks.
 *
 * Uses a standard mutex + condition_variable task queue. This is NOT on
 * the hot ingestion path (the lock-free RingBuffer handles that). The
 * ThreadPool is used for CPU-bound work like regex matching and RAG queries.
 *
 * RAII: destructor signals shutdown and joins all worker threads.
 */

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace logmonitor {

class ThreadPool {
public:
    /**
     * @param num_threads Number of worker threads. Defaults to hardware concurrency - 2
     *                    (reserving threads for the consumer and UI).
     */
    explicit ThreadPool(std::size_t num_threads = 0)
        : stop_(false)
    {
        if (num_threads == 0) {
            auto hw = std::thread::hardware_concurrency();
            num_threads = (hw > 2) ? hw - 2 : 1;
        }

        workers_.reserve(num_threads);
        for (std::size_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~ThreadPool() {
        shutdown();
    }

    // Non-copyable, non-movable
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /**
     * @brief Submit a callable for async execution.
     * @return std::future<ReturnType> to retrieve the result.
     * @throws std::runtime_error if pool has been shut down.
     */
    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using ReturnType = std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );

        std::future<ReturnType> result = task->get_future();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) {
                throw std::runtime_error("ThreadPool: cannot submit after shutdown");
            }
            tasks_.emplace([task]() { (*task)(); });
        }

        condition_.notify_one();
        return result;
    }

    /**
     * @brief Submit a fire-and-forget task (no future returned).
     * Slightly cheaper than submit() when you don't need the result.
     */
    void enqueue(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) return;
            tasks_.emplace(std::move(task));
        }
        condition_.notify_one();
    }

    /// Signal shutdown and join all threads. Safe to call multiple times.
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) return;
            stop_ = true;
        }
        condition_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    [[nodiscard]] std::size_t thread_count() const noexcept {
        return workers_.size();
    }

    [[nodiscard]] std::size_t pending_tasks() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tasks_.size();
    }

private:
    void worker_loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this] {
                    return stop_ || !tasks_.empty();
                });

                if (stop_ && tasks_.empty()) {
                    return;  // Drain remaining tasks before exiting
                }

                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool stop_;
};

} // namespace logmonitor
