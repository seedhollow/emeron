#pragma once

// Threading model for the whole app:
//
//   * The GLFW/ImGui thread owns all UI state and must never block on adb.
//   * ThreadPool runs the blocking adb work.
//   * Dispatcher carries results back; Application drains it once per frame,
//     so panel callbacks run on the UI thread and need no locking.
//
// Panels therefore look like:
//     pool.submit([&]{ auto r = doSlowAdbThing();
//                      dispatcher.post([this, r]{ applyToUi(r); }); });

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace em {

using Task = std::function<void()>;

class ThreadPool {
public:
    explicit ThreadPool(std::size_t workers = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    void submit(Task task);

    // Drops queued-but-not-started work. Running tasks finish normally; they
    // should poll `stopRequested()` for long loops.
    void drainPending();

    [[nodiscard]] std::size_t pendingCount() const;
    [[nodiscard]] std::size_t workerCount() const noexcept { return threads_.size(); }
    [[nodiscard]] bool stopRequested() const noexcept { return stop_.load(std::memory_order_acquire); }

private:
    void workerLoop();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> queue_;
    std::vector<std::thread> threads_;
    std::atomic<bool> stop_{false};
};

// Single-consumer queue of callables drained on the UI thread.
class Dispatcher {
public:
    void post(Task task);

    // Runs everything queued at entry. Tasks posted from within a task are
    // deferred to the next drain, which keeps a frame's work bounded.
    std::size_t drain();

    [[nodiscard]] std::size_t pendingCount() const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::deque<Task> queue_;
};

// Wraps a value that a worker writes and the UI thread reads. Avoids sprinkling
// ad-hoc mutexes across the collectors.
template <typename T>
class Shared {
public:
    Shared() = default;
    explicit Shared(T initial) : value_(std::move(initial)) {}

    void set(T value) {
        const std::lock_guard lock{mutex_};
        value_ = std::move(value);
        ++revision_;
    }

    [[nodiscard]] T get() const {
        const std::lock_guard lock{mutex_};
        return value_;
    }

    // Mutates in place under the lock; for appending to a large container.
    template <typename Fn>
    void mutate(Fn&& fn) {
        const std::lock_guard lock{mutex_};
        std::forward<Fn>(fn)(value_);
        ++revision_;
    }

    template <typename Fn>
    void read(Fn&& fn) const {
        const std::lock_guard lock{mutex_};
        std::forward<Fn>(fn)(value_);
    }

    [[nodiscard]] std::uint64_t revision() const {
        const std::lock_guard lock{mutex_};
        return revision_;
    }

private:
    mutable std::mutex mutex_;
    T value_{};
    std::uint64_t revision_ = 0;
};

}  // namespace em
