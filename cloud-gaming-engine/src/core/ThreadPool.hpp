#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace cge {

// A fixed group of worker threads that run tasks from a shared queue.
//
// Starting a new thread for every small job is slow, so we start the threads
// once and reuse them. Tasks run in the order they were submitted.
//
//   ThreadPool pool(4, "workers");
//   pool.submit([] { doSomething(); });
//   auto answer = pool.submitWithResult([] { return 42; });
//   int value = answer.get();   // waits until the task has run
class ThreadPool {
public:
    ThreadPool(std::size_t threadCount, std::string name);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Adds a task to the queue. Returns false if the pool is shutting down.
    bool submit(std::function<void()> task);

    // Adds a task and returns a std::future that will hold its result.
    template <typename F>
    auto submitWithResult(F&& function) -> std::future<decltype(function())> {
        using Result = decltype(function());
        auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<F>(function));
        std::future<Result> future = task->get_future();
        if (!submit([task] { (*task)(); })) {
            // The pool is stopping: run the task right here so the future
            // always gets a value and nobody waits forever.
            (*task)();
        }
        return future;
    }

    // Stops accepting new tasks, runs the tasks already queued, then joins
    // all threads. Safe to call more than once.
    void shutdown();

    std::size_t threadCount() const { return threadCount_; }
    std::size_t queuedTasks() const;
    std::size_t busyThreads() const { return busy_.load(); }

private:
    void workerLoop();

    std::string name_;
    std::size_t threadCount_;
    std::vector<std::thread> threads_;
    mutable std::mutex mutex_;
    std::condition_variable wakeUp_;
    std::deque<std::function<void()>> tasks_;
    bool stopping_ = false;
    std::atomic<std::size_t> busy_{0};
};

}  // namespace cge
