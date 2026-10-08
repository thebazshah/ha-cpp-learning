#include "core/ThreadPool.hpp"

#include <exception>

#include "core/Logger.hpp"

namespace cge {

ThreadPool::ThreadPool(std::size_t threadCount, std::string name)
    : name_(std::move(name)), threadCount_(threadCount == 0 ? 1 : threadCount) {
    threads_.reserve(threadCount_);
    for (std::size_t i = 0; i < threadCount_; ++i) {
        threads_.emplace_back([this] { workerLoop(); });
    }
}

ThreadPool::~ThreadPool() { shutdown(); }

bool ThreadPool::submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return false;
        tasks_.push_back(std::move(task));
    }
    wakeUp_.notify_one();
    return true;
}

void ThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wakeUp_.notify_all();
    for (std::thread& thread : threads_) {
        if (thread.joinable()) thread.join();
    }
}

std::size_t ThreadPool::queuedTasks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}

void ThreadPool::workerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // Sleep until there is work to do or the pool is stopping.
            wakeUp_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (tasks_.empty()) return;  // stopping and nothing left to do
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }

        ++busy_;
        try {
            task();
        } catch (const std::exception& error) {
            // A failing task must never take the whole worker thread down.
            LOG_ERROR("pool") << name_ << ": task failed: " << error.what();
        } catch (...) {
            LOG_ERROR("pool") << name_ << ": task failed with an unknown error";
        }
        --busy_;
    }
}

}  // namespace cge
