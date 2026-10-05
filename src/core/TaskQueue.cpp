#include "core/TaskQueue.h"

#include <algorithm>
#include <utility>

namespace em {

ThreadPool::ThreadPool(std::size_t workers) {
    if (workers == 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        // adb work is I/O bound, so oversubscribing a little is fine, but we
        // cap it: too many concurrent `adb shell` calls starve the daemon.
        workers = std::clamp<std::size_t>(hw == 0 ? 4 : hw / 2, 2, 6);
    }
    threads_.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
        threads_.emplace_back([this] { workerLoop(); });
    }
}

ThreadPool::~ThreadPool() {
    {
        const std::lock_guard lock{mutex_};
        stop_.store(true, std::memory_order_release);
        queue_.clear();
    }
    cv_.notify_all();
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
}

void ThreadPool::submit(Task task) {
    if (!task) return;
    {
        const std::lock_guard lock{mutex_};
        if (stop_.load(std::memory_order_acquire)) return;
        queue_.push_back(std::move(task));
    }
    cv_.notify_one();
}

void ThreadPool::drainPending() {
    const std::lock_guard lock{mutex_};
    queue_.clear();
}

std::size_t ThreadPool::pendingCount() const {
    const std::lock_guard lock{mutex_};
    return queue_.size();
}

void ThreadPool::workerLoop() {
    for (;;) {
        Task task;
        {
            std::unique_lock lock{mutex_};
            cv_.wait(lock, [this] {
                return stop_.load(std::memory_order_acquire) || !queue_.empty();
            });
            if (stop_.load(std::memory_order_acquire)) return;
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        task();
    }
}

void Dispatcher::post(Task task) {
    if (!task) return;
    const std::lock_guard lock{mutex_};
    queue_.push_back(std::move(task));
}

std::size_t Dispatcher::drain() {
    std::deque<Task> batch;
    {
        const std::lock_guard lock{mutex_};
        batch.swap(queue_);
    }
    for (auto& task : batch) task();
    return batch.size();
}

std::size_t Dispatcher::pendingCount() const {
    const std::lock_guard lock{mutex_};
    return queue_.size();
}

void Dispatcher::clear() {
    const std::lock_guard lock{mutex_};
    queue_.clear();
}

}  // namespace em
